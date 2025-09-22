#include "types.h"
#include "defs.h"
#include "riscv.h"
#include "../tests/print_test.h"

// start() jumps here in supervisor mode on all CPUs.
void main() {
  consoleinit();
  printfinit();
  printf("Hello, os\n");
  long long id = 2023302111177;
  printf("My ID is %lld\n", id);

  run_output_tests();

  clean_tests();

  panic("main");
}