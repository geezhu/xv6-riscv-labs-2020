#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/riscv.h"
#include "user/user.h"

/* Possible states of a thread: */
#define FREE        0x0
#define RUNNING     0x1
#define RUNNABLE    0x2

#define STACK_SIZE  8192
#define MAX_THREAD  4

struct ut_context{
    uint64 ra;
    uint64 sp;

    // callee-saved
    uint64 s0;
    uint64 s1;
    uint64 s2;
    uint64 s3;
    uint64 s4;
    uint64 s5;
    uint64 s6;
    uint64 s7;
    uint64 s8;
    uint64 s9;
    uint64 s10;
    uint64 s11;
};

char thread_name[][10]={"main","thread_a","thread_b","thread_c","thread_d"};
struct thread {
  char              stack[STACK_SIZE]; /* the thread's stack */
  int               state;             /* FREE, RUNNING, RUNNABLE */
  struct ut_context context;
  int               thread_id;
  uint64            func;
  uint64            arg;
  struct thread*    next;
  struct thread*    prev;
};
struct thread* thread_header=0;
struct thread *current_thread=0;
extern void thread_switch(struct ut_context*, struct ut_context*);
extern void thread_yield_adaper();
uint64      thread_yield(void);
void 
thread_init(void)
{
  // main() is thread 0, which will make the first invocation to
  // thread_schedule().  it needs a stack so that the first thread_switch() can
  // save thread 0's state.  thread_schedule() won't run the main thread ever
  // again, because its state is set to RUNNING, and thread_schedule() selects
  // a RUNNABLE thread.
  current_thread=(struct thread*)malloc(sizeof(struct thread));
  current_thread->state = RUNNING;
  current_thread->thread_id=0;
  current_thread->next=current_thread;
  current_thread->prev=current_thread;
  thread_header =current_thread;
  sigalarm(1,thread_yield_adaper);
}

void 
thread_schedule(void)
{
  struct thread *t, *next_thread;

  /* Find another runnable thread. */
  next_thread = 0;
  t = current_thread->next;
  while (t!=current_thread){
    if(t->state==RUNNABLE){
        next_thread=t;
    }
    t=t->next;
  }

  if (next_thread == 0) {
    printf("thread_schedule: no runnable threads\n");
    exit(-1);
  }

  if (current_thread != next_thread) {         /* switch threads?  */
    next_thread->state = RUNNING;
    t = current_thread;
    current_thread = next_thread;
    //sigret
    sigresume();
    thread_switch(&t->context,&current_thread->context);
  } else
    next_thread = 0;
}
void
thread_join(){
    struct thread *t, *next_thread;

    /* Find another runnable thread. */
    int i=0;
    while (1){
        if(i%500000==0){
//            printf("join=%d\n",i/500000);
        }
        i++;
    };
    do {
        //stop sig
//        int sigalarm(int ticks, void (*handler)());
//        int sigreturn(void);
        sigalarm(0,0);
        next_thread = 0;
        t = current_thread->next;
        while (t!=current_thread){
            if(t->state==RUNNABLE){
                next_thread=t;
            }
            t=t->next;
        }
        sigalarm(10,thread_yield_adaper);
        for (int i = 0; i < 1000*500000; i++) {
            asm volatile("nop"); // avoid compiler optimizing away loop
        }
        //resume sig
    } while (next_thread!=0);
    printf("thread_join: no runnable threads\n");

    if (current_thread == next_thread) {         /* switch threads?  */
        printf("panic: run current thread\n");
    }
    sigalarm(0,0);
}
void
thread_finish(){
    current_thread->state = FREE;
    thread_schedule();
//    sigresume();
    sigreturn();
}
void
thread_adapter(){
//    printf("adapter\n");
    if(current_thread->arg==0){
        ((void (*)())current_thread->func)();
    } else{
        ((void (*)(void*))current_thread->func)((void*)current_thread->arg);
    }
    thread_finish();
}
void 
thread_create(void (*func)())
{
  struct thread *t;

  for (t = thread_header; t->next!=thread_header; t=t->next) {
    if (t->state == FREE) break;
  }
  if(t->state==FREE){
      t->next->prev=t->prev;
      t->prev->next=t->next;
      t->next=thread_header;
      t->prev=thread_header->prev;
      thread_header->prev->next=t;
      thread_header->prev=t;
  }else{
     t->next=(struct thread*)malloc(sizeof(struct thread));
     t->next->thread_id=thread_header->prev->thread_id+1;
     t->next->prev=t;
     t->next->next=thread_header;
     //for new thread
     t=t->next;
     t->next->prev=t;
  }
  t->state = RUNNABLE;
  t->func=(uint64)func;
  t->arg=0;
  t->context.ra=(uint64)(thread_adapter);
  t->context.sp=(uint64)(t->stack+STACK_SIZE);
}

uint64
thread_yield(void)
{
//  printf("alarm!\n");
//  printf("trap thread id=%d\n",current_thread->thread_id);

  uint64 ra=(uint64)sigra();

  current_thread->state = RUNNABLE;
  thread_schedule();
  return ra;

//    sf=(struct stackframe*)(r_fp()-16);
//    printf("change ra=%p\n",sf->ra);
}

volatile int a_started, b_started, c_started,d_started;
volatile int a_n, b_n, c_n,d_n;

void
thread_a(void)
{
  int i;
  printf("thread_a started\n");
  a_started = 1;
  while(b_started == 0 || c_started == 0|| d_started == 0);
  
  for (i = 0; i < 100*500000; i++) {
      if(i%100000==0){
          printf("thread_a %d\n", i/100000);
      }
//    printf("thread_a %d\n", i);
    a_n += 1;
  }
  printf("thread_a: exit after %d\n", a_n);

}

void 
thread_b(void)
{
  int i;
  printf("thread_b started\n");
  b_started = 1;
  while(a_started == 0 || c_started == 0|| d_started == 0);
  
  for (i = 0; i < 100*500000; i++) {
      if(i%100000==0){
          printf("thread_b %d\n", i/100000);
      }
    b_n += 1;
  }
  printf("thread_b: exit after %d\n", b_n);

}

void 
thread_c(void)
{
  int i;
  printf("thread_c started\n");
  c_started = 1;
  while(a_started == 0 || b_started == 0|| d_started == 0);
  
  for (i = 0; i < 100*500000; i++) {
      if(i%100000==0){
          printf("thread_c %d\n", i/100000);
      }
//    printf("thread_c %d\n", i);
    c_n += 1;
  }
  printf("thread_c: exit after %d\n", c_n);
}
void
thread_d(void)
{
  int i;
  printf("thread_d started\n");
  d_started = 1;
  while(a_started == 0 || b_started == 0|| c_started == 0);

  for (i = 0; i < 100*500000; i++) {
      if(i%100000==0){
          printf("thread_d %d\n", i/100000);
      }
//    printf("thread_d %d\n", i);
    d_n += 1;
  }
  printf("thread_d: exit after %d\n", d_n);
}

int 
main(int argc, char *argv[]) 
{
  a_started = b_started = c_started = 0;
  a_n = b_n = c_n = 0;
  printf("init\n");
  thread_init();
  thread_create(thread_a);
  thread_create(thread_b);
  thread_create(thread_c);
  thread_create(thread_d);
  printf("td\n");
  thread_join();
  exit(0);
}
