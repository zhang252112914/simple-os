// Interrupt priority levels (0 = highest, 7 = lowest)
#define IRQ_PRIORITY_CRITICAL 0
#define IRQ_PRIORITY_HIGH 2
#define IRQ_PRIORITY_NORMAL 4
#define IRQ_PRIORITY_LOW 6
#define IRQ_PRIORITY_IDLE 7

// Interrupt handler return values
#define IRQ_HANDLED 1
#define IRQ_NOT_HANDLED 0

#define ALLOW_NESTED_INTERRUPTS 1

// Interrupt handler type
typedef int (*interrupt_handler_t)(void *dev_id);

// Structure for interrupt handler registration
struct irq_handler {
  interrupt_handler_t handler;
  void *dev_id; // Device identifier for shared interrupts
  char *name;   // Handler name for debugging
  int valid;    // Is this slot occupied?
};

// IRQ descriptor
struct irq_desc {
  struct irq_handler handlers[MAX_HANDLERS_PER_IRQ];
  // struct spinlock lock;   // Protects this IRQ descriptor
  int priority;           // Priority level (0-7)
  int enabled;            // Is this IRQ enabled?
  uint64 count;           // Number of times this IRQ fired
  uint64 unhandled_count; // Number of unhandled interrupts
};

struct trapframe {
  uint64 sepc;    // Saved supervisor exception program counter
  uint64 sstatus; // Saved supervisor status register
  uint64 stval;   // Trap value (faulting address)
  uint64 scause;  // Trap cause code
};
