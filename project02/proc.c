#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include <stddef.h>

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

struct queue{
  struct proc *head;
  struct proc *tail;
  struct spinlock lock;
  int time_quantum;		// time quantum
  int level;			// level of queue
};

struct monopolyqueue{
  struct proc *head;
  struct proc *tail;
  struct spinlock lock;
  int size;
};

struct queue mlfq[4];
struct monopolyqueue moq;

int my_password = 2022066953;

int Mono;
int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void enqueue(struct queue *q, struct proc *p);
void monodequeue(struct proc *target);

uint global_ticks=0;


//init_queue
void 
init_queue()
{

  for(int i=0; i<4; i++){
    //mlfq[i].head = mlfq[i].tail = 0;
    //mlfq[i].time_quantum = 2*i+2;
    mlfq[i].level =i;
    mlfq[i].time_quantum = 2 * i + 2;
    mlfq[i].head =NULL;
  }
}


//init_monoqueue
void
init_monoqueue()
{
  moq.head = NULL;
  moq.size = 0;
}

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
  acquire(&ptable.lock);
  init_queue();
  init_monoqueue();
  release(&ptable.lock);
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}


//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.


static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  p->level_of_queue = 0; // initialize the queue level to 0
  p->time_quantum = 0; // initialize the time quantum to 0

  enqueue(&mlfq[0], p);

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    acquire(&ptable.lock);
    p->state = UNUSED;
    release(&ptable.lock);
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  
  sched();
  
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}


//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.


// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  struct proc *p = myproc();
  if(p==0){
    cprintf("yield failed\n");
  }
  p->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == SLEEPING && p->chan == chan){
      p->state = RUNNABLE;
    }
  }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
        
      cprintf("Process %d has been killed!\n", pid);
      
      if(p->monopolized ==1){
        monodequeue(p);
        p->monopolized = 0;
        
        if(moq.head == NULL){
          unmonopolize();
        }
      }
      
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

/*
//priority_boosting
void
priority_boosting()
{
  struct proc *p;
  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    
    if(p->state == UNUSED) continue;
    
    if(p->state == RUNNABLE || p->state == SLEEPING){
      p->level_of_queue =0;
      p->ticks = 0;  
    }
  }
  release(&ptable.lock);
}*/


//enqueue
void
enqueue(struct queue *q, struct proc *p)
{
  struct proc *last = q->head;
  
  if(last){
  
    while(last->next) last = last->next; 	// find last node
    last->next = p; 	//enqueue new proecess
  }
  
  else{
    q->head = p;
  }
  p->next = NULL;
}


//dequeue
void
dequeue(struct queue *q, struct proc *p)
{
    if (q->head == NULL) return; // empty queue

    acquire(&q->lock);

    if (q->head == p) { // p is head
        q->head = p->next;
        
        if (q->head == NULL) {
            q->tail = NULL; // if queue is empty, set tail to empty
        }
    } else { // p is not head
    
        struct proc *current = q->head;
       
        while (current->next != p) {
        
            if (current->next == NULL) {
                release(&q->lock); // if p is not in q, return;
                return;
            }
            current = current->next;
        }
        current->next = p->next;
        
        if (current->next == NULL) {
            q->tail = current; // dequeue last element
        }
    }
   
    p->next = NULL;
    release(&q->lock); 
}


//monoenqueue
void
monoenqueue(struct proc *p)
{
  p->monopolized =1;
  struct proc *last = moq.head;
  
  if(last){
    
    while(last->next) last = last->next; 	// find last node
    last->next = p; 	//enqueue new proecess
  }
  
  else{
    moq.head = p;
    moq.tail =p;
  }
  p->next = NULL;
}


// monodequeue
void 
monodequeue(struct proc *target) 
{
    if (moq.head == NULL) return;

    acquire(&moq.lock);

    if (target == moq.head) {
      moq.head = target->next;
      
      if (moq.head == NULL) {
        moq.tail = NULL;
      }
    } 
    
    else {
      struct proc *prev = moq.head;
        
      while (prev->next != target) {
        
        if (prev->next == NULL) {
          cprintf("Error! This process is not in moq.\n");
          release(&moq.lock);
          return;
        }
        
        prev = prev->next;
      }
      
      prev->next = target->next;
        
      if (prev->next == NULL) {
        moq.tail = prev;
      }
    }
    
    target->monopolized = 0;
    target->next = NULL;
    release(&moq.lock);
}


// getlevel
int
getlev(void)
{
  struct proc *p;
  struct proc *current = myproc();
  
  for(p = moq.head; p!= NULL; p = p->next){
  
    if(p == current){
      return 99;
    }
  }
  return current->level_of_queue;
}


// setpriority
int
setpriority(int pid, int priority)
{
  struct proc *p;
  
  if(priority < 0 || priority >10) return -2; //wrong priority
  acquire(&ptable.lock);
  
  //struct proc *process;
  int found =0;
  
  for(p = ptable.proc; p<&ptable.proc[NPROC]; p++){
  
    if(p->pid == pid){
      found = 1;
      p->priority = priority;
      
      //cprintf("priority: %d, id: %d\n", p->priority, p->pid);
      break;
    }
  }
  
  release(&ptable.lock);
  
  return found ? 0 : -1;
      
}

 
// setmonopoly
int
setmonopoly(int pid, int password)
{
  //cprintf("pid: %d\n", pid);
  if(password != my_password) return -2;
 
  acquire(&ptable.lock);
  struct proc *p = NULL;
  
  int found = 0;
  
  for(p = ptable.proc; p<&ptable.proc[NPROC]; p++){
  
    if(p->pid == pid){
      found = 1;
      break;
    }
  }
  
  if(found == 0) {
    cprintf("Error! Invalid process.\n");
    release(&ptable.lock);
    return -1;
  }
  
  else if(found ==1){
  
    dequeue(&mlfq[p->level_of_queue], p);
    monoenqueue(p);
    
    if(p->state == RUNNABLE) moq.size++;
    
	
    p->monopolized = 1;
    p->level_of_queue = 99;
    release(&ptable.lock);
  }
  return moq.size;
}


//monopolize
void
monopolize()
{
  Mono=1; 
}
  
  
//unmonopolize
void
unmonopolize()
{
  struct proc *p;
  acquire(&moq.lock);
  
  for(p = moq.head; p!= NULL; p= p->next){
  
    if(p->state == ZOMBIE && p->monopolized ==1) {
      monodequeue(p);
    }    
  }
  //cprintf("no!\n");
  release(&moq.lock);

  global_ticks = 0;
  Mono = 0;
}


void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  //init_queue();
  //init_monoqueue();
  for(;;) {
    
    if (Mono == 1) {    

      struct proc *inmoq = NULL;
    
      for (inmoq = moq.head; inmoq != NULL; inmoq =inmoq->next) {
          //cprintf("id: %d\n", inmoq->pid);
      
        if(inmoq->state == RUNNABLE) break;
      }
    
      acquire(&ptable.lock);
      c->proc = inmoq;
      switchuvm(inmoq);
      inmoq->state = RUNNING;
    
      swtch(&(c->scheduler), inmoq->context);
      switchkvm();
      release(&ptable.lock);
    
      // Process is done running for now.
      c->proc = 0;
		
        
      if(inmoq->state == ZOMBIE){
        monodequeue(inmoq);
        inmoq->monopolized =0;
      }
      
      if(moq.head==NULL){
        unmonopolize();
        //release(&ptable.lock);
      }

    }
    
    else if (Mono == 0){
      // Enable interrupts on this processor.
      sti();
    
      if(++global_ticks >= 100){
        
        for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
          
          if(p->monopolized == 1) continue;
          
          if(p->state != UNUSED && p->state != ZOMBIE){
            dequeue(&mlfq[p->level_of_queue], p);
            p->level_of_queue = 0;
            p->time_quantum = mlfq[0].time_quantum;
            enqueue(&mlfq[0], p);
          }
        }
        global_ticks = 0;
      }

      struct proc *selected_proc = NULL;
      int selected_queue = -1;
      acquire(&ptable.lock);
      // Iterate over queues from highest to lowest priority
      for(int i = 0; i < 4; i++){
     
        if(i<3){
          
          for(p = mlfq[i].head; p != NULL; p = p->next){
            
            if(p->state == RUNNABLE){
              selected_proc = p;
              selected_queue = i;
              break;  // Found a runnable process in the queue
            }
          }
          
          if(selected_proc != NULL){
            break;  // Found a runnable process, no need to check lower priority queues
          }
	}
	
	else if(i==3){
	  
	  // Check L3 for priority scheduling
	  if (!selected_proc) { // No process is selected from L0, L1, L2
	    
	    struct proc *highest = NULL;
	    int highpri = -1;
	    
	    for (p = mlfq[3].head; p != NULL; p = p->next) {
	    
	      if (p->state == RUNNABLE && p->priority > highpri) {
	        highest = p;
	        highpri= p->priority;
	        //cprintf("Run!%d\n ", highpri);
	      }
	    }
	    selected_proc = highest;
	    selected_queue = 3;
	  }
        }
      }
      
      // If a runnable process is selected, switch to it
      if(selected_proc != NULL){
    
        if(selected_proc->state == RUNNABLE){
          p = selected_proc;
          c->proc = p;
          switchuvm(p);
          p->state = RUNNING;
          
          swtch(&(c->scheduler), p->context);
          switchkvm();
          
          // Process is done running for now.
          c->proc = 0;
          p->time_quantum--;
          
          if(p->time_quantum <= 0){
          
            if(p->level_of_queue < 3){
              dequeue(&mlfq[selected_queue], p);
              
              int next_queue = 3;
                
              if(p->level_of_queue == 0){
                next_queue = (p->pid % 2 == 0) ? 2 : 1;
              }
              p->level_of_queue = next_queue;
              p->time_quantum = mlfq[next_queue].time_quantum;
              enqueue(&mlfq[next_queue], p);
            }
            
            else{
              p->priority = p->priority > 0 ? p->priority - 1 : 0;
              p->time_quantum = mlfq[3].time_quantum;
            }
          }
        }
      }
      release(&ptable.lock);
    }
  }
}
