#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"

struct mmap_area ma[64];
struct spinlock mm_lock;

static int map_mmap_page(pagetable_t, struct mmap_area *, uint64);
static int populate_mmap_area(struct proc *, struct mmap_area *);
static void clear_mmap_entry(struct mmap_area *);
static int snapshot_mmap_area(struct proc *, uint64, struct mmap_area *);
static void cleanup_child_mmaps(struct proc *, struct mmap_area **, int);
void unmap_proc_mmaps(struct proc *);
static int remove_mmap_entry(struct proc *, uint64, struct mmap_area *);

int weight_table[40] = {
 /* 0  */     88761,     71755,     56483,     46273,     36291,
 /* 5  */     29154,     23254,     18705,     14949,     11916,
 /* 10 */      9548,      7620,      6100,      4904,      3906,
 /* 15 */      3121,      2501,      1991,      1586,      1277,
 /* 20 */      1024,       820,       655,       526,       423,
 /* 25 */       335,       272,       215,       172,       137,
 /* 30 */       110,        87,        70,        56,        45,
 /* 35 */        36,        29,        23,        18,        15,
};

int avg_vruntime;
int min_vruntime;
int total_weight;

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
	initlock(&mm_lock, "mmap");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
	p->nice = 20;
	p->vdeadline = BASE_SLICE;
  p->runtime = 0;
  p->vruntime = 0;
  p->time_slice = BASE_SLICE;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
	unmap_proc_mmaps(p);

  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
	p->nice = 0;
	p->runtime = 0;
  p->vruntime = 0;
  p->time_slice = 0;
  p->vdeadline = 0;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if(sz + n > TRAPFRAME) {
      return -1;
    }
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();
	struct mmap_area *parent_maps[64];
  struct mmap_area *child_slots[64];
  int map_count = 0;

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

	np->vruntime = p->vruntime;
  np->nice = p->nice;

  np->vdeadline = np->vruntime + (BASE_SLICE*(weight_table[20]/weight_table[np->nice]));

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

	  // --- clone mmap metadata and already-mapped pages (lazy pages stay lazy) ---
  acquire(&mm_lock);
  for (int mi = 0; mi < 64; mi++) {
    if (ma[mi].length == 0 || ma[mi].p != p) continue; // skip unused / other process

    // find a free slot in the global table for the child
    int free_idx = -1;
    for (int mj = 0; mj < 64; mj++) {
      if (ma[mj].length == 0) { free_idx = mj; break; }
    }
    if (free_idx < 0) {                      // no free slot
      release(&mm_lock);
      goto fork_fail;
    }

    // duplicate metadata; file-backed mapping must bump ref count
    parent_maps[map_count] = &ma[mi];
    child_slots[map_count] = &ma[free_idx];
    map_count++;

    ma[free_idx].addr = ma[mi].addr;
    ma[free_idx].length = ma[mi].length;
    ma[free_idx].offset = ma[mi].offset;
    ma[free_idx].prot = ma[mi].prot;
    ma[free_idx].flags = ma[mi].flags;
    ma[free_idx].p = np;
    ma[free_idx].f = (ma[mi].flags & MAP_ANONYMOUS) ? 0 : filedup(ma[mi].f);
  }
  release(&mm_lock);


  // copy only pages that are already mapped in parent
  for(int mi = 0; mi < map_count; mi++){
    struct mmap_area *src = parent_maps[mi];
    for(uint64 va = src->addr; va < src->addr + src->length; va += PGSIZE){
      pte_t *pte = walk(p->pagetable, va, 0);
      if(pte == 0 || (*pte & PTE_V) == 0)
        continue;
      uint64 pa = PTE2PA(*pte);

      char *mem = kalloc();
      if(mem == 0)
        goto fork_fail;
      memmove(mem, (void*)pa, PGSIZE);

      uint flags = PTE_FLAGS(*pte) & (PTE_U | PTE_R | PTE_W);
      if(mappages(np->pagetable, va, PGSIZE, (uint64)mem, flags) != 0){
        kfree(mem);
        goto fork_fail;
      }
    }
  }  

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

	update_avg_vruntime();

  return pid;

  // --- cleanup path on failure after allocproc ---
fork_fail:
  // unmap any pages mapped into child's user space and free proc
  cleanup_child_mmaps(np,child_slots,map_count);
  uvmunmap(np->pagetable, 0, np->sz/PGSIZE, 1);
  freeproc(np);
  release(&np->lock);
  return -1;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();

		update_avg_vruntime();

    int found = 0;

		struct proc *min_vdl_proc = 0; 
    int min_vdl = MAX_INT;

    for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock);	
      if(p->state == RUNNABLE && p->vdeadline < min_vdl) {
        if (isEligible(p)){
          min_vdl = p->vdeadline;
          min_vdl_proc = p;
        }
      }
      release(&p->lock);
    }

    if(min_vdl_proc) {
      acquire(&min_vdl_proc->lock);
      if(min_vdl_proc->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        min_vdl_proc->time_slice = BASE_SLICE;
        min_vdl_proc->state = RUNNING;
        c->proc = min_vdl_proc;
        swtch(&c->context, &min_vdl_proc->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
        found = 1;
      }
      release(&min_vdl_proc->lock);
    }

    if(found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void
wakeup(void *chan)
{
  struct proc *p;
	struct proc *cur = mycpu()->proc;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != cur){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
				p->vdeadline = p->vruntime + (BASE_SLICE *(weight_table[20]/weight_table[p->nice]));
        p->time_slice = BASE_SLICE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

int getnice(int pid)
{
  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);

    if(p->pid == pid){
      int v = p->nice;
      release(&p->lock);
      return v;
    }
    release(&p->lock);
  }
  return -1;
}

int
setnice(int pid, int value)
{
  if(value < 0 || value > 39)
    return -1;
  
  struct proc *p;
  for(p=proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->nice = value;
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void ps(int pid)
{
  static char* states[] = {
    "UNUSED  ",
    "USED    ",
    "SLEEPING", 
    "RUNNABLE", 
    "RUNNING ", 
    "ZOMBIE  "
  };

  struct proc *p;
  int runtime_weight;
  int weight;
  char *eligible;

  printf("name\t\tpid\tstate\t\tpriority\truntime/weight\truntime\t\tvruntime\tvdeadline\tisEligible\tticks %d\n", ticks * 1000);

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if((!pid || p->pid == pid) && p->state!=UNUSED){
      weight = weight_table[p->nice];
      runtime_weight = (weight)?(p->runtime/weight):0;
      eligible = isEligible(p)?"true":"false";
      //printf("%s\t%d\t%s\t\t%d\t%d\t%d\t\t%d\t%d\t%s\n", p->name, p->pid, states[p->state], p->nice, runtime_weight, p->runtime, p->vruntime, p->vdeadline, eligible);
      printf("%s\t\t", p->name);
      printf("%d\t", p->pid);
      printf("%s\t", states[p->state]);
      printf("%d\t\t", p->nice);
      printf("%d\t\t", runtime_weight);
      printf("%d\t\t", p->runtime);
      printf("%d\t\t", p->vruntime);
      printf("%d\t\t", p->vdeadline);
      printf("%s\n", eligible);
    }
    release(&p->lock);
  }

  return;
}

extern struct spinlock wait_lock;

int
waitpid(int pid){
  struct proc *p;
  struct proc *mp = myproc();
  
  acquire(&wait_lock);
  for(;;){
    int found = 0;
    
    for(p = proc; p < &proc[NPROC]; p++){
      if(p->parent == mp && p->pid == pid){
        found = 1;
        
        acquire(&p->lock); //이프문에 안들어가면 오버헤드 발생
        if(p->state == ZOMBIE){
          freeproc(p);
          release(&p->lock);
          release(&wait_lock);
          return 0;
        }
        release(&p->lock);
      }
    }
    
    if(!found || mp->killed){
      release(&wait_lock);
      return -1;
    }
    
    sleep(mp, &wait_lock);
  }
}


void
update_avg_vruntime(void){
  avg_vruntime = 0;
  total_weight = 0;
  struct proc *p;
  min_vruntime = MAX_INT;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state == RUNNABLE || p->state == RUNNING){
      if(p->vruntime < min_vruntime) min_vruntime = p->vruntime;

      total_weight += weight_table[p->nice];
    }
    release(&p->lock);
  }

  if(min_vruntime == MAX_INT) min_vruntime = 0;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state == RUNNABLE || p->state == RUNNING){
      avg_vruntime += (p->vruntime - min_vruntime)*weight_table[p->nice];
    }
    release(&p->lock);
  }
}

int
isEligible(struct proc *p){
  return (avg_vruntime >= total_weight * (p->vruntime - min_vruntime))?1:0;
}

static void
clear_mmap_entry(struct mmap_area *m)
{
  memset(m, 0, sizeof(*m));
}

static int
map_mmap_page(pagetable_t pt, struct mmap_area *area, uint64 va)
{
  pte_t *pte = walk(pt, va, 0);
  if(pte && (*pte & PTE_V))
    return 0;

  //printf("[map_mmap_page] calling kalloc, freemem before=%d\n", freemem());
  char *pa = kalloc();
  //printf("[map_mmap_page] mem=%p after kalloc, freemem=%d\n", pa, freemem());
  if(pa == 0)
    return -1;
  memset(pa, 0, PGSIZE);

  if(!(area->flags & MAP_ANONYMOUS) && area->f){
    uint64 page_off = va - area->addr;
    uint64 remaining = (uint64)area->length - page_off;
    int n = remaining >= PGSIZE ? PGSIZE : (int)remaining;
    if(n > 0){
      if(area->f->type != FD_INODE){
        kfree(pa);
        return -1;
      }
      uint off = (uint)((uint64)area->offset + page_off);
      ilock(area->f->ip);
      int r = readi(area->f->ip, 0, (uint64)pa, off, n);
      iunlock(area->f->ip);
      if(r < 0){
        kfree(pa);
        return -1;
      }
    }
  }

  int perm = PTE_U | PTE_R;
  if(area->prot & PROT_WRITE)
    perm |= PTE_W;

  if(mappages(pt, va, PGSIZE, (uint64)pa, perm) != 0){
    kfree(pa);
    return -1;
  }

  return 0;
}

static int
populate_mmap_area(struct proc *p, struct mmap_area *area)
{
  int mapped = 0;

  for(uint64 va = area->addr; va < area->addr + area->length; va += PGSIZE){
    if(map_mmap_page(p->pagetable, area, va) < 0){
      if(mapped > 0)
        uvmunmap(p->pagetable, area->addr, mapped, 1);
      return -1;
    }
    mapped++;
  }

  return 0;
}

static void
cleanup_child_mmaps(struct proc *child, struct mmap_area **slots, int count)
{
  for(int i = 0; i < count; i++){
    if(slots[i] == 0)
      continue;

    struct mmap_area snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    acquire(&mm_lock);
    if(slots[i]->p == child && slots[i]->length != 0){
      snapshot = *slots[i];
      clear_mmap_entry(slots[i]);
    }
    release(&mm_lock);

    if(snapshot.length == 0)
      continue;

    uvmunmap(child->pagetable, snapshot.addr, snapshot.length/PGSIZE, 1);
    if(!(snapshot.flags & MAP_ANONYMOUS) && snapshot.f)
      fileclose(snapshot.f);
  }
}

void
unmap_proc_mmaps(struct proc *p)
{
  for(;;){
    struct mmap_area snapshot;
    int found = 0;

    acquire(&mm_lock);
    for(int i = 0; i < 64; i++){
      if(ma[i].length == 0 || ma[i].p != p)
        continue;
      snapshot = ma[i];
      clear_mmap_entry(&ma[i]);
      found = 1;
      break;
    }
    release(&mm_lock);

    if(!found)
      break;

    if(p->pagetable)
      uvmunmap(p->pagetable, snapshot.addr, snapshot.length/PGSIZE, 1);
    if(!(snapshot.flags & MAP_ANONYMOUS) && snapshot.f)
      fileclose(snapshot.f);
  }
}

uint64
mmap(uint64 addr, int length, int prot, int flags, int fd, int offset)
{
  struct proc *p = myproc();
  uint64 start = MMAPBASE + addr;
  int supported_flags = MAP_ANONYMOUS | MAP_POPULATE;

  if((addr % PGSIZE) != 0)
    return 0;
  if(length <= 0 || (length % PGSIZE) != 0)
    return 0;
  if(start < MMAPBASE || start + (uint64)length < start)
    return 0;
  if(flags & ~supported_flags)
    return 0;
  if(prot != PROT_READ && prot != (PROT_READ | PROT_WRITE))
    return 0;

  struct file *file = 0;
  if(flags & MAP_ANONYMOUS){
    if(fd != -1 || offset != 0)
      return 0;
  } else {
    if(fd < 0 || fd >= NOFILE)
      return 0;
    if(offset < 0 || (offset % PGSIZE) != 0)
      return 0;
    file = p->ofile[fd];
    if(file == 0)
      return 0;
    if((prot & PROT_READ) && !file->readable)
      return 0;
    if((prot & PROT_WRITE) && !file->writable)
      return 0;
  }

  struct file *file_for_entry = 0;
  if(file)
    file_for_entry = filedup(file);

  acquire(&mm_lock);
  int slot = -1;
  for(int i = 0; i < 64; i++){
    if(ma[i].length == 0){
      if(slot < 0)
        slot = i;
      continue;
    }
    if(ma[i].p != p)
      continue;
    uint64 other_start = ma[i].addr;
    uint64 other_end = other_start + ma[i].length;
    if(!(start + length <= other_start || other_end <= start)){
      release(&mm_lock);
      if(file_for_entry)
        fileclose(file_for_entry);
      return 0;
    }
  }
  if(slot < 0){
    release(&mm_lock);
    if(file_for_entry)
      fileclose(file_for_entry);
    return 0;
  }

  ma[slot].addr = start;
  ma[slot].length = length;
  ma[slot].offset = (flags & MAP_ANONYMOUS) ? 0 : offset;
  ma[slot].prot = prot;
  ma[slot].flags = flags;
  ma[slot].p = p;
  ma[slot].f = (flags & MAP_ANONYMOUS) ? 0 : file_for_entry;
  release(&mm_lock);

  if(flags & MAP_POPULATE){
    if(populate_mmap_area(p, &ma[slot]) < 0){
      struct mmap_area snapshot;

      acquire(&mm_lock);
      snapshot = ma[slot];
      clear_mmap_entry(&ma[slot]);
      release(&mm_lock);

      if(!(snapshot.flags & MAP_ANONYMOUS) && snapshot.f)
        fileclose(snapshot.f);
      uvmunmap(p->pagetable, snapshot.addr, snapshot.length/PGSIZE, 1);
      return 0;
    }
  }

  return start;
}

static int
snapshot_mmap_area(struct proc *p, uint64 va, struct mmap_area *out)
{
  int found = 0;

  acquire(&mm_lock);
  for(int i = 0; i < 64; i++){
    if(ma[i].length == 0 || ma[i].p != p)
      continue;
    uint64 start = ma[i].addr;
    uint64 end = start + ma[i].length;
    if(start <= va && va < end){
      *out = ma[i];
      found = 1;
      break;
    }
  }
  release(&mm_lock);

  return found;
}

static int
remove_mmap_entry(struct proc *p, uint64 addr, struct mmap_area *out)
{
  int found = 0;

  acquire(&mm_lock);
  for(int i = 0; i < 64; i++){
    if(ma[i].length == 0 || ma[i].p != p)
      continue;
    if(ma[i].addr == addr){
      *out = ma[i];
      clear_mmap_entry(&ma[i]);
      found = 1;
      break;
    }
  }
  release(&mm_lock);

  return found;
}

int
vmfault_mmap(pagetable_t pt, uint64 fault_va, int read)
{
  struct proc *p = myproc();
  uint64 va = PGROUNDDOWN(fault_va);
  struct mmap_area area;

  //printf("[vmfault_mmap] pid=%d fault_va=0x%lu (va=0x%lu) read=%d\n", p->pid, fault_va, va, read);

  if(!snapshot_mmap_area(p,va,&area)){
    //printf("[vmfault_mmap] no mmap area found for va=0x%lu\n", va);
    return 0;
  }

  //printf("[vmfault_mmap] found mmap area: addr=0x%lu len=%d prot=%d flags=%d\n",area.addr, area.length, area.prot, area.flags);

  // Check access permissions.
  if (!read && !(area.prot & PROT_WRITE))
    return 0; // Write fault but mapping is read-only.
  if (read && !(area.prot & PROT_READ))
    return 0; // Read fault but mapping does not allow read.

  // If the page is already mapped, nothing to do.
  pte_t *pte = walk(pt, va, 0);
  if (pte && (*pte & PTE_V))
    return 1;

  //printf("[vmfault_mmap] allocating page for va=0x%lu (read=%d)\n", va, read);

  if(map_mmap_page(pt,&area,va)<0){
    //printf("[vmfault_mmap] map_mmap_page failed for va=0x%lu\n", va);
    return 0;
  }

  //printf("[vmfault_mmap] page successfully mapped for va=0x%lu\n", va);
  return 1; // Successfully handled one lazy page fault.
}

int
munmap(uint64 addr)
{
  struct proc *p = myproc();
  struct mmap_area area;

  if(!remove_mmap_entry(p,addr,&area)) return -1;

  int start = PGROUNDDOWN(area.addr);
  int npages = (area.length + PGSIZE - 1) / PGSIZE;
  /*
  printf("[munmap] unmap range start=0x% npages=%d end=0x%ld\n", start, npages, start + (uint64)npages*PGSIZE);
  // 디버그: 언매핑 직전 남은 PTE가 있는지 확인
  for (uint64 va = start; va < start + npages*PGSIZE; va += PGSIZE) {
    pte_t *pte = walk(p->pagetable, va, 0);
    if (pte && (*pte & PTE_V))
      printf("[munmap] still mapped before unmap va=0x%ld pte=0x%lx\n", va, *pte);
  }
  */
  uvmunmap(p->pagetable, start, npages,1);
  /*
  // 언매핑 직후 확인
  for (uint64 va = start; va < start + npages*PGSIZE; va += PGSIZE) {
    pte_t *pte = walk(p->pagetable, va, 0);
    if (pte && (*pte & PTE_V))
      printf("[munmap] still mapped AFTER unmap va=0x%ld pte=0x%lx\n", va, *pte);
  }
  */

  if(!(area.flags & MAP_ANONYMOUS) && area.f) fileclose(area.f);

  return 1;
}

int
freemem(void)
{
  return freememCount();
}
