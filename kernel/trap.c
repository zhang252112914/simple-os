#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "trap.h"
#include "defs.h"

// struct spinlock tickslock;
uint ticks;

extern char trampoline[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

// Global IRQ descriptors
struct irq_desc irq_descriptors[MAX_IRQS];

// Nested interrupt tracking
struct {
  int depth;          // Current interrupt nesting depth
  int saved_priority; // Priority before interrupt
} interrupt_context;

static void dump_trapframe(const struct trapframe *tf);

__attribute__((weak)) void handle_illegal_instruction(struct trapframe *tf) {
  printf("Illegal instruction at sepc=0x%lx\n", tf->sepc);
  dump_trapframe(tf);
  tf->sepc += 4; // Skip the illegal instruction to avoid infinite loop
}

__attribute__((weak)) void handle_syscall(struct trapframe *tf) {
  printf("Unexpected system call: sepc=0x%lx\n", tf->sepc);
  dump_trapframe(tf);
  tf->sepc += 4; // Skip the syscall instruction to avoid infinite loop
}

__attribute__((weak)) void handle_instruction_page_fault(struct trapframe *tf) {
  printf("Instruction page fault at 0x%lx\n", tf->stval);
  dump_trapframe(tf);
  tf->sepc += 4; // Skip the faulting instruction to avoid infinite loop
}

__attribute__((weak)) void handle_load_page_fault(struct trapframe *tf) {
  printf("Load fault (scause=%lu) at 0x%lx\n", tf->scause, tf->stval);
  dump_trapframe(tf);
  tf->sepc += 4; // Skip the faulting instruction to avoid infinite loop
}

__attribute__((weak)) void handle_store_page_fault(struct trapframe *tf) {
  printf("Store fault (scause=%lu) at 0x%lx\n", tf->scause, tf->stval);
  dump_trapframe(tf);
  tf->sepc += 4; // Skip the faulting instruction to avoid infinite loop
}

void trapinit(void) {
  // Initialize all IRQ descriptors
  for (int i = 0; i < MAX_IRQS; i++) {
    // initlock(&irq_descriptors[i].lock, "irq");
    irq_descriptors[i].priority = IRQ_PRIORITY_NORMAL;
    irq_descriptors[i].enabled = 0;
    irq_descriptors[i].count = 0;
    irq_descriptors[i].unhandled_count = 0;

    // Initialize handler slots
    for (int j = 0; j < MAX_HANDLERS_PER_IRQ; j++) {
      irq_descriptors[i].handlers[j].valid = 0;
      irq_descriptors[i].handlers[j].handler = 0;
      irq_descriptors[i].handlers[j].dev_id = 0;
    }
  }

  interrupt_context.depth = 0;
  interrupt_context.saved_priority = IRQ_PRIORITY_IDLE;
}

// set up to take exceptions and traps while in the kernel.
void trapinithart(void) { w_stvec((uint64)kernelvec); }

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void kerneltrap() {
  struct trapframe tf;
  int which_dev = 0;

  tf.sepc = r_sepc();
  tf.sstatus = r_sstatus();
  tf.scause = r_scause();
  tf.stval = r_stval();

  if ((tf.sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if (intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if (tf.scause & (1UL << 63)) {
    if ((which_dev = devintr()) == 0) {
      printf("Unknown interrupt: scause=0x%lx sepc=0x%lx\n", tf.scause,
             tf.sepc);
      panic("kerneltrap");
    }
  } else {
    handle_exception(&tf);
  }

  // give up the CPU if this is a timer interrupt.
  // if (which_dev == 2 && myproc() != 0)
  // yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(tf.sepc);
  w_sstatus(tf.sstatus);
}

void clockintr() {
  // if (cpuid() == 0) {
  // acquire(&tickslock);
  ticks++;
  // wakeup(&ticks);
  // release(&tickslock);
  // }

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second.
  w_stimecmp(r_time() + 1000000);
}

//
// Register an interrupt handler
// Supports multiple handlers per IRQ (shared interrupts)
//
int register_interrupt(int irq, interrupt_handler_t handler, void *dev_id,
                       char *name) {
  if (irq < 0 || irq >= MAX_IRQS || handler == 0)
    return -1;

  struct irq_desc *desc = &irq_descriptors[irq];

  // acquire(&desc->lock);

  // Find an empty slot
  int slot = -1;
  for (int i = 0; i < MAX_HANDLERS_PER_IRQ; i++) {
    if (!desc->handlers[i].valid) {
      slot = i;
      break;
    }
  }

  if (slot == -1) {
    // release(&desc->lock);
    printf("register_interrupt: no free slots for IRQ %d\n", irq);
    return -1;
  }

  // Register the handler
  desc->handlers[slot].handler = handler;
  desc->handlers[slot].dev_id = dev_id;
  desc->handlers[slot].valid = 1;

  if (name) {
    desc->handlers[slot].name = name;
  }

  // release(&desc->lock);

  printf("Registered handler for IRQ %d: %s\n", irq, name ? name : "unnamed");
  return 0;
}

//
// Unregister an interrupt handler
//
void unregister_interrupt(int irq, interrupt_handler_t handler, void *dev_id) {
  if (irq < 0 || irq >= MAX_IRQS)
    return;

  struct irq_desc *desc = &irq_descriptors[irq];

  // acquire(&desc->lock);

  // Find and remove the handler
  for (int i = 0; i < MAX_HANDLERS_PER_IRQ; i++) {
    if (desc->handlers[i].valid && desc->handlers[i].handler == handler &&
        desc->handlers[i].dev_id == dev_id) {
      desc->handlers[i].valid = 0;
      desc->handlers[i].handler = 0;
      desc->handlers[i].dev_id = 0;
      break;
    }
  }

  // release(&desc->lock);
}

//
// Enable a specific interrupt
//
void enable_interrupt(int irq) {
  if (irq < 0 || irq >= MAX_IRQS)
    return;

  // acquire(&irq_descriptors[irq].lock);
  irq_descriptors[irq].enabled = 1;
  // release(&irq_descriptors[irq].lock);

  // Platform-specific enable (e.g., PLIC on RISC-V)
  // plic_enable(irq);
}

//
// Disable a specific interrupt
//
void disable_interrupt(int irq) {
  if (irq < 0 || irq >= MAX_IRQS)
    return;

  // acquire(&irq_descriptors[irq].lock);
  irq_descriptors[irq].enabled = 0;
  // release(&irq_descriptors[irq].lock);

  // Platform-specific disable
  // plic_disable(irq);
}

//
// Set interrupt priority
//
void set_irq_priority(int irq, int priority) {
  if (irq < 0 || irq >= MAX_IRQS)
    return;

  if (priority < 0 || priority > 7)
    priority = IRQ_PRIORITY_NORMAL;

  // acquire(&irq_descriptors[irq].lock);
  irq_descriptors[irq].priority = priority;
  // release(&irq_descriptors[irq].lock);
}

//
// Get interrupt priority
//
int get_irq_priority(int irq) {
  if (irq < 0 || irq >= MAX_IRQS)
    return -1;

  return irq_descriptors[irq].priority;
}

//
// Handle an interrupt with priority-based nested interrupt support
//
void handle_irq(int irq) {
  if (irq < 0 || irq >= MAX_IRQS)
    return;

  struct irq_desc *desc = &irq_descriptors[irq];

  // Update statistics
  // acquire(&desc->lock);
  desc->count++;

  if (!desc->enabled) {
    // release(&desc->lock);
    return;
  }

  int current_priority = desc->priority;
  // release(&desc->lock);

  // Check if we can handle nested interrupts
  int can_nest = 0;

  if (ALLOW_NESTED_INTERRUPTS) {
    // Only allow nesting if:
    // 1. We haven't exceeded max depth
    // 2. Current interrupt has higher priority (lower number) than previous
    if (interrupt_context.depth < MAX_INTERRUPT_DEPTH &&
        current_priority < interrupt_context.saved_priority) {
      can_nest = 1;
    }
  }

  // Enter interrupt context
  enter_interrupt();

  // Save old priority and set new one
  int old_priority = interrupt_context.saved_priority;
  interrupt_context.saved_priority = current_priority;

  // Enable interrupts if nesting is allowed and safe
  if (can_nest) {
    intr_on();
  }

  // Call all registered handlers (shared interrupt support)
  int handled = 0;
  // acquire(&desc->lock);

  for (int i = 0; i < MAX_HANDLERS_PER_IRQ; i++) {
    if (desc->handlers[i].valid) {
      // release(&desc->lock);

      // Call handler
      int ret = desc->handlers[i].handler(desc->handlers[i].dev_id);
      if (ret == IRQ_HANDLED)
        handled = 1;

      // acquire(&desc->lock);
    }
  }

  if (!handled) {
    desc->unhandled_count++;
  }

  // release(&desc->lock);

  // Disable interrupts before exit
  intr_off();

  // Restore priority
  interrupt_context.saved_priority = old_priority;

  // Exit interrupt context
  exit_interrupt();
}

//
// Enter interrupt context
//
void enter_interrupt(void) { interrupt_context.depth++; }

//
// Exit interrupt context
//
void exit_interrupt(void) {
  if (interrupt_context.depth > 0)
    interrupt_context.depth--;
}

//
// Check if we're in interrupt context
//
int in_interrupt(void) { return interrupt_context.depth > 0; }

//
// Get current interrupt nesting depth
//
int interrupt_depth(void) { return interrupt_context.depth; }

//
// Get interrupt count for an IRQ
//
uint64 get_irq_count(int irq) {
  if (irq < 0 || irq >= MAX_IRQS)
    return 0;

  return irq_descriptors[irq].count;
}

static void dump_trapframe(const struct trapframe *tf) {
  if (tf == 0)
    return;

  printf("  sepc=0x%lx\n", tf->sepc);
  printf("  sstatus=0x%lx\n", tf->sstatus);
  printf("  scause=0x%lx\n", tf->scause);
  printf("  stval=0x%lx\n", tf->stval);
}

void handle_exception(struct trapframe *tf) {
  if (tf == 0)
    panic("handle_exception: null trapframe");

  switch (tf->scause) {
  case 2: // Illegal instruction
    handle_illegal_instruction(tf);
    break;
  case 8: // System call from U-mode
    handle_syscall(tf);
    break;
  case 12: // Instruction page fault
    handle_instruction_page_fault(tf);
    break;
  case 5:  // Load access fault (e.g. MMIO without mapping)
  case 13: // Load page fault
    handle_load_page_fault(tf);
    break;
  case 7:  // Store/AMO access fault
  case 15: // Store/AMO page fault
    handle_store_page_fault(tf);
    break;
  default:
    printf("Unknown exception: scause=0x%lx\n", tf->scause);
    dump_trapframe(tf);
    tf->sepc += 4; // Skip the faulting instruction to avoid infinite loop
  }
}

//
// handle an interrupt, exception, or system call from user space.
// called from, and returns to, trampoline.S
// return value is user satp for trampoline.S to switch to.
//
// uint64 usertrap(void) {
// int which_dev = 0;

// if ((r_sstatus() & SSTATUS_SPP) != 0)
// panic("usertrap: not from user mode");

// // send interrupts and exceptions to kerneltrap(),
// // since we're now in the kernel.
// w_stvec((uint64)kernelvec); // DOC: kernelvec

// struct proc *p = myproc();

// // save user program counter.
// p->trapframe->epc = r_sepc();

// if (r_scause() == 8) {
// // system call

// if (killed(p))
// kexit(-1);

// // sepc points to the ecall instruction,
// // but we want to return to the next instruction.
// p->trapframe->epc += 4;

// // an interrupt will change sepc, scause, and sstatus,
// // so enable only now that we're done with those registers.
// intr_on();

// syscall();
// } else if ((which_dev = devintr()) != 0) {
// // ok
// } else if ((r_scause() == 15 || r_scause() == 13) &&
// vmfault(p->pagetable, r_stval(), (r_scause() == 13) ? 1 : 0) !=
// 0) {
// // page fault on lazily-allocated page
// } else {
// printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
// printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
// setkilled(p);
// }

// if (killed(p))
// kexit(-1);

// // give up the CPU if this is a timer interrupt.
// if (which_dev == 2)
// yield();

// prepare_return();

// // the user page table to switch to, for trampoline.S
// uint64 satp = MAKE_SATP(p->pagetable);

// // return to trampoline.S; satp value in a0.
// return satp;
// }

//
// set up trapframe and control registers for a return to user space
//
// void prepare_return(void) {
// struct proc *p = myproc();

// // we're about to switch the destination of traps from
// // kerneltrap() to usertrap(). because a trap from kernel
// // code to usertrap would be a disaster, turn off interrupts.
// intr_off();

// // send syscalls, interrupts, and exceptions to uservec in trampoline.S
// uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
// w_stvec(trampoline_uservec);

// // set up trapframe values that uservec will need when
// // the process next traps into the kernel.
// p->trapframe->kernel_satp = r_satp();         // kernel page table
// p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
// p->trapframe->kernel_trap = (uint64)usertrap;
// p->trapframe->kernel_hartid = r_tp(); // hartid for cpuid()

// // set up the registers that trampoline.S's sret will use
// // to get to user space.

// // set S Previous Privilege mode to User.
// unsigned long x = r_sstatus();
// x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
// x |= SSTATUS_SPIE; // enable interrupts in user mode
// w_sstatus(x);

// // set S Exception Program Counter to the saved user pc.
// w_sepc(p->trapframe->epc);
// }

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int devintr(void) {
  uint64 scause = r_scause();

  if (scause == 0x8000000000000009L) {
    // Supervisor external interrupt via PLIC
    int irq = plic_claim();

    if (irq > 0) {
      handle_irq(irq);
      plic_complete(irq);
    }

    return 1;
  } else if (scause == 0x8000000000000005L) {
    // Timer interrupt
    clockintr();
    return 2;
  } else {
    return 0;
  }
}