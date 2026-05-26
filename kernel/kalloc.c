// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

int swap_enabled = 0;

void
swap_enable(void)
{
  swap_enabled = 1;
}

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

extern void swapread(uint64 ptr, int blkno);
extern void swapwrite(uint64 ptr, int blkno);

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// pa4: struct for page control
struct page pages[PHYSTOP/PGSIZE];
struct page *page_lru_head;
int num_free_pages;
int num_lru_pages;
struct spinlock lru_lock; // pa4 lock for lru list

uchar *swap_bitmap;
struct spinlock swap_lock;

struct page * // helper pa <-> page 
pa2page(uint64 pa)
{
  uint64 idx = (pa-KERNBASE) / PGSIZE;
  return &pages[idx];
}

uint64
page2pa(struct page *p)
{
  uint64 idx = p-pages;
  return KERNBASE + idx*PGSIZE;
}

void
lru_init(void)
{
  initlock(&lru_lock, "lru");
  page_lru_head = 0;
}

void
lru_add(struct page *p)
{
  acquire(&lru_lock);

  p->next = 0;
  p->prev = 0;

  if(page_lru_head == 0){
    page_lru_head = p;
    p->next = p;
    p->prev = p;
  }
  else{
    struct page *head = page_lru_head;
    struct page *tail = head->prev;
    p->next = head;
    p->prev = tail;
    tail->next = p;
    head->prev = p;
  }

  release(&lru_lock);
}

void
lru_remove(struct page *p)
{
  acquire(&lru_lock);

  if(page_lru_head == 0 || p == 0){
    release(&lru_lock);
    return;
  }

  if(p->next == 0 || p->prev == 0){
    release(&lru_lock);
    return;
  }

  if(p->next == p && p->prev == p){ // only one node
    page_lru_head = 0;
  }
  else{
    if(p == page_lru_head)
      page_lru_head = p->next;

    p->prev->next = p->next;
    p->next->prev = p->prev;
  }

  p->next = 0;
  p->prev = 0;

  release(&lru_lock);
}

struct page *
lru_select_victim(void)
{
  acquire(&lru_lock);

  if (page_lru_head == 0) {
    release(&lru_lock);
    return 0;
  }

  struct proc *p = myproc();
  if (p == 0) {
    // 커널 컨텍스트에서 호출된 경우: 스왑 못 함
    release(&lru_lock);
    return 0;
  }

  pagetable_t target = p->pagetable;

  struct page *start = page_lru_head;
  struct page *cur = start;

  // 원형 리스트 한 바퀴 돌면서,
  // "현재 프로세스의 페이지" + "access bit가 0"인 것을 찾는다.
  do {
    // 1) 다른 프로세스의 페이지면 그냥 건너뜀
    if (cur->pagetable != target) {
      cur = cur->next;
      continue;
    }

    uint64 va = (uint64)cur->vaddr;
    pte_t *pte = walk(cur->pagetable, va, 0);

    if (pte == 0) {
      // 매핑이 사라진 페이지는 LRU에서 제거
      struct page *next = cur->next;

      // 락을 쥔 상태에서 직접 제거 (lru_remove 안 부름)
      if (cur->next == cur) {
        page_lru_head = 0;
      } else {
        if (cur == page_lru_head)
          page_lru_head = cur->next;
        cur->prev->next = cur->next;
        cur->next->prev = cur->prev;
      }

      cur->next = 0;
      cur->prev = 0;

      if (page_lru_head == 0) {
        release(&lru_lock);
        return 0;
      }

      cur = next;
      start = page_lru_head;
      continue;
    }

    // access bit가 1이면 지우고 다음 후보로
    if (*pte & PTE_A) {
      *pte &= ~PTE_A;
      cur = cur->next;
      continue;
    }

    // access bit가 0인 현재 프로세스의 페이지 → victim
    struct page *victim = cur;
    page_lru_head = cur->next;
    release(&lru_lock);
    return victim;

  } while (cur != start);

  // 한 바퀴 돌았는데도 못 찾으면 포기
  release(&lru_lock);
  return 0;
}

void
swapinit(void)
{
  initlock(&swap_lock, "swap");
  swap_bitmap = (uchar*)kalloc();
  if(swap_bitmap==0)
    panic("swapinit: no mem for swap bitmap");
  memset(swap_bitmap, 0, PGSIZE);

  swap_enable();
}

static int
swap_alloc_slot(void)
{
  acquire(&swap_lock);
  for(int i = 0; i < PGSIZE*8 && i < N_SWAP_SLOT; i++){
    int byte=i/8;
    int bit = i%8;

    if((swap_bitmap[byte] & (1<<bit)) == 0) {
      swap_bitmap[byte] |= (1 << bit);
      release(&swap_lock);
      return i;
    }
  }
  release(&swap_lock);
  return -1;
}

void swap_free_slot(int idx)
{
  acquire(&swap_lock);
  int byte = idx / 8;
  int bit  = idx % 8;
  swap_bitmap[byte] &= ~(1 << bit);
  release(&swap_lock);
}

int
swap_out_one(void)
{
  struct page *victim = lru_select_victim();
  if(victim == 0)
    return -1; // lru empty

  pagetable_t pt = victim->pagetable;
  uint64 va = (uint64)victim->vaddr;

  pte_t *pte = walk(pt,va,0);
  if(!(pte && (*pte&PTE_V)))
    return -1;

  uint64 pa = PTE2PA(*pte);

  int slot = swap_alloc_slot();
  if(slot < 0){
    printf("swap_out_one: no swap slot\n");
    return -1;
  }  

  // 1) write data of physical page into disk
  swapwrite(pa,slot);

  // 2) update PTE to swapped state
  //    encode slot number as "fake" physical address (slot*PGSIZE)

  uint64 flags = PTE_FLAGS(*pte);
  flags &= ~PTE_V;
  flags |= PTE_S;
  uint64 slot_pa = (uint64)slot*PGSIZE;
  *pte = PA2PTE(slot_pa) | flags;

  // 3) delete from LRU + free physical page
  lru_remove(victim);
  kfree((void*)pa);

  return 0;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// pa4: kalloc function
void *
kalloc(void)
{
  struct run *r;

again:
  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(!r){
    struct proc *p = myproc();
    // no free page -> try swap-out
    if(swap_enabled && p && !swap_out_one())
      goto again; // retry if it works

    // also no victim (lru is empty) -> OOM
    printf("kalloc: out of memory\n");
    return 0;
  }

  memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
