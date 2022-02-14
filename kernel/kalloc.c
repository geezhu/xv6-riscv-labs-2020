// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock[NCPU];
  struct spinlock reflock[NCPU];
  struct run *freelist[NCPU];
} kmem;
#define cpu_map(addr) (((uint64)addr/PGSIZE)%NCPU)
//#define cpu_map(addr) 0
void
kreflock(void* pa){
    acquire(&kmem.reflock[cpu_map(pa)]);
}
void
krefunlock(void* pa){
    release(&kmem.reflock[cpu_map(pa)]);
}
void
kinit()
{
  for (int i = 0; i < NCPU; ++i) {
      initlock(&kmem.lock[i], "kmem");
      initlock(&kmem.reflock[i], "kmem.refcount");
  }
  char *ptr=end+PGCOUNT;
  while (ptr!=end){
      kreflock(ptr);
      *ptr=1;
      krefunlock(ptr);
      ptr--;
  }
  freerange(end+PGCOUNT, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
//  printf("kfree %p\n",pa);
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");
  kreflock(pa);
  dec_refcount(pa);
  if(refcount(pa)!=0){
      krefunlock(pa);
      return;
  }
  krefunlock(pa);
  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
//  printf("cpuid=%d\n", cpu_map(pa));
  acquire(&kmem.lock[cpu_map(pa)]);
  r->next = kmem.freelist[cpu_map(pa)];
  kmem.freelist[cpu_map(pa)] = r;
  release(&kmem.lock[cpu_map(pa)]);
}
void * kget(int cpuid){
    struct run*r=0;
    acquire(&kmem.lock[cpuid]);
    r = kmem.freelist[cpuid];
    if(r)
        kmem.freelist[cpuid] = r->next;
    release(&kmem.lock[cpuid]);
    return r;
}
void *
ksteal(int cpuid){
    struct run *r=0;
    int i=cpuid+1;
    for (; i !=cpuid ; ++i) {
        if(i==NCPU){
//            printf("reset to zero");
            i=0;
            if(i==cpuid){
                break;
            }
        }
        r= kget(i);
        if(r){
//            printf("steal from cpu %d\n",i);
            break;
        }
    }
//    if(i==cpuid){
//        printf("out of mem\n");
//    }
    return r;
}
// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  push_off();
  uint64 id=cpuid();
  r= kget(id);
  if(!r)
      r= ksteal(id);
  pop_off();
  kreflock(r);
  if(r)
      inc_refcount(r);
  krefunlock(r);
  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
