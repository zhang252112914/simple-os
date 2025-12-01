// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

int page_refcount[(PHYSTOP - KERNBASE) >> 12];

// lock to protect the page_refcount array
struct spinlock pageref_lock;

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void kinit() {
  initlock(&kmem.lock, "kmem");
  initlock(&pageref_lock, "pageref");
  freerange(end, (void *)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end) {
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa) {
  struct run *r;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&pageref_lock);
  if (--page_refcount[REF_INDEX(pa)] <= 0) {
    memset(pa, 1, PGSIZE);

    r = (struct run *)pa;
    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
  }
  release(&pageref_lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *kalloc(void) {
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if (r) {
    memset((char *)r, 5, PGSIZE); // fill with junk
    page_refcount[REF_INDEX(r)] = 1;
  }
  return (void *)r;
}

void *kcowalloc(void *pa) {
  acquire(&pageref_lock);

  if (page_refcount[REF_INDEX(pa)] <= 1) {
    release(&pageref_lock);
    return pa; // only one reference, no need to copy
  }

  void *new_pa = kalloc();
  if (new_pa == 0) {
    release(&pageref_lock);
    return 0; // allocation failed
  }

  memmove(new_pa, pa, PGSIZE);

  page_refcount[REF_INDEX(pa)]--;

  release(&pageref_lock);

  return new_pa;
}

void kref(void *pa) {
  acquire(&pageref_lock);
  page_refcount[REF_INDEX(pa)]++;
  release(&pageref_lock);
}
