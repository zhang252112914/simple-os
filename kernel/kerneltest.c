#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

#define assert(expr)                                                           \
  do {                                                                         \
    if (!(expr))                                                               \
      panic("Assertion failed: " #expr);                                       \
  } while (0)

void test_physical_memory(void) {
  // 测试基本分配和释放
  void *page1 = kalloc();
  void *page2 = kalloc();
  assert(page1 != page2);
  assert(((uint64)page1 & 0xFFF) == 0); // 页对齐检查
  // 测试数据写入
  *(int *)page1 = 0x12345678;
  assert(*(int *)page1 == 0x12345678);
  // 测试释放和重新分配
  kfree(page1);
  void *page3 = kalloc();
  // page3可能等于page1（取决于分配策略）
  kfree(page2);
  kfree(page3);

  printf("physical memory test passed\n");
}

void test_pagetable(void) {
  pagetable_t pt = uvmcreate();
  uint64 va = 0x1000000;
  uint64 pa = (uint64)kalloc();
  assert(mappages(pt, va, PGSIZE, pa, PTE_R | PTE_W) == 0);

  pte_t *pte = walk(pt, va, 0);
  assert(pte != 0 && (*pte & PTE_V));
  assert(PTE2PA(*pte) == pa);

  assert(*pte & PTE_R);
  assert(*pte & PTE_W);
  assert(!(*pte & PTE_X));

  printf("pagetable test passed\n");
}

void kerneltest(void) {
  test_physical_memory();
  test_pagetable();
  printf("all kernel tests passed\n");
}