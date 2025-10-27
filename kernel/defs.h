// console.c
void consoleinit(void);
void consputc(int);
void clear_screen(void);

// kalloc.c
void *kalloc(void);
void kfree(void *);
void kinit(void);

// plic.c
void plicinit(void);
void plicinithart(void);
int plic_claim(void);
void plic_complete(int);

// printf.c
int printf(char *, ...) __attribute__((format(printf, 1, 2)));
void panic(char *) __attribute__((noreturn));
void printfinit(void);

// string.c
int memcmp(const void *, const void *, uint);
void *memmove(void *, const void *, uint);
void *memset(void *, int, uint);
char *safestrcpy(char *, const char *, int);
int strlen(const char *);
int strncmp(const char *, const char *, uint);
char *strncpy(char *, const char *, int);

// trap.c
extern uint ticks;
void trapinit(void);
void trapinithart(void);
void handle_exception(struct trapframe *);
int register_interrupt(int, interrupt_handler_t, void *, char *);
void unregister_interrupt(int, interrupt_handler_t, void *);
void enable_interrupt(int);
void disable_interrupt(int);
void set_irq_priority(int, int);
int get_irq_priority(int);
void enter_interrupt(void);
void exit_interrupt(void);
int in_interrupt(void);
int interrupt_depth(void);
void print_irq_stats(void);
uint64 get_irq_count(int);

// uart.c
void uartinit(void);
int uartintr(void *);
void uartwrite(char[], int);
void uartputc_sync(int);
int uartgetc(void);

// vm.c
void kvminit(void);
void kvminithart(void);
void kvmmap(pagetable_t, uint64, uint64, uint64, int);
int mappages(pagetable_t, uint64, uint64, uint64, int);
pagetable_t uvmcreate(void);
uint64 uvmalloc(pagetable_t, uint64, uint64, int);
uint64 uvmdealloc(pagetable_t, uint64, uint64);
int uvmcopy(pagetable_t, pagetable_t, uint64);
void uvmfree(pagetable_t, uint64);
void uvmunmap(pagetable_t, uint64, uint64, int);
void uvmclear(pagetable_t, uint64);
pte_t *walk(pagetable_t, uint64, int);
uint64 walkaddr(pagetable_t, uint64);
int copyout(pagetable_t, uint64, char *, uint64);
int copyin(pagetable_t, char *, uint64, uint64);
int copyinstr(pagetable_t, char *, uint64, uint64);
int ismapped(pagetable_t, uint64);
uint64 vmfault(pagetable_t, uint64, int);

// kerneltest.c
void kerneltest(void);
