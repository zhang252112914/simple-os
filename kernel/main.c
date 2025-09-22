#include "types.h"
#include "defs.h"
#include "riscv.h"

// start() jumps here in supervisor mode on all CPUs.
void main() {
  uartinit();
  uart_puts("hello os\n");
}