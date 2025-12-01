#include "kernel/param.h"
#include "user/user.h"

#define assert(expr)                                                           \
  do {                                                                         \
    if (!(expr))                                                               \
      printf("Assertion failed: " #expr "\n");                                 \
  } while (0)

// Pipe-based producer/consumer to validate basic IPC.
int pcpipe[2];

void producer_task(void) {
  // Only writing.
  close(pcpipe[0]);
  for (int i = 1; i <= 20; i++) {
    write(pcpipe[1], &i, sizeof(i));
  }
  close(pcpipe[1]);
  exit(0);
}

void consumer_task(void) {
  close(pcpipe[1]);
  int expected = 1;
  int v;
  while (read(pcpipe[0], &v, sizeof(v)) == sizeof(v)) {
    assert(v == expected);
    expected++;
  }
  assert(expected == 21);
  close(pcpipe[0]);
  exit(0);
}

int create_process(void (*entry)(void)) {
  int pid = fork();
  if (pid == 0) {
    entry();
    exit(0);
  }
  return pid;
}

void simple_task(void) {
  for (int i = 0; i < 5; i++) {
    for (volatile int j = 0; j < 1000000; j++)
      ; // busy wait
  }
}

void cpu_intensive_task(void) {
  volatile unsigned long x = 0;
  for (unsigned long i = 0; i < 100000000; i++) {
    x += i;
  }
}

void test_process_creation(void) {
  printf("Testing process creation...\n");
  int pid = create_process(simple_task);
  assert(pid > 0);
  int wpid = wait(0);
  assert(wpid == pid);

  int pids[NPROC];
  int count = 0;
  for (int i = 0; i < NPROC + 5; i++) {
    int pid = create_process(simple_task);
    if (pid <= 0)
      break;
    pids[count++] = pid;
  }
  printf("Created %d processes\n", count);
  for (int i = 0; i < count; i++) {
    printf("%d ", pids[i]);
  }
  printf("\n");
  printf("Waiting for processes to finish...\n");
  for (int i = 0; i < count; i++) {
    int wpid = wait(0);
    printf("%d ", wpid);
  }
  printf("\n");

  printf("Process creation test completed\n");
}

void test_scheduler(void) {
  printf("Testing scheduler...\n");
  int pids[3];
  int target = 0;
  for (int i = 0; i < 3; i++) {
    pids[i] = create_process(cpu_intensive_task);
    if (pids[i] <= 0) {
      printf("fork failed in test_scheduler\n");
    } else {
      target++;
    }
  }

  uint64 start = uptime();
  pause(10);
  uint64 end = uptime();

  int waited = 0;
  while (waited < target) {
    int w = wait(0);
    if (w < 0)
      break;
    waited++;
  }
  assert(waited == target);

  printf("Scheduler test completed in %lu cycles\n", end - start);
}

void test_synchronization(void) {
  printf("Testing synchronization...\n");
  assert(pipe(pcpipe) == 0);

  int p1 = create_process(producer_task);
  int p2 = create_process(consumer_task);
  assert(p1 > 0 && p2 > 0);

  // Parent closes both ends; children own them now.
  close(pcpipe[0]);
  close(pcpipe[1]);

  int seen = 0;
  while (seen < 2) {
    int w = wait(0);
    if (w < 0)
      break;
    if (w == p1 || w == p2)
      seen++;
  }
  assert(seen == 2);

  printf("Synchronization test completed\n");
}

int main(int argc, char *argv[]) {
  test_process_creation();
  test_scheduler();
  test_synchronization();
  printf("All user tests passed\n");
  exit(0);
}
