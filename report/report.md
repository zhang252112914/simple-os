

## 一、核心运行机制与流程图解 (Execution Flow Analysis)

### 场景一:从创建到运行 (Fork & Exec Flow)

#### 1.1 Fork系统调用的完整生命周期

当用户进程调用 `fork()` 时,系统将经历以下完整的状态转换链路:

**第一阶段:进程控制块的分配与初始化**

从 `allocproc()` 开始,内核需要在进程表中寻找一个状态为 `UNUSED` 的槽位。因为进程表是一个固定大小的数组(定义在 proc.c 中的 `struct proc proc[NPROC]`),所以内核必须通过线性扫描来定位空闲槽位:

```c
// 遍历进程表寻找UNUSED槽位
for(p = proc; p < &proc[NPROC]; p++) {
  acquire(&p->lock);
  if(p->state == UNUSED) {
    goto found;
  }
  release(&p->lock);
}
```

这里的关键设计点在于:**为什么要在检查 `p->state` 之前就持有锁?** 因为在多核系统中,如果不持锁,可能出现两个CPU同时发现同一个槽位为 `UNUSED`,从而导致进程表损坏。

**第二阶段:内核栈与上下文的初始化**

找到空闲槽位后,内核需要为新进程分配内核栈,并初始化其 `context` 结构。观察 `allocproc()` 中的关键代码:

```c
// 分配一页作为trapframe
if((p->trapframe = (struct trapframe *)kalloc()) == 0){
  freeproc(p);
  return 0;
}

// 初始化上下文,设置返回地址为forkret
memset(&p->context, 0, sizeof(p->context));
p->context.ra = (uint64)forkret;
p->context.sp = p->kstack + PGSIZE;
```

这里揭示了一个核心机制:**为什么 `context.ra` 要设置为 `forkret`?** 因为这个新进程此时并没有运行过,它没有真正的"返回地址"。当调度器第一次通过 `swtch()` 切换到这个进程时,会从 `context.ra` 指向的地址开始执行。因此,`forkret` 充当了"伪造的返回点",它会完成一些首次运行的初始化工作,然后跳转到用户态。

**第三阶段:地址空间的复制**

在 `fork()` 实现中,父进程的虚拟地址空间需要被完整复制:

```c
// 复制用户内存空间
if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
  freeproc(np);
  return -1;
}
np->sz = p->sz;
```

这里使用的是 `uvmcopy()` 函数,它逐页复制父进程的物理页面。因为RISC-V使用三级页表,所以复制过程必须遍历整个页表结构,为子进程建立完全独立的映射关系。

**第四阶段:Trapframe的伪造 — 解决"神奇返回"之谜**

这是 `fork` 机制中最精妙的部分。观察 `fork()` 中对 trapframe 的处理:

```c
// 复制父进程的trapframe
*(np->trapframe) = *(p->trapframe);

// 关键:设置子进程的返回值为0
np->trapframe->a0 = 0;
```

**逻辑推演:为什么子进程能从相同位置继续执行?**

因为 `trapframe->epc` 保存的是父进程调用 `fork()` 时的 **程序计数器值**。当父进程陷入内核态执行系统调用时,硬件自动将 `pc` 保存到 `sepc` 寄存器中,内核又将其保存到 `trapframe->epc`。当子进程首次被调度时,它会通过 `usertrapret()` 和 `trampoline.S` 中的 `sret` 指令返回用户态,此时硬件会将 `trapframe->epc` 加载到 `pc` 中,从而使子进程"恰好"从父进程的 `fork` 调用点继续执行。

但为什么返回值不同? 因为在 RISC-V 调用约定中,**系统调用的返回值存放在 `a0` 寄存器中**,而 `a0` 也被保存在 trapframe 中。父进程的 trapframe 保留了原始的 `a0` 值,所以父进程的 `fork()` 返回子进程的 PID;而子进程的 `trapframe->a0` 被人为设置为 0,所以子进程的 `fork()` 返回 0。

**第五阶段:状态标记与调度准备**

最后,子进程被标记为 `RUNNABLE`:

```c
acquire(&np->lock);
np->state = RUNNABLE;
release(&np->lock);
```

此时子进程进入调度队列,等待某个CPU的 `scheduler()` 循环选中它。

---

### 场景二:进程调度与切换回路 (The Scheduler Loop)

#### 2.1 从进程主动放弃CPU到重新获得CPU的完整路径

**起点:进程调用 `yield()`**

当一个进程时间片用完或主动让出CPU时,它会调用 `yield()`:

```c
void yield(void) {
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}
```

**关键逻辑点1:为什么要在持有 `p->lock` 的情况下修改 `p->state`?**

因为在多核系统中,如果不持锁,其他CPU的调度器可能正在读取这个进程的状态。如果出现"先修改状态,后持锁"的操作顺序,可能导致调度器观察到不一致的状态(例如看到进程是 `RUNNABLE`,但它实际上还在运行中)。

**传递点:`sched()` 的守门员职责**

`sched()` 是进程上下文切换的"守门员",它进行一系列检查:

```c
void sched(void) {
  struct proc *p = myproc();
  
  // 必须持有进程锁
  if(!holding(&p->lock))
    panic("sched p->lock");
  
  // 不能持有其他锁
  if(mycpu()->noff != 1)
    panic("sched locks");
  
  // 进程必须不在运行状态
  if(p->state == RUNNING)
    panic("sched running");
  
  // 中断必须关闭
  if(intr_get())
    panic("sched interruptible");

  // 切换到调度器上下文
  swtch(&p->context, &mycpu()->context);
}
```

**关键逻辑点2:为什么要检查 `mycpu()->noff != 1`?**

因为 `noff` 记录了当前CPU持有的锁的数量。如果 `noff > 1`,说明除了进程锁之外还持有其他锁。此时切换上下文会导致死锁 — 因为调度器可能会尝试获取这些已经被持有的锁。

**核心切换点:`swtch()` 的魔法**

`swtch()` 是整个操作系统中最底层的上下文切换实现:

```assembly
.globl swtch
swtch:
    # 保存旧上下文 (old context)
    sd ra, 0(a0)
    sd sp, 8(a0)
    sd s0, 16(a0)
    sd s1, 24(a0)
    sd s2, 32(a0)
    sd s3, 40(a0)
    sd s4, 48(a0)
    sd s5, 56(a0)
    sd s6, 64(a0)
    sd s7, 72(a0)
    sd s8, 80(a0)
    sd s9, 88(a0)
    sd s10, 96(a0)
    sd s11, 104(a0)

    # 加载新上下文 (new context)
    ld ra, 0(a1)
    ld sp, 8(a1)
    ld s0, 16(a1)
    ld s1, 24(a1)
    ld s2, 32(a1)
    ld s3, 40(a1)
    ld s4, 48(a1)
    ld s5, 56(a1)
    ld s6, 64(a1)
    ld s7, 72(a1)
    ld s8, 80(a1)
    ld s9, 88(a1)
    ld s10, 96(a1)
    ld s11, 104(a1)
    
    ret
```

**深度逻辑剖析:为什么切换栈指针就等于切换执行流?**

这是理解操作系统最核心的概念之一。让我们逐步推演:

1. **栈的本质是什么?** 栈不仅存储局部变量,更重要的是存储**函数调用链**。每次函数调用时,返回地址会被压入栈中。

2. **当 `sp` 被切换时发生了什么?** 假设进程A的栈指针是 `0x800001000`,调度器的栈指针是 `0x800002000`。当执行 `ld sp, 8(a1)` 时,如果 `a1` 指向调度器的 context,则 `sp` 被加载为 `0x800002000`。

3. **`ret` 指令做了什么?** `ret` 在RISC-V中等价于 `jr ra`,即跳转到 `ra` 寄存器指向的地址。因为我们刚刚执行了 `ld ra, 0(a1)`,所以 `ra` 现在指向调度器上下文中保存的返回地址。

4. **物理效果是什么?** CPU开始从新的栈上弹出函数调用帧,执行流完全切换到了另一个"世界"。如果这个上下文来自调度器,则CPU会继续执行 `scheduler()` 中 `swtch()` 之后的代码;如果来自某个进程,则会回到该进程的内核态代码。

**到达调度器循环**

当 `swtch()` 返回到 `scheduler()` 时,调度器继续它的无限循环:

```c
void scheduler(void) {
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    intr_on();
    
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);
        
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}
```

**关键逻辑点3:为什么在 `swtch` 之前需要持有 `p->lock`?**

因为从检查 `p->state == RUNNABLE` 到执行 `swtch` 之间,如果不持锁,可能出现以下竞态条件:
- CPU1 检查到进程P是 `RUNNABLE`
- CPU2 也检查到进程P是 `RUNNABLE`
- 两个CPU同时执行 `swtch`,试图运行同一个进程,导致灾难性后果

**返回点:新进程被唤醒**

当调度器选中某个 `RUNNABLE` 进程并执行 `swtch(&c->context, &p->context)` 时,CPU的执行流会"跳跃"到该进程上次调用 `sched()` 时保存的位置(即 `sched()` 中 `swtch()` 的下一条指令):

```c
void sched(void) {
  // ...
  swtch(&p->context, &mycpu()->context);
  // 进程在这里"醒来"!
}

void yield(void) {
  // ...
  sched();
  release(&p->lock);  // 进程继续执行这里
}
```

---

### 场景三:进程的消亡 (Exit & Wait Interaction)

#### 3.1 Exit的状态转换与资源回收机制

**起点:进程调用 `exit()`**

当进程完成工作或遇到错误时,它会调用 `exit()`:

```c
void exit(int status) {
  struct proc *p = myproc();

  // 关闭所有打开的文件
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  // 关闭当前工作目录
  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // 将子进程过继给init进程
  reparent(p);

  // 唤醒父进程
  wakeup(p->parent);

  acquire(&p->lock);
  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // 永不返回
  sched();
  panic("zombie exit");
}
```

**关键逻辑点1:为什么进程变成 `ZOMBIE` 而不是直接变成 `UNUSED`?**

因为父进程可能需要通过 `wait()` 获取子进程的退出状态(`xstate`)。如果立即释放进程控制块,这些信息就会丢失。`ZOMBIE` 状态表示:"我已经死了,但我的尸体还有用"。

**关键逻辑点2:为什么需要 `reparent()`?**

观察 `reparent()` 的实现:

```c
void reparent(struct proc *p) {
  struct proc *pp;
  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}
```

因为如果一个进程的所有子进程还没退出,它就直接消失了,这些子进程将成为"孤儿进程"。Unix的设计哲学是:**所有进程必须有父进程**。所以操作系统将这些孤儿过继给 `init` 进程(PID=1),由 `init` 负责回收它们。

**关键逻辑点3:`wait_lock` 和 `p->lock` 的双重保护**

注意代码中的锁的获取顺序:

```c
acquire(&wait_lock);
// ... reparent & wakeup ...
acquire(&p->lock);
p->state = ZOMBIE;
release(&wait_lock);
sched();
```

**为什么要先获取 `wait_lock`?** 因为父进程的 `wait()` 也会持有这个锁。这样可以保证在 `exit` 设置状态和 `wait` 读取状态之间不会出现竞态条件。

**为什么要在 `sched()` 之前 release `wait_lock`?** 因为 `sched()` 要求调用者只持有进程锁,不能持有其他锁(前文提到的 `noff == 1` 检查)。

#### 3.2 Wait的轮询与唤醒机制

**父进程的等待循环**

观察 `wait()` 的实现:

```c
int wait(uint64 addr) {
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        acquire(&pp->lock);
        havekids = 1;
        if(pp->state == ZOMBIE){
          // 找到僵尸子进程,回收资源
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, 
                                  (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }

    // 等待子进程退出
    sleep(p, &wait_lock);
  }
}
```

**关键逻辑点4:为什么 `wait()` 需要在循环中?**

因为 `sleep()` 可能被虚假唤醒(spurious wakeup)。在多核系统中,可能发生以下情况:
1. 进程A在等待子进程B
2. 子进程C(也是A的子进程)退出,调用 `wakeup(A)`
3. A被唤醒,但它发现C已经被回收了,B还没退出
4. A必须重新进入 `sleep`

因此,**永远不要假设被唤醒就意味着条件满足**,必须重新检查条件。

**关键逻辑点5:`sleep/wakeup` 的握手协议**

注意 `sleep` 的调用方式:

```c
sleep(p, &wait_lock);
```

这里传入了 `wait_lock`。观察 `sleep()` 的实现:

```c
void sleep(void *chan, struct spinlock *lk) {
  struct proc *p = myproc();
  
  acquire(&p->lock);
  release(lk);  // 先释放调用者的锁

  p->chan = chan;
  p->state = SLEEPING;

  sched();

  p->chan = 0;

  release(&p->lock);
  acquire(lk);  // 重新获取调用者的锁
}
```

**为什么要这样设计?** 因为如果不在 `sleep` 内部释放 `wait_lock`,会出现"丢失唤醒"问题:

**错误的设计(没有锁交接):**
```c
// 父进程
release(&wait_lock);
sleep(p, &wait_lock);  // 假设sleep不自动release

// 子进程
acquire(&wait_lock);
wakeup(parent);
release(&wait_lock);
```

时序分析:
1. 父进程释放 `wait_lock`
2. **此时子进程执行 `wakeup`,但父进程还没进入 `sleep`!**
3. 父进程进入 `sleep`,永久睡眠(因为唤醒信号已经错过了)

**正确的设计(原子锁交接):**
```c
// sleep内部保证:持有wait_lock -> 修改状态为SLEEPING -> 释放wait_lock
// 这样保证了wakeup不可能在状态修改之前执行
```

---

## 二、关键代码逻辑深度剖析 (Deep Code Logic)

### 2.1 `scheduler()` — 调度器的无限轮回

#### 核心代码结构分析

```c
void scheduler(void) {
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;  // 标记当前CPU没有运行任何进程
  
  for(;;){
    // 开启中断,允许时钟中断和设备中断
    intr_on();
    
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      
      if(p->state == RUNNABLE) {
        // 找到可运行进程
        p->state = RUNNING;
        c->proc = p;
        
        // 切换到进程的上下文
        swtch(&c->context, &p->context);
        
        // 当进程让出CPU时,会返回到这里
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}
```

#### 逐行逻辑注释

**Q: 为什么外层是无限循环?**

因为调度器是每个CPU核心的"永动机"。当系统启动后,每个CPU都会在 `start()` 和 `main()` 初始化完成后,进入 `scheduler()` 并永不退出。这是操作系统的基本运行模式:**CPU要么在运行用户进程,要么在运行调度器**。

**Q: 为什么要在循环开始时调用 `intr_on()`?**

因为在某些代码路径中(如 `sleep`),中断可能被关闭。如果调度器不重新开启中断,时钟中断将无法触发,进程调度将停止。但是,一旦 `swtch` 切换到进程上下文,该进程自己的中断状态将被恢复(因为 `swtch` 恢复了所有寄存器,包括 `sstatus`)。

**Q: 如果所有进程都在 `SLEEPING`,调度器在做什么?**

调度器会继续循环扫描进程表。在每次循环中:
1. 扫描所有进程,发现都是 `SLEEPING`
2. 循环结束,回到外层 `for(;;)`
3. 执行 `intr_on()`,重新扫描

在这个过程中,CPU实际上是"空转"的,在不断地轮询。这是一种简单但低效的实现。**更高级的操作系统会使用 `hlt` 指令让CPU进入低功耗状态,等待中断唤醒**。

**Q: 为什么在 `swtch` 之前必须持有 `p->lock`?**

这是防止多个CPU同时调度同一个进程的核心机制。考虑以下竞态条件:

```
时间线:
T1: CPU0 检查到进程P是 RUNNABLE
T2: CPU1 也检查到进程P是 RUNNABLE
T3: CPU0 执行 p->state = RUNNING
T4: CPU1 也执行 p->state = RUNNING (错误!)
T5: CPU0 和 CPU1 同时通过 swtch 切换到进程P
```

结果:两个CPU同时运行同一个进程,导致栈冲突、寄存器混乱等灾难性后果。

**通过持锁解决:**

```
时间线:
T1: CPU0 acquire(&p->lock)
T2: CPU0 检查到进程P是 RUNNABLE
T3: CPU1 尝试 acquire(&p->lock) -> 阻塞等待
T4: CPU0 执行 p->state = RUNNING
T5: CPU0 执行 swtch
T6: (稍后) 进程P调用 sched(),release(&p->lock)
T7: CPU1 获取锁,发现 p->state == RUNNING,跳过
```

#### 并发保护的细节推敲

观察 `swtch` 调用前后的锁状态:

```c
acquire(&p->lock);       // 调度器持有进程锁
swtch(&c->context, &p->context);
// swtch 返回时,仍然持有 p->lock!
c->proc = 0;
release(&p->lock);
```

**关键问题:为什么 `swtch` 返回后,锁还在调度器手里?**

因为 `swtch` 只是切换了寄存器和栈,**并没有执行任何锁操作**。锁的状态是通过全局变量(锁结构体中的 `locked` 字段)维护的,不会因为上下文切换而改变。

**那么,进程是如何释放这个锁的?**

当进程通过 `yield()` -> `sched()` -> `swtch()` 切换回调度器时,观察代码路径:

```c
// yield中:
acquire(&p->lock);
sched();
release(&p->lock);  // 在这里释放!
```

所以,**锁的所有权在逻辑上发生了转移**:
1. 调度器获取锁
2. 调度器切换到进程(物理上,锁仍然被持有)
3. 进程运行
4. 进程切换回调度器(物理上,锁仍然被持有)
5. 调度器释放锁

这种设计保证了:**从获取锁到释放锁的整个期间,进程的状态不会被其他CPU修改**。

---

### 2.2 `allocproc()` — 进程的诞生仪式

#### 核心代码结构

```c
static struct proc* allocproc(void) {
  struct proc *p;

  // 寻找UNUSED槽位
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // 分配trapframe页
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // 分配页表
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // 设置上下文,准备首次调度
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}
```

#### Trapframe 与 Context 的初始化逻辑

**Q: Trapframe 和 Context 的本质区别是什么?**

- **Trapframe**: 保存**用户态**的寄存器状态。当陷入内核(系统调用/中断)时,硬件和软件协同将用户态寄存器保存到 trapframe 中。定义在 proc.h:

```c
struct trapframe {
  uint64 kernel_satp;   // 内核页表
  uint64 kernel_sp;     // 内核栈指针
  uint64 kernel_trap;   // 陷阱处理函数地址
  uint64 epc;           // 用户程序计数器
  uint64 kernel_hartid; // hart ID
  uint64 ra;
  uint64 sp;
  uint64 gp;
  uint64 tp;
  // ... 其他通用寄存器 ...
  uint64 a0;            // 系统调用返回值
};
```

- **Context**: 保存**内核态**的寄存器状态。当进程在内核态主动让出CPU(如调用 `yield`)时,通过 `swtch` 将内核态寄存器保存到 context 中。定义在 proc.h:

```c
struct context {
  uint64 ra;   // 返回地址
  uint64 sp;   // 栈指针
  uint64 s0;   // 被调用者保存寄存器
  uint64 s1;
  // ... s2 ~ s11 ...
};
```

**Q: 为什么 Context 只保存 `ra`, `sp`, `s0-s11`?**

因为根据 RISC-V 调用约定:**被调用者保存寄存器**(callee-saved registers)包括 `s0-s11` 和 `ra`、`sp`。当函数调用发生时,被调用函数必须保证这些寄存器在返回时恢复原值。而 **调用者保存寄存器**(caller-saved registers,如 `a0-a7`, `t0-t6`)由调用者负责保存。

因此,`swtch` 只需要保存被调用者保存寄存器,因为它被视为一个"函数调用"。

#### "伪造上下文"的精妙设计

**Q: 为什么 `context.ra` 要设置为 `forkret`?**

这是操作系统中最巧妙的设计之一。让我们追踪一个新进程的首次调度:

1. **分配时刻**: `allocproc()` 设置 `p->context.ra = (uint64)forkret`
2. **调度时刻**: 调度器执行 `swtch(&c->context, &p->context)`
3. **`swtch` 内部**: 执行 `ld ra, 0(a1)`,将 `forkret` 地址加载到 `ra` 寄存器
4. **`swtch` 结尾**: 执行 `ret`,等价于 `jr ra`,跳转到 `forkret`

因此,`forkret` 充当了进程的"首次启动函数"。观察 `forkret()` 的实现:

```c
void forkret(void) {
  static int first = 1;

  release(&myproc()->lock);

  if (first) {
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}
```

**逻辑推演:为什么要释放 `myproc()->lock`?**

因为调度器在执行 `swtch` 之前持有了这个锁(见前文分析)。当 `swtch` 切换到新进程时,锁的所有权被"转移"了,但锁仍然被持有。新进程必须主动释放它。

**为什么不在调度器中释放?**

因为调度器不知道切换到的是一个"首次运行"的进程,还是一个"被中断后恢复"的进程。对于后者,锁会在 `yield()` 的末尾被释放(见前文)。所以,**首次运行的进程必须自己负责释放锁**。

**Q: 为什么 `context.sp` 设置为 `p->kstack + PGSIZE`?**

因为RISC-V的栈是向下增长的。`p->kstack` 是内核栈的起始地址(低地址),`p->kstack + PGSIZE` 是栈的顶部(高地址)。将 `sp` 设置为栈顶,意味着栈是空的,准备接受新的函数调用帧。

---

## 三、系统设计难点与解决方案 (Design Challenges)

### 3.1 死锁预防:Sleep/Wakeup 的锁协议

#### 问题背景:为什么需要锁?

考虑一个典型的生产者-消费者场景:

```c
// 消费者
while(buffer_empty()) {
  sleep(&buffer);
}
consume();

// 生产者
produce();
wakeup(&buffer);
```

**错误场景(无锁保护):**

```
时间线:
T1: 消费者检查 buffer_empty() -> true
T2: 生产者 produce(),调用 wakeup(&buffer)
T3: 消费者调用 sleep(&buffer) -> 永久睡眠!
```

因为在T1和T3之间,生产者已经发出了唤醒信号,但消费者错过了它。

#### 解决方案:条件锁(Condition Variable Lock)

在xv6中,`sleep` 和 `wakeup` 使用一个共享锁来保护条件检查和状态修改:

```c
// 消费者
acquire(&buffer_lock);
while(buffer_empty()) {
  sleep(&buffer, &buffer_lock);
}
consume();
release(&buffer_lock);

// 生产者
acquire(&buffer_lock);
produce();
wakeup(&buffer);
release(&buffer_lock);
```

**关键:sleep 的原子锁交接**

观察 `sleep()` 的实现:

```c
void sleep(void *chan, struct spinlock *lk) {
  struct proc *p = myproc();
  
  acquire(&p->lock);  // 1. 先获取进程锁
  release(lk);        // 2. 再释放条件锁

  p->chan = chan;
  p->state = SLEEPING;

  sched();            // 3. 切换上下文

  p->chan = 0;
  release(&p->lock);  // 4. 释放进程锁
  acquire(lk);        // 5. 重新获取条件锁
}
```

**逐步推演:为什么这样设计能避免死锁?**

**第一步:先获取进程锁**

因为要修改 `p->state`,必须持有 `p->lock`(这是多核并发保护的基本要求)。

**第二步:释放条件锁**

在这一步之前,进程同时持有两个锁(`p->lock` 和 `lk`)。如果不释放 `lk`,可能出现:
- 进程A在 `sleep` 中持有 `buffer_lock`
- 进程B想要 `produce`,尝试获取 `buffer_lock` -> 阻塞
- 进程B持有某个进程C的锁,导致调度器无法调度C
- 形成锁依赖环,死锁!

**第三步:在持有进程锁的情况下调用 `sched()`**

这符合 `sched()` 的要求(见前文:`noff == 1`)。

**第四步:唤醒后释放进程锁**

当进程被 `wakeup` 唤醒后,它会从 `sched()` 返回。此时调度器已经 release 了进程锁(在 `swtch` 返回后)... **等等,这里有个陷阱!**

**深度剖析:锁在 `swtch` 前后的传递**

让我们重新审视 `sleep` 和 `wakeup` 的完整路径:

**Sleep 路径:**
```c
// 进程A调用:
sleep(chan, &lk)
  acquire(&p->lock)     // A持有p->lock
  release(lk)
  sched()
    swtch(&p->context, &c->context)
    // CPU切换到调度器
```

**调度器路径:**
```c
scheduler()
  acquire(&p->lock)     // 调度器持有p->lock
  swtch(&c->context, &p->context)
  // CPU切换到进程A
  c->proc = 0
  release(&p->lock)     // 调度器释放p->lock
```

**错了!** 仔细看代码,调度器在 `swtch` **之前**就持有了 `p->lock`,并在 `swtch` **之后**释放它。那么,进程A何时释放这个锁?

**答案:进程A在首次 `sleep` 时永远不会释放 `p->lock`,而是"转移"给调度器!**

让我们重新梳理:

1. 进程A: `acquire(&p->lock)` -> 持有锁
2. 进程A: `swtch` -> 切换到调度器(锁仍然被持有,但逻辑上已转移给调度器)
3. 调度器:恢复执行,发现自己持有 `p->lock`
4. 调度器: `release(&p->lock)` -> 释放锁
5. (稍后)调度器选中进程A: `acquire(&p->lock)` -> 重新获取锁
6. 调度器: `swtch` -> 切换到进程A(锁被转移回进程A)
7. 进程A:从 `swtch` 返回,发现自己持有 `p->lock`
8. 进程A: `release(&p->lock)` -> 释放锁

所以,**锁在进程和调度器之间来回传递,但在整个 `sleep` -> `wakeup` -> `schedule` -> `resume` 的过程中,总有人持有它**,这保证了状态的一致性。

#### Wakeup 的实现细节

```c
void wakeup(void *chan) {
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}
```

**Q: 为什么要检查 `p != myproc()`?**

因为进程不能唤醒自己。如果一个进程正在调用 `wakeup`,它肯定不在 `SLEEPING` 状态。

**Q: 为什么要逐个持有每个进程的锁?**

因为要修改 `p->state`,必须持有 `p->lock`。这里采用的是"细粒度锁"策略:为每个进程单独持锁,而不是用一个全局锁保护整个进程表。这样可以提高并发性。

---

### 3.2 首次调度的"伪造上下文"技巧

#### 问题背景

新创建的进程从未运行过,它没有"上一次运行时保存的寄存器状态"。但调度器的 `swtch` 机制依赖于"恢复上次保存的上下文"。**如何让一个从未运行过的进程被调度上台?**

#### 解决方案:人为构造一个假的 Context

在 `allocproc()` 中:

```c
memset(&p->context, 0, sizeof(p->context));
p->context.ra = (uint64)forkret;
p->context.sp = p->kstack + PGSIZE;
```

这相当于告诉调度器:"这个进程上次运行到了 `forkret` 函数,它的栈在 `p->kstack + PGSIZE`"。

#### 首次调度的完整路径

**第一步:调度器选中新进程**

```c
scheduler()
  acquire(&p->lock);
  p->state = RUNNING;
  swtch(&c->context, &p->context);
```

**第二步:`swtch` 恢复"伪造的上下文"**

```assembly
swtch:
  # 保存调度器的上下文到 c->context
  sd ra, 0(a0)
  sd sp, 8(a0)
  # ...

  # 加载进程的上下文(这是伪造的!)
  ld ra, 0(a1)      # ra = forkret
  ld sp, 8(a1)      # sp = p->kstack + PGSIZE
  # ...

  ret               # 跳转到 ra,即 forkret
```

**第三步:进入 `forkret`**

```c
void forkret(void) {
  static int first = 1;

  release(&myproc()->lock);  // 释放调度器传递来的锁

  if (first) {
    first = 0;
    fsinit(ROOTDEV);         // 初始化文件系统(仅首次)
  }

  usertrapret();             // 返回用户态
}
```

**第四步:`usertrapret()` 准备用户态环境**

`usertrapret()` 会:
1. 设置 trapframe(包含用户态寄存器值)
2. 设置 RISC-V 的 `stvec` 指向用户态陷阱处理入口
3. 调用 `trampoline.S` 中的 `userret`

**第五步:通过 `sret` 返回用户态**

```assembly
userret:
  # 恢复用户态寄存器(从trapframe加载)
  ld ra, 40(a0)
  ld sp, 48(a0)
  # ... 恢复所有通用寄存器 ...
  
  ld a0, 112(a0)    # 恢复 a0(系统调用返回值)
  
  sret              # 返回用户态,pc = trapframe->epc
```

至此,进程完成了"从不存在 -> 内核态 -> 用户态"的完整转换。

#### 逻辑总结

**为什么这种"伪造"能成功?**

因为操作系统的上下文切换机制是**对称的**:它只关心寄存器值,不关心这些值从哪里来。通过人为设置 `ra` 和 `sp`,我们欺骗了 `swtch`,让它以为这个进程"曾经"在 `forkret` 中被中断,现在要恢复它。

这种技巧在操作系统中被称为 **"Trampoline"**(蹦床),因为它让执行流"跳跃"到一个新的起点,绕过了正常的函数调用路径。

---

## 四、总结与反思

本实验的进程管理与调度机制展现了操作系统设计的三个核心原则:

1. **抽象与封装**:通过 `struct proc` 和 `struct context`,将进程的复杂状态封装成可管理的数据结构。
2. **不变式(Invariant)保护**:通过锁机制,保证并发环境下的状态一致性(如"一个进程同一时刻只能被一个CPU运行")。
3. **对称性设计**:`swtch` 的对称性使得正常切换和首次调度可以用统一的机制处理。

本报告通过逐层剖析 `fork-exec-schedule-exit-wait` 的完整链路,揭示了看似简单的系统调用背后隐藏的深刻设计哲学。每一个锁的获取顺序、每一个寄存器的保存时机,都经过精心设计,以避免竞态条件和死锁。理解这些细节,是掌握操作系统内核开发的关键。

找到具有 1 个许可证类型的类似代码