#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "types.h"

// Mutual exclusion lock.
struct spinlock {
  uint locked; // Is the lock held?

  // For debugging:
  char *name; // Name of lock.
};

#endif // SPINLOCK_H