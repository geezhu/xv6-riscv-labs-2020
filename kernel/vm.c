#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"
#include "fcntl.h"
#include "sleeplock.h"
#include "file.h"
/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

void        freewalk(pagetable_t pagetable);
static pte_t *     walk(pagetable_t pagetable, uint64 va, int alloc);
void        pte_parser(pte_t pte);
/*
 * create a direct-map page table of process for the kernel.
 */
void
proc_kvmmap(struct proc* proc,uint64 va, uint64 pa, uint64 sz, int perm)
{
    if(proc==nullptr){
        if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
            panic("kvmmap");
    } else{
        if(mappages(proc->kernel_pagetable, va, sz, pa, perm) != 0)
            panic("kvmmap");
    }

}
// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freekpagetable(pagetable_t kernel_pagetable)
{


    // unmap uart registers
    uvmunmap(kernel_pagetable, UART0, 1, 0);

    // unmap virtio mmio disk interface
    uvmunmap(kernel_pagetable, VIRTIO0, 1, 0);


    // unmap PLIC
    uvmunmap(kernel_pagetable,PLIC, PGROUNDUP(0x400000)/PGSIZE, 0);

    // unmap kernel text executable and read-only.
    uvmunmap(kernel_pagetable,KERNBASE, PGROUNDUP((uint64)etext-KERNBASE)/PGSIZE, 0);

    // unmap kernel data and the physical RAM we'll make use of.
    uvmunmap(kernel_pagetable,(uint64)etext, PGROUNDUP(PHYSTOP-(uint64)etext)/PGSIZE, 0);

    // unmap the trampoline for trap entry/exit to
    // the highest virtual address in the kernel.
    uvmunmap(kernel_pagetable,TRAMPOLINE, 1, 0);

    uvmunmap(kernel_pagetable, KSTACK(0), 1, 1);

    //free the pagetable
    freewalk(kernel_pagetable);
}

void
proc_kvminit(struct proc* proc)
{
    if(proc==nullptr){
        kernel_pagetable = (pagetable_t) kalloc();
        memset(kernel_pagetable, 0, PGSIZE);
    } else{
        proc->kernel_pagetable=(pagetable_t) kalloc();
        memset(proc->kernel_pagetable, 0, PGSIZE);
    }

    // uart registers
    proc_kvmmap(proc,UART0, UART0, PGSIZE, PTE_R | PTE_W);

    // virtio mmio disk interface
    proc_kvmmap(proc,VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

    // CLINT
    if(proc==nullptr){
        proc_kvmmap(proc,CLINT, CLINT, 0x10000, PTE_R | PTE_W);
    }

    // PLIC
    proc_kvmmap(proc,PLIC, PLIC, 0x400000, PTE_R | PTE_W);

    // map kernel text executable and read-only.
    proc_kvmmap(proc,KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

    // map kernel data and the physical RAM we'll make use of.
    proc_kvmmap(proc,(uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

    // map the trampoline for trap entry/exit to
    // the highest virtual address in the kernel.
    proc_kvmmap(proc,TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}
// Allocate a page for the process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_kstackinit(struct proc* p)
{
    if(p==nullptr){
        panic("proc_kstackinit");
    }
    char *pa = kalloc();
    if(pa == 0)
        panic("kalloc");
    uint64 va = KSTACK(0);
    proc_kvmmap(p,va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    p->kstack = va;
}
// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
proc_kvminithart(struct proc* p)
{
    if(p!=nullptr){
        w_satp(MAKE_SATP(p->kernel_pagetable));
        sfence_vma();
    } else{
        w_satp(MAKE_SATP(kernel_pagetable));
        sfence_vma();
    }

}
//map user space to p->kernel_pagetable if newsz>oldsz
//unmap p->kernel_pagetable if newsz<oldsz
void
proc_usermapping(struct proc* p, uint64 oldsz,uint64 newsz)
{
    if(newsz> PGROUNDUP(p->sz)){
        panic("proc_usermapping");
    };
    if(newsz>=p->vma_bound||oldsz>=p->vma_bound){
        printf("warning mmap crash with pagetable mapping");
        return;
    }
    if(newsz>PLIC){
        newsz=PLIC;
    };
    if(oldsz>PLIC){
        oldsz=PLIC;
    }
    if(oldsz>newsz){
        uint64 npages= (PGROUNDUP(oldsz)- PGROUNDUP(newsz))/PGSIZE;
        uvmunmap(p->kernel_pagetable, PGROUNDUP(newsz),npages,0);
    } else if(oldsz<newsz){
        for (uint64 va = PGROUNDUP(oldsz),desva= PGROUNDUP(newsz); va < desva; va+=PGSIZE) {
            pte_t* pte=walk(p->pagetable,va,0);
            if(pte==0||!(PTE_FLAGS(*pte)&PTE_V)){
                continue;
            }
            if(mappages(p->kernel_pagetable, va, PGSIZE, PTE2PA(*pte), PTE_FLAGS(*pte)&~PTE_U) != 0)
                panic("proc_usermapping: mappages");
        }
    }
}
/*
 * create a direct-map page table for the kernel.
 */
void
kvminit()
{
  proc_kvminit(nullptr);
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  proc_kvminithart(nullptr);
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
static pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}
uint64
proc_kvmpa(struct proc*p,uint64 va)
{
    uint64 off = va % PGSIZE;
    pte_t *pte;
    uint64 pa;
    if(p){
        pte = walk(p->kernel_pagetable, va, 0);
    } else{
        pte = walk(kernel_pagetable,va,0);
    }
    if(pte == 0)
        panic("proc_kvmpa");
    if((*pte & PTE_V) == 0)
        panic("proc_kvmpa");
    pa = PTE2PA(*pte);
    return pa+off;
}
// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(uint64 va)
{
  return proc_kvmpa(nullptr,va);
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0){
        continue;
    }
//      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0){
//        printf("uvmunmap: not mapped\n");
        continue;
    }
//      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
//        pte_parser(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;
  uint64 flags;
  pte_t* pte;
  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    pte= walk(pagetable,a,0);
    if(pte!=0&& IS_COW(*pte)){
//        mem=kalloc();
        flags=COW_WFLAGS(*pte);
        memmove(mem,(char *)PTE2PA(*pte),PGSIZE);
        uvmunmap(pagetable,a,1,1);
        proc_usermapping(myproc(),a+PGSIZE,a);
        if(mappages(pagetable, a, PGSIZE, (uint64)mem, flags) != 0){
            kfree(mem);
            return 0;
        }
    }else{
        memset(mem, 0, PGSIZE);
        if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
            kfree(mem);
            uvmdealloc(pagetable, a, oldsz);
            return 0;
        }
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
    return copy(old,new,0,sz);
}
// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
copy(pagetable_t old, pagetable_t new, uint64 begin,uint64 copy_end)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = begin; i < copy_end; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0){
        continue;
    }
//      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0){
//        printf("uvmcopy: page not present\n");
        continue;
    }
//      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    if(i==myproc()->ustack){
        //not to copy ustack
        flags = PTE_FLAGS(*pte);
        if((mem = kalloc()) == 0)
            goto err;
        memmove(mem, (char*)pa, PGSIZE);
        if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
            kfree(mem);
            goto err;
        }
        continue;
    }

    *pte= PA2PTE(pa)| COW_FLAGS(*pte);
    if(mappages(new, i, PGSIZE, (uint64)pa, COW_FLAGS(*pte)) != 0){
//      kfree(mem);
      goto err;
    }
    kreflock((void *)pa);
    inc_refcount(pa);
    krefunlock((void *)pa);
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;
  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(va0<MAXVA){
        pte=  walk(pagetable,va0,0);
    } else{
        return -1;
    }
    if(pa0 == 0 || IS_COW(*pte)){
        struct proc* p=myproc();
        uint64 va= PGROUNDDOWN(va0);
        //don't use PGROUNDUP ,it's possible that will equal to PGROUNDDOWN
        //use PGROUNDDOWN+PGSIZE instead
        if(va==p->ustack){
            printf("[%d]ustack_pf\n",p->pid);
            pte_parser(*pte);
        }
        int ret= page_fault_handler(p,va);
        if(ret==0){
            pa0 = walkaddr(pagetable, va0);
        } else{
            return -1;
        }
//        if(va<p->sz && va!=(p->ustack-PGSIZE)){
//            if(uvmalloc(p->pagetable, va, va+PGSIZE)!=0){
//                proc_usermapping(p,va, va+PGSIZE);
//                pa0 = walkaddr(pagetable, va0);
//            } else{
//                p->killed=1;
//                return -1;
//            }
//        } else{
//            return -1;
//        }
//        pa0= walkaddr(pagetable,va0);
    }
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
    if(srcva<PLIC){
        if(srcva+len<=PLIC){
            return copyin_new(pagetable,dst,srcva,len);
        } else{
            uint64 copy_len=PLIC-srcva;
            if(copyin_new(pagetable,dst,srcva,copy_len)==-1){
                return -1;
            } else{
                dst+=copy_len;
                len-=copy_len;
                srcva=PLIC;
            }
        }
    }
    uint64 n, va0, pa0;

    while(len > 0){
        va0 = PGROUNDDOWN(srcva);
        pa0 = walkaddr(pagetable, va0);
        if(pa0 == 0){
            struct proc* p=myproc();
            uint64 va= PGROUNDDOWN(va0);
            //don't use PGROUNDUP ,it's possible that will equal to PGROUNDDOWN
            //use PGROUNDDOWN+PGSIZE instead
            int ret= page_fault_handler(p,va);
            if(ret==0){
                pa0 = walkaddr(pagetable, va0);
            } else{
                return -1;
            }
//            if(va<p->sz && va!=(p->ustack-PGSIZE)){
//                if(uvmalloc(p->pagetable, va, va+PGSIZE)!=0){
//                    proc_usermapping(p,va, va+PGSIZE);
//                    pa0 = walkaddr(pagetable, va0);
//                } else{
//                    p->killed=1;
//                    return -1;
//                }
//            } else{
//                return -1;
//            }
        }
        n = PGSIZE - (srcva - va0);
        if(n > len)
            n = len;
        memmove(dst, (void *)(pa0 + (srcva - va0)), n);

        len -= n;
        dst += n;
        srcva = va0 + PGSIZE;
    }
    return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
    if(srcva<PLIC){
        if(srcva+max<=PLIC){
            int ret= copyinstr_new(pagetable,dst,srcva,max);
            if(ret==1){
                return -1;
            }
            return ret;
        } else{
            uint64 copy_len=PLIC-srcva;
            int ret;
            if((ret=copyinstr_new(pagetable,dst,srcva,copy_len))==-1){
                return -1;
            } else if (ret==1){
                dst+=copy_len;
                max-=copy_len;
                srcva=PLIC;
            } else{
                return 0;
            }
        }
    }
    uint64 n, va0, pa0;
    int got_null = 0;

    while(got_null == 0 && max > 0){
        va0 = PGROUNDDOWN(srcva);
        pa0 = walkaddr(pagetable, va0);
        if(pa0 == 0){
            struct proc* p=myproc();
            uint64 va= PGROUNDDOWN(va0);
            //don't use PGROUNDUP ,it's possible that will equal to PGROUNDDOWN
            //use PGROUNDDOWN+PGSIZE instead
            int ret= page_fault_handler(p,va);
            if(ret==0){
                pa0 = walkaddr(pagetable, va0);
            } else{
                return -1;
            }
//            if(va<p->sz && va!=(p->ustack-PGSIZE)){
//                if(uvmalloc(p->pagetable, va, va+PGSIZE)!=0){
//                    proc_usermapping(p,va, va+PGSIZE);
//                    pa0 = walkaddr(pagetable, va0);
//                } else{
//                    p->killed=1;
//                    return -1;
//                }
//            } else{
//                return -1;
//            }
        }
        n = PGSIZE - (srcva - va0);
        if(n > max)
            n = max;

        char *p = (char *) (pa0 + (srcva - va0));
        while(n > 0){
            if(*p == '\0'){
                *dst = '\0';
                got_null = 1;
                break;
            } else {
                *dst = *p;
            }
            --n;
            --max;
            p++;
            dst++;
        }

        srcva = va0 + PGSIZE;
    }
    if(got_null){
        return 0;
    } else {
        return -1;
    }
}
void vmprint_impl(pagetable_t pagetable,int level){
    if(level == 2){
        printf("page table %p\n",pagetable);
    }
    for(int i = 0; i < 512; i++){
        pte_t pte = pagetable[i];
        if((pte & PTE_V)){
            // this PTE points to a lower-level page table.
            uint64 child = PTE2PA(pte);
            for(int j=2;j>=level;j--){
                printf("..");
                if(j == level){
                    printf("%d: ",i);
                } else{
                    printf(" ");
                }
            }
            printf("pte %p pa %p\n",pte,child);
            if(level != 0){
                vmprint_impl((pagetable_t)child,level-1);
            }
        }
    }
}
void pte_parser(pte_t pte){
    uint64 pa=PTE2PA(pte);
    uint64 flags= PTE_FLAGS(pte);
#define b(x) (!(!(x)))
    printf("PTE=(PA=%p,V=%d,U=%d,R=%d,W=%d,X=%d,C=%d)\n",pa,  b(flags&PTE_V),   b(flags&PTE_U),  b(flags&PTE_R), b(flags&PTE_W), b(flags&PTE_X),
           b(flags&PTE_C));
#undef b
}
int in_Interval(uint64 target,struct virtual_memory_area* vma){
    return target>=vma->vm_start&&target<vma->vm_end;
}
//-1 for err other for index
int mmap_valid(struct proc* p,uint64 va){
    if(va<p->vma_bound||va>=TRAPFRAME){
       return -1;
    }
    int i;
    for (i = 0; i < NVMA; ++i) {
        if(in_Interval(va,&p->vma[i])){
            i=-i;
            break;
        }
        if(!p->vma[i].valid){
            break;
        }
    }
    if(i>0){
        return -1;
    }
    if(i<0){
        return -i;
    }
    if(i==0&&p->vma[0].valid&& in_Interval(va,&p->vma[0])){
        return 0;
    } else{
        return -1;
    }
//    if(i==0){
//        return -1;
//    }
//  p->vma[i-1].vm_start<=va;
//  it's ok that vm_end>=va&&va+PGSIZE>vm_end;
//    if(p->vma[i-1].vm_end<=va){
//        return -1;
//    }
//    return i-1;
};
int load_vma(struct proc* p,uint64 va,int index){
#define min(a,b) ((a<b)?(a):(b))
    struct file* f=0;
    struct virtual_memory_area* vma=&p->vma[index];
    uint32 offset=va-vma->vm_start+vma->offset;
    uint len=vma->vm_end-va;
    len=min(len,PGSIZE);
    if(offset<vma->offset){
        printf("load_vma\n");
        return -1;
    }
    f=vma->file;
    ilock(f->ip);
    if(readi(f->ip, 1, va, offset, len) <= 0){
        printf("load_vma\n");
        return -1;
    }
    iunlock(f->ip);
    //reset flags
    pte_t *pte;
    if((pte = walk(p->pagetable, va, 0)) == 0)
        return -1;
//    pte_parser(*pte);
    if(!(*pte & PTE_V))
        panic("unmapped");
    int perm=0;
//    printf("begin pte\n");
//    pte_parser(*pte);
    uint64 pa= PTE2PA(*pte);

    if(vma->vm_prot&PROT_READ){
        perm|=PTE_R;
    }
    if(vma->vm_prot&PROT_WRITE){
        perm|=PTE_W;
    }
    if(vma->vm_prot&PROT_EXEC){
        perm|=PTE_X;
    }
    *pte = PA2PTE(pa) | perm | PTE_V | PTE_U;
//    pte_parser(*pte);
    return 0;
};
int copy_vma(struct proc* p,struct proc* np){
    //copy
    for (int i = 0; i < NVMA && p->vma[i].valid; ++i) {
        np->vma[i]=p->vma[i];
        np->vma[i].file=filedup(p->vma[i].file);
    }
    np->vma_bound=p->vma_bound;
    return copy(p->pagetable,np->pagetable,p->vma_bound,TRAPFRAME);
}
void unmap_all_vma(struct proc* p){
//    printf("call uva,pid=%d\n",p->pid);
    struct virtual_memory_area* vma=p->vma;
//    uvmunmap(p->pagetable,p->vma_bound,PGROUNDUP(TRAPFRAME-p->vma_bound)/PGSIZE,1);
    while (vma->valid){
        unmap_vma(p,vma->vm_start,vma->vm_end,0);
    }
}
void vma_swap(struct virtual_memory_area* v1,struct virtual_memory_area* v2){
    if(v1==0||v2==0){
        panic("null swap");
    }
    struct virtual_memory_area tmp;
    tmp=*v1;
    *v1=*v2;
    *v2=tmp;
}
int unmap_vma(struct proc* p,uint64 begin,uint64 end,int vma_index){
    //copy
    //write MAP_SHARED to file
//    printf("unmap[%p,%p]---\n",begin,end);
//    for (int i = 0; i < NVMA; ++i) {
//        if(!p->vma[i].valid){
//            break;
//        }
//        printf("---[%p,%p]\n",p->vma[i].vm_start,p->vma[i].vm_end);
//    }
    struct virtual_memory_area* vma=&p->vma[vma_index];
    if(vma_index==-1){
        return -1;
    }
    if(vma->vm_start>begin||vma->vm_end<end||end<begin){
        return -1;
    }
    if(vma->vm_start!=begin&&vma->vm_end!=end){
        return -1;
    }
//    printf("vma---[%p,%p]\n",vma->vm_start,vma->vm_end);
    uint32 offset=begin-vma->vm_start+vma->offset;
    if(vma->vm_start==begin){
        vma->vm_start=end;
    } else{
        vma->vm_end=begin;
    }
//    printf("vma---[%p,%p]\n",vma->vm_start,vma->vm_end);
//    printf("unmap [%p,%p]\n",begin,end);
//    size_t len=end-begin;
    struct file* f=vma->file;
    uint64 va= PGROUNDDOWN(begin);
    pte_t *pte;
    uint32 writelen=0;
    uint64 pagebound=0;
//    printf("va=%p\n",va);
//    printf("end=%p\n",end);
    for (uint64 i = va; i < end; i+=writelen,offset+=writelen) {
        pagebound= min(PGROUNDUP(i+1),end);
//        printf("pgbound=%p\n",pagebound);
        writelen= pagebound-i;
//        printf("wlen=%d\n",writelen);
        pte= walk(p->pagetable,i,0);
        if(pte==0||!(*pte&PTE_V)){
//            printf("begin=%p,end=%p\n",begin,end);
//            printf("unmap[%p],PGSIZE=1,not exist,pid=%d\n", PGROUNDDOWN(i),p->pid);
            continue;
        }
        if(vma->vm_flag&MAP_SHARED&&(*pte&PTE_D)){
            begin_op();
            ilock(f->ip);
            if (writei(f->ip,1,i,offset,writelen)<0){
                iunlock(f->ip);
                end_op();
                return -1;
            }
            iunlock(f->ip);
            end_op();
        }
        if(i< PGROUNDDOWN(vma->vm_start)||i> PGROUNDUP(vma->vm_end)){
//            printf("unmap[%p],PGSIZE=1,pid=%d\n", PGROUNDDOWN(i),p->pid);
            uvmunmap(p->pagetable, PGROUNDDOWN(i), 1, 1);
        }
    }
    if(vma->vm_start==vma->vm_end){

        if(vma->vm_start> PGROUNDDOWN(vma->vm_start)){
//            printf("unmap[%p] eq,PGSIZE=1,pid=%d\n", PGROUNDDOWN(vma->vm_start),p->pid);
            uvmunmap(p->pagetable, PGROUNDDOWN(vma->vm_start), 1, 1);
        }
        fileclose(vma->file);
        vma->valid=0;
        int i=vma_index+1;
        while (i!=NVMA&&vma[i].valid){
            vma_swap(&p->vma[i-1],&p->vma[i]);
            i++;
        }
    }
    int i;
    for (i = 0; i < NVMA ; ++i) {
        if(!p->vma[i].valid){
            i=-i;
            break;
        }
    }
    if(i<0){
        i=-i;
        i-=1;
        if(p->vma[i].valid){
            p->vma_bound=p->vma[i].vm_start;
        } else{
            panic("unmap_vma\n");
        }
    } else if(i>0){
        if(i!=NVMA){
            panic("unmap_vma\n");
        }
        p->vma_bound=p->vma[i].vm_start;
    } else{
        p->vma_bound=TRAPFRAME;
    }
//    printf("after unmap----\n");
    for (int i = 0; i < NVMA; ++i) {
        if(!p->vma[i].valid){
            break;
        }
//        printf("---[%p,%p]\n",p->vma[i].vm_start,p->vma[i].vm_end);
    }
//    printf("unb=%p",p->vma_bound);
    return 0;
}


int map_vma(struct proc* p,uint64 begin,uint64 end, int prot, int flags,
             struct file* f, uint32 offset){
    //add to p->vma[]
    //set valid
//    printf("map[%p,%p]\n", begin,end);
    struct virtual_memory_area* vma=p->vma;
    int i;
    for (i = 0; i < NVMA; ++i) {
        if(!vma[i].valid){
            break;
        }
    }
    int j;
    for (j = 0;  j< i; ++j) {
        if(in_Interval(begin,&vma[j])|| in_Interval(end-1,&vma[j])){
            return -1;
        }
    }
    if(i==NVMA){
        return -1;
    }
    if(begin>=end){
        return -1;
    }
    vma[i].vm_start=begin;
    vma[i].vm_end=end;
    vma[i].vm_prot=prot;
    vma[i].vm_flag=flags;
    vma[i].file= filedup(f);
    vma[i].offset=offset;
    vma[i].valid=1;
    while (i>0){
        if(vma[i].vm_start>vma[i-1].vm_start){
            vma_swap(&vma[i],&vma[i-1]);
        } else{
            break;
        }
        i--;
    }
    for (i = 0; i < NVMA ; ++i) {
        if(!vma[i].valid){
            i=-i;
            break;
        }
    }
    if(i<0){
        i=-i;
        i-=1;
        if(vma[i].valid){
            p->vma_bound=vma[i].vm_start;
        } else{
            panic("unmap_vma\n");
        }
    } else if(i>0){
        if(i!=NVMA){
            panic("unmap_vma\n");
        }
        p->vma_bound=vma[i].vm_start;
    } else{
        p->vma_bound=TRAPFRAME;
    }
//    printf("unb=%p",p->vma_bound);
    return 0;
}
//#define TEST_PFH
int page_fault_handler(struct proc* p,uint64 va){
#define lazy_valid(va) (va<p->sz)
#define not_stack(va) va!=(p->ustack-PGSIZE)
    int mmap_index= mmap_valid(p,va);
#ifdef TEST_PFH
    mmap_index=-1;
#endif
    if((lazy_valid(va)|| mmap_index!=-1) && not_stack(va)){
        if(uvmalloc(p->pagetable, va, va+PGSIZE)!=0){
            if(lazy_valid(va)){
                proc_usermapping(p,va, va+PGSIZE);
            } else{
//                printf("mmapindex!=-1\n");
                if(load_vma(p,va,mmap_index)==-1){
//                    printf("pfh vma ret 1\n");
                    p->killed=1;
                    return 1;
                }
            }
            return 0;
        } else{
//            printf("pfh uvmalloc ret 1\n");
            p->killed=1;
            return 1;
        }
    } else{
//        printf("pfh invalid ret 1\n");
        return -1;
    }
}