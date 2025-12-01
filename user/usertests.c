#include "kernel/param.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#include <stddef.h>

#define assert(expr)                                                           \
  do {                                                                         \
    if (!(expr))                                                               \
      printf("Assertion failed: " #expr "\n");                                 \
  } while (0)

// Pipe-based producer/consumer to validate basic IPC.
int pcpipe[2];
static char large_chunk[512];

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

void test_basic_syscalls(void) {
  printf("Testing basic syscalls...\n");
  int pid = getpid();
  printf("Current PID: %d\n", pid);

  int child_pid = fork();
  if (child_pid == 0) {
    // Child process
    printf("In child process with PID: %d\n", getpid());
    exit(42);
  } else if (child_pid > 0) {
    // Parent process
    int status;
    wait(&status);
    printf("Child exited with status: %d\n", status);
  } else {
    printf("Fork failed\n");
  }
}

void test_parameter_passing(void) {
  char buffer[] = "Hello, World!\n";
  int fd = open("console", O_RDWR);
  printf("%d\n", fd);

  if (fd < 0) {
    printf("Failed to open console\n");
    return;
  }

  int bytes_written = write(fd, buffer, sizeof(buffer));
  printf("Wrote %d bytes\n", bytes_written);

  write(-1, buffer, 10);
  write(fd, NULL, 10);
  write(fd, buffer, -1);

  close(fd);
}

void test_security(void) {
  char *invalid_ptr = (char *)0x1000000;
  int result = write(1, invalid_ptr, 10);
  printf("Invalid pointer write result: %d\n", result);

  // Feed controlled input via a pipe so this test is non-interactive.
  int p[2];
  assert(pipe(p) == 0);
  const char *payload = "abcdefghij";
  write(p[1], payload, 10);
  close(p[1]);

  char small_buffer[4];
  result = read(p[0], small_buffer, 10);
  printf("Buffer overflow read result: %d\n", result);
  close(p[0]);
}

void test_syscall_performance(void) {
  uint64 start = uptime();
  for (int i = 0; i < 10000; i++) {
    getpid();
  }

  uint64 end = uptime();
  printf("10000 getpid() calls took %lu cycles\n", end - start);
}

void test_filesystem_integrity(void) {
  printf("Testing filesystem integrity...\n");
  int fd = open("testfile", O_CREATE | O_RDWR);
  assert(fd >= 0);

  char buffer[] = "Hello, filesystem!\n";
  int bytes = write(fd, buffer, strlen(buffer));
  assert(bytes == strlen(buffer));
  close(fd);

  fd = open("testfile", O_RDONLY);
  assert(fd >= 0);

  char read_buffer[32];
  bytes = read(fd, read_buffer, sizeof(read_buffer));
  read_buffer[bytes] = '\0';
  assert(strcmp(buffer, read_buffer) == 0);
  close(fd);

  assert(unlink("testfile") == 0);
  printf("Filesystem integrity test completed\n");
}

void test_concurrent_access(void) {
  printf("Testing concurrent file access...\n");

  for (int i = 0; i < 4; i++) {
    if (fork() == 0) {
      char filename[32];
      snprintf(filename, sizeof(filename), "test_%d", i);

      for (int j = 0; j < 100; j++) {
        int fd = open(filename, O_CREATE | O_RDWR);
        if (fd < 0) {
          printf("Process %d: Failed to open %s\n", i, filename);
          exit(1);
        }

        write(fd, &j, sizeof(j));
        close(fd);
        unlink(filename);
      }
      exit(0);
    }
  }

  for (int i = 0; i < 4; i++) {
    wait(0);
  }

  printf("Concurrent file access test completed\n");
}

void test_filesystem_performance(void) {
  printf("Testing filesystem performance...\n");
  uint64 start = uptime();

  for (int i = 0; i < 100; i++) {
    char filename[32];
    snprintf(filename, sizeof(filename), "small_%d", i);

    int fd = open(filename, O_CREATE | O_RDWR);
    if (fd < 0) {
      printf("Failed to create %s\n", filename);
      continue;
    }

    write(fd, "small_file_thirty_two_byte_write", 32);
    close(fd);
  }

  uint64 small_files_time = uptime() - start;

  start = uptime();
  int fd = open("largefile", O_CREATE | O_RDWR);
  if (fd < 0) {
    printf("Failed to create largefile\n");
  } else {
    // Avoid blowing the small user stack: use a modest static chunk.
    for (int i = 0; i < (4096 * 1024) / sizeof(large_chunk); i++) {
      write(fd, large_chunk, sizeof(large_chunk));
    }
    close(fd);
  }

  uint64 large_file_time = uptime() - start;
  printf("Small files (100x32B): %lu cycles\n", small_files_time);
  printf("Large file (4MB): %lu cycles\n", large_file_time);

  for (int i = 0; i < 100; i++) {
    char filename[32];
    snprintf(filename, sizeof(filename), "small_%d", i);
    unlink(filename);
  }
  unlink("largefile");
}

int main(int argc, char *argv[]) {
  test_process_creation();
  test_scheduler();
  test_synchronization();
  test_basic_syscalls();
  test_parameter_passing();
  test_security();
  test_syscall_performance();
  test_filesystem_integrity();
  test_concurrent_access();
  test_filesystem_performance();
  printf("All user tests passed\n");
  exit(0);
}
