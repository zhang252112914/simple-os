#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

// start() jumps here in supervisor mode on all CPUs.
void main() {
  consoleinit();
  printfinit();
  printf("\n");
  printf("kernel is booting\n");
  printf("\n");
  kinit();       // physical page allocator
  kvminit();     // create kernel page table
  kvminithart(); // turn on paging
  kerneltest();  // run kernel tests
  panic("main");
}
