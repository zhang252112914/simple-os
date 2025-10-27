# 实验报告：中断处理与时钟管理分析
## 一、中断/异常处理的总体架构

### 运行特权级别

- 内核运行在 RISC‑V 的 S（Supervisor）态；在平台初始化阶段可能使用 M（Machine）态完成硬件配置。用户程序以 U（User）态运行。
- 当发生 trap（异常或中断）时，硬件根据 stvec 指向的入口跳转到内核（通常的路径：kernelvec.S -> trampoline.S -> C 层的 trap 处理函数）。

### 关键寄存器

- sepc / stval / scause：保存陷入指令地址、异常的附加信息与原因编码。
- sstatus：包含中断使能位与当前特权级，需要在内核入口处保存/修改（例如清除 SIE、设置 SPIE 等）。
- sscratch：用于在用户态/内核态切换时保存临时数据（trampoline 使用）。
- PLIC / CLINT（平台外设寄存器）:
	- PLIC：外部中断控制器（设备中断仲裁，提供优先级和 claim/complete 机制）。
	- CLINT（或平台对应机制）：机器定时器 / S 模式定时器（mtime / mtimecmp）用于时钟中断。

### 硬件中断控制寄存器

- sie / sip：S 级别中断的使能与挂起位。
- PLIC 的 memory-mapped 寄存器：优先级、使能、claim/complete 寄存器（由 plic.c 操作）。

## 二、中断/异常处理流程（从触发到返回）

### 汇编层入口（低级）

- kernelvec.S / entry.S / trampoline.S 提供 trap 的低级入口。入口汇编负责尽量少量地保存寄存器，并把控制权转交给 C 层处理函数（例如 usertrap / kerneltrap）。
- trampoline.S 在用户态到内核态切换时负责把用户寄存器保存到进程的 trapframe、设置内核栈与调整 sstatus 等。

### C 层分派

- trap.c 中的主处理函数根据 scause 或平台中断来源分派：
	- 系统调用（ecall）：交给 syscall 子系统处理，返回时调整 sepc 跳过 ecall 指令。
	- 时钟中断（timer interrupt）：调用定时器处理器（如 timerintr / clockintr），更新系统时钟（ticks）并可能触发调度（yield）。
	- 外部设备中断：通过 devintr/PLIC 获取中断号并调用具体设备处理函数（如 uartintr），处理完成后调用 plic_complete。
	- 同步异常（页错误、非法指令等）：根据情况尝试处理（例如页缺失由 vm.c 处理）或将进程标记为 killed。


### 上下文保存与恢复

- 汇编入口先保存少量寄存器，随后在 C 层或者 trampoline 将用户所有通用寄存器保存到进程的 struct trapframe（定义在 trap.h / defs.h）。
- 返回用户态前由 trampoline 恢复 trapframe 中的寄存器并执行 sret，从而恢复 sstatus、sepc 并返回用户态指令流。

## 三、时钟（定时器）中断的实现与调度交互

### 触发机制

- 硬件层：CLINT（或平台等效）维护 mtime / mtimecmp，当 mtime 达到 mtimecmp 时产生定时器中断。S 模式下也可通过平台提供的 S-态定时器。
- 内核启动时会配置下一个定时器中断（写 mtimecmp）并使能 S-态定时器中断，使得周期性时钟中断能到达内核。

### 处理流程

- 中断到达后：汇编入口保存上下文 -> C 层 kerneltrap/devintr 分派到定时器处理函数（例如 timerintr）。
- timerintr 常执行的操作：
	- 更新系统时钟计数（例如 ticks++），唤醒等待定时器的进程（wakeup(&ticks)）。
	- 如果当前进程在用户态且时间片耗尽，调用 yield() 或设置需要调度的标志，从而触发上下文切换。

### 时钟与上下文切换

- 在时钟中断处理期间保存的 trapframe 可保证在进行进程切换时完整恢复用户态上下文。
- 调度器会保存必要的内核上下文（内核栈与 trapframe），选择下一个可运行进程，切换页表（satp）、内核栈指针，并通过 trampoline 恢复目标进程的 trapframe 并执行 sret 返回用户态。

## 四、异常处理机制

### 常见异常类型

- 同步异常：ecall（系统调用）、非法指令、页异常（加载/存储/取指缺页）、对齐错误等。

### 处理策略

- 系统调用：在 usertrap 中识别 ecall 并调用 syscall 子系统，最终将返回值放入寄存器（如 a0）并通过 usertrapret 返回。
- 页异常（缺页）：如果实现了虚拟内存（vm.c），内核会尝试通过加载页面或扩展栈来满足访问，否则将进程标记为 killed。
- 非法指令或严重异常：通常直接将进程标记为 killed，并在需要时打印调试信息；若在内核态发生则可能导致 panic。

### 诊断信息

- trap 处理程序会打印 scause、sepc、stval 等信息以便调试，输出通常通过 printf 或串口（uart）记录。

## 五、与 PLIC（外部中断控制器）的交互

### 工作流程

- 外部设备（如 UART）触发中断后，PLIC 将该中断置为 pending 并在 CPU 端提供可索取的中断号。
- 内核调用 plic_claim() 获取中断号，调用对应设备的处理函数（例如 uartintr），处理结束后调用 plic_complete(interrupt) 通知 PLIC 完成中断处理。

### 优点

- PLIC 的 claim/complete 模式避免每次中断时轮询所有设备寄存器，提高中断分派效率并支持优先级管理。

## 六、源代码中值得关注的位置（建议阅读顺序）

- kernelvec.S, entry.S, trampoline.S：中断入口与上下文保存/恢复的汇编实现。
- trap.c, trap.h：C 层主要分派函数（usertrap、kerneltrap、devintr、timerintr、usertrapret 等）。
- plic.c：PLIC 初始化、claim/complete 与设备分派逻辑。
- uart.c：串口中断处理示例（设备中断处理）。
- vm.c：与页面异常/缺页处理相关的实现。
- start.c, kernel.ld：特权级初始化、stvec 设置、时钟/PLIC 的初始使能。

## 七、实验验证建议（运行与观测）

1. 编译与启动

bash
make
# 或者仓库自带的 qemu 启动命令，例如：
# make qemu
# 或者手动执行（视 Makefile 与构件位置而定）：
# qemu-system-riscv64 -nographic -machine virt -kernel kernel/kernel


2. 观察时钟中断行为

- 在控制台或串口输出中查找 ticks 增长、定时唤醒或调度日志。
- 如需更详细观察，可在 trap.c 的定时器处理处添加打印（CPU id、当前 tick 等）。

3. 测试抢占性调度

- 启动两个持续占用 CPU 的用户进程（busy-loop），验证内核是否通过时钟中断周期性切换进程。

4. 测试外设中断

- 在串口发送/接收数据，观察 plic_claim/plic_complete 与 uartintr 的调用，以及串口中断的处理结果。

## 八、结论（要点回顾）

- 中断/异常处理采用汇编入口与 C 层分派相结合的设计，关键在于正确保存/恢复用户态上下文（trapframe）并维护 sepc、sstatus 等寄存器的语义。
- 时钟中断由 CLINT/mtimecmp（或平台等效）产生，内核在定时器处理器中更新 ticks、唤醒等待者并触发调度，从而实现抢占式多道调度。
- 外部中断通过 PLIC 进行仲裁，使用 claim/complete 模式交付中断号并交由相应设备处理器处理。
- 异常（系统调用、页错误、非法指令等）在 trap 处理程序中被分类处理：系统调用被服务，严重异常通常会导致进程终止。

## 参考（源码文件）

- kernelvec.S, trampoline.S, entry.S
- trap.c, trap.h
- plic.c
- uart.c
- vm.c, memlayout.h
- start.c, kernel.ld
