#include "defs.h"

// start() jumps here in supervisor mode on all CPUs.
void main() {
  consoleinit();
  printfinit();
  printf("\n");
  printf("xv6 kernel is booting\n");
  printf("\n");
  kinit();            // physical page allocator
  kvminit();          // create kernel page table
  kvminithart();      // turn on paging
  procinit();         // process table
  trapinit();         // trap vectors
  trapinithart();     // install kernel trap vector
  plicinit();         // set up interrupt controller
  plicinithart();     // ask PLIC for device interrupts
  binit();            // buffer cache
  iinit();            // inode table
  fileinit();         // file table
  virtio_disk_init(); // emulated hard disk
  kerneltest();       // run kernel tests
  userinit();         // first user process
  scheduler();
}
