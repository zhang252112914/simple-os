// Mutual exclusion spin locks.

#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"

void initlock(struct spinlock *lk, char *name) {
  lk->name = name;
  lk->locked = 0;
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
void acquire(struct spinlock *lk) {
  push_off(); // Just disable interrupts

  if (holding(lk))
    panic("acquire");

  lk->locked = 1; // No atomic operation needed!
}

// Release the lock.
void release(struct spinlock *lk) {
  if (!holding(lk))
    panic("release");

  lk->locked = 0; // Simple assignment is fine

  pop_off(); // Re-enable interrupts
}

// Check whether this cpu is holding the lock.
// Interrupts must be off.
int holding(struct spinlock *lk) { return lk->locked; }

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.

void push_off(void) {
  int old = intr_get();

  // disable interrupts to prevent an involuntary context
  // switch while using mycpu().
  intr_off();

  if (mycpu()->noff == 0)
    mycpu()->intena = old;
  mycpu()->noff += 1;
}

void pop_off(void) {
  struct cpu *c = mycpu();
  if (intr_get())
    panic("pop_off - interruptible");
  if (c->noff < 1)
    panic("pop_off");
  c->noff -= 1;
  if (c->noff == 0 && c->intena)
    intr_on();
}
