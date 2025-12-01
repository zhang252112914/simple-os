# 系统调用实验报告：系统设计原理与实现逻辑分析

## 1. 系统调用全链路追踪 (The System Call Lifecycle)

以 `getpid()` 系统调用为例，完整追踪从用户态到内核态再返回的全过程。

### 1.1 触发阶段：用户空间到硬件陷入

```
┌─────────────────────────────────────────────────────────────────┐
│  用户程序                                                        │
│  pid = getpid();                                                │
└──────────────────────────┬──────────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│  usys.S (由 usys.pl 生成)                                        │
│  .global getpid                                                 │
│  getpid:                                                        │
│      li a7, SYS_getpid    # 将系统调用号 11 加载到 a7 寄存器      │
│      ecall                 # 触发环境调用异常                    │
│      ret                   # 返回用户程序                        │
└──────────────────────────┬──────────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│  RISC-V 硬件响应 ecall 指令                                      │
│  1. scause ← 8 (Environment call from U-mode)                   │
│  2. sepc ← PC (保存 ecall 指令的地址)                            │
│  3. sstatus.SPP ← 0 (记录来自用户模式)                           │
│  4. sstatus.SIE ← 0 (禁用中断)                                   │
│  5. PC ← stvec (跳转到陷阱处理程序入口)                          │
└─────────────────────────────────────────────────────────────────┘
```

**为什么需要 `a7` 寄存器？**

`usys.pl` 脚本为每个系统调用生成汇编存根代码：

```perl
sub entry {
    my $name = shift;
    print ".global $name\n";
    print "${name}:\n";
    print " li a7, SYS_${name}\n";  # 系统调用号放入 a7
    print " ecall\n";                # 触发陷阱
    print " ret\n";
}
```

RISC-V 调用约定规定 `a0-a7` 用于参数传递，其中 `a7` 专门用于承载系统调用号。这样设计的原因是：`a0-a5` 留给系统调用的实际参数，`a0` 还要用于返回值。

**硬件做了什么？**

当 `ecall` 执行时，CPU 硬件自动完成以下操作：
- **`scause`** 寄存器被设置为 8（用户态环境调用），这让内核知道陷入原因
- **`sepc`** 保存当前 PC（即 `ecall` 指令地址），用于返回时恢复执行位置
- **`sstatus.SPP`** 位记录陷入前的特权级别（0 表示用户态）
- PC 跳转到 **`stvec`** 寄存器指向的地址

### 1.2 入口与分发：Trampoline 与上下文切换

```
┌─────────────────────────────────────────────────────────────────┐
│  stvec 指向 TRAMPOLINE (虚拟地址最高页)                          │
│  为什么需要 trampoline？                                         │
│  - 用户进程和内核都需要访问这段代码                              │
│  - 它被映射到每个进程页表的固定位置 (TRAMPOLINE)                  │
│  - 切换页表前后，这个虚拟地址都有效                              │
└──────────────────────────┬──────────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│  trampoline.S :: uservec                                        │
│                                                                 │
│  # 此时还在用户页表下运行！                                      │
│  # a0 被用作临时寄存器，需要先保存                               │
│                                                                 │
│  1. csrw sscratch, a0      # 临时保存 a0 到 sscratch             │
│  2. li a0, TRAPFRAME       # a0 ← trapframe 地址                 │
│  3. sd ra, 40(a0)          # 保存所有通用寄存器到 trapframe       │
│     sd sp, 48(a0)                                               │
│     ... (保存 t0-t6, s0-s11, a0-a7 等)                          │
│  4. csrr t0, sscratch      # 取回原 a0 值                        │
│     sd t0, 112(a0)         # 保存原 a0                           │
└──────────────────────────┬──────────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│  切换到内核环境                                                  │
│                                                                 │
│  ld sp, 8(a0)              # sp ← 内核栈指针 (trapframe->kernel_sp)│
│  ld tp, 32(a0)             # tp ← hartid (trapframe->kernel_hartid)│
│  ld t0, 16(a0)             # t0 ← usertrap 函数地址              │
│  ld t1, 0(a0)              # t1 ← 内核页表 satp 值               │
│  csrw satp, t1             # 切换到内核页表！                    │
│  sfence.vma zero, zero     # 刷新 TLB                            │
│  jr t0                     # 跳转到 usertrap()                   │
└──────────────────────────┬──────────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│  trap.c :: usertrap()                                           │
│                                                                 │
│  w_stvec((uint64)kernelvec);  # 切换陷阱向量到内核模式           │
│  p->trapframe->epc = r_sepc(); # 保存用户 PC                     │
│                                                                 │
│  if(r_scause() == 8) {        # 检查是否是系统调用               │
│      p->trapframe->epc += 4;  # 返回时跳过 ecall 指令            │
│      intr_on();               # 允许中断                         │
│      syscall();               # 调用系统调用分发器               │
│  }                                                              │
└──────────────────────────┬──────────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│  syscall.c :: syscall()                                         │
│                                                                 │
│  int num = p->trapframe->a7;  # 从 a7 获取系统调用号             │
│                                                                 │
│  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {        │
│      p->trapframe->a0 = syscalls[num]();  # 执行并存储返回值     │
│  } else {                                                       │
│      p->trapframe->a0 = -1;   # 非法调用号返回 -1                │
│  }                                                              │
└─────────────────────────────────────────────────────────────────┘
```

**为什么需要 Trampoline 页？**

这是一个精妙的设计。问题在于：当 CPU 执行 `ecall` 后跳转到陷阱处理代码时，**页表还是用户进程的页表**。但陷阱处理代码在内核中，如果直接切换页表，当前正在执行的代码地址就会失效！

解决方案是 Trampoline（蹦床）页：
1. 它被映射到**每个用户进程页表**和**内核页表**的相同虚拟地址 (`TRAMPOLINE = MAXVA - PGSIZE`)
2. 无论使用哪个页表，这个虚拟地址都指向同一段物理内存
3. 这样在切换页表的瞬间，代码地址仍然有效

**系统调用号如何映射到内核函数？**

在 `syscall.c` 中，通过函数指针数组实现分发：

```c
static uint64 (*syscalls[])(void) = {
    [SYS_fork]    sys_fork,
    [SYS_exit]    sys_exit,
    [SYS_wait]    sys_wait,
    // ...
    [SYS_getpid]  sys_getpid,  // syscalls[11] = sys_getpid
};
```

`syscall()` 函数从 `trapframe->a7` 读取系统调用号，直接作为数组索引找到对应函数。

### 1.3 返回机制：从内核态回到用户态

```
┌─────────────────────────────────────────────────────────────────┐
│  内核函数执行完毕                                                │
│  sys_getpid() 返回 p->pid                                       │
│  返回值被 syscall() 写入 p->trapframe->a0                        │
└──────────────────────────┬──────────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│  trap.c :: usertrapret()                                        │
│                                                                 │
│  intr_off();              # 关中断                               │
│  w_stvec(TRAMPOLINE + (uservec - trampoline)); # 恢复用户陷阱向量│
│                                                                 │
│  # 准备 trapframe 供 userret 使用                                │
│  p->trapframe->kernel_satp = r_satp();                          │
│  p->trapframe->kernel_sp = p->kstack + PGSIZE;                  │
│  p->trapframe->kernel_trap = (uint64)usertrap;                  │
│  p->trapframe->kernel_hartid = r_tp();                          │
│                                                                 │
│  # 设置 sstatus，准备返回用户态                                  │
│  w_sepc(p->trapframe->epc);  # 恢复用户 PC                       │
│                                                                 │
│  # 跳转到 trampoline 的 userret                                  │
│  uint64 fn = TRAMPOLINE + (userret - trampoline);               │
│  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);                │
└──────────────────────────┬──────────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│  trampoline.S :: userret                                        │
│                                                                 │
│  csrw satp, a1            # 切换到用户页表                       │
│  sfence.vma zero, zero    # 刷新 TLB                             │
│                                                                 │
│  # 从 trapframe 恢复所有用户寄存器                               │
│  ld ra, 40(a0)                                                  │
│  ld sp, 48(a0)                                                  │
│  ld a0, 112(a0)           # 最后恢复 a0（包含系统调用返回值）    │
│                                                                 │
│  sret                     # 返回用户态                           │
│  # 硬件自动：PC ← sepc, 特权级 ← sstatus.SPP                    │
└─────────────────────────────────────────────────────────────────┘
```

**返回值传递的关键路径：**
1. `sys_getpid()` 返回 `myproc()->pid`
2. `syscall()` 将返回值写入 `p->trapframe->a0`
3. `userret` 从 trapframe 恢复 `a0` 寄存器
4. 用户程序从 `a0` 读取到返回值

---

## 2. 参数传递与内存屏障 (Data Transfer & Memory Safety)

### 2.1 参数提取机制

系统调用参数通过 RISC-V 调用约定存放在 `a0-a5` 寄存器中，这些值在陷入时被保存到 `trapframe`。

```c
// syscall.c - 参数提取函数

// 获取第 n 个参数（通用寄存器值）
static uint64 argraw(int n)
{
  struct proc *p = myproc();
  switch (n) {
  case 0: return p->trapframe->a0;
  case 1: return p->trapframe->a1;
  case 2: return p->trapframe->a2;
  case 3: return p->trapframe->a3;
  case 4: return p->trapframe->a4;
  case 5: return p->trapframe->a5;
  }
  panic("argraw");
  return -1;
}

// 获取整数参数
int argint(int n, int *ip)
{
  *ip = argraw(n);
  return 0;
}

// 获取地址参数
int argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
  return 0;
}
```

**设计解析：**
- `argraw()` 使用 switch 语句直接从 trapframe 的对应字段读取参数
- `argint()` 和 `argaddr()` 是类型安全的包装函数
- 参数编号 0-5 对应 `a0-a5` 寄存器

**Trapframe 结构中的寄存器布局：**

```c
// proc.h
struct trapframe {
  uint64 kernel_satp;   //   0 - 内核页表
  uint64 kernel_sp;     //   8 - 内核栈
  uint64 kernel_trap;   //  16 - usertrap 地址
  uint64 epc;           //  24 - 用户 PC
  uint64 kernel_hartid; //  32 - CPU 核心 ID
  uint64 ra;            //  40
  uint64 sp;            //  48
  // ... 其他寄存器
  uint64 a0;            // 112 - 第一个参数/返回值
  uint64 a1;            // 120
  uint64 a2;            // 128
  // ...
  uint64 a7;            // 168 - 系统调用号
};
```

### 2.2 跨空间数据拷贝：安全边界

**核心问题：为什么内核不能直接解引用用户指针？**

1. **地址空间隔离**：用户指针是用户页表下的虚拟地址，内核运行在内核页表下
2. **安全威胁**：恶意用户可能传递指向内核空间的地址，试图读取/破坏内核数据
3. **无效指针**：用户可能传递未映射的地址，直接解引用会导致内核崩溃

**解决方案：页表遍历验证**

```c
// vm.c - copyout: 从内核拷贝数据到用户空间
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    
    // 关键：通过 walkaddr 查找用户页表，验证地址合法性
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;  // 地址未映射，拒绝操作
    
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    
    // 使用物理地址直接写入
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// vm.c - copyin: 从用户空间拷贝数据到内核
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}
```

**`walkaddr` 的安全验证逻辑：**

```c
// vm.c
uint64 walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  // 安全检查 1：地址不能超过最大用户虚拟地址
  if(va >= MAXVA)
    return 0;

  // 通过页表遍历找到 PTE
  pte = walk(pagetable, va, 0);
  
  // 安全检查 2：PTE 必须存在
  if(pte == 0)
    return 0;
  
  // 安全检查 3：页面必须有效（V 位）
  if((*pte & PTE_V) == 0)
    return 0;
  
  // 安全检查 4：必须是用户可访问的页面（U 位）
  if((*pte & PTE_U) == 0)
    return 0;
  
  pa = PTE2PA(*pte);
  return pa;
}
```

**字符串获取的安全实现：**

```c
// vm.c - fetchstr: 安全地从用户空间获取字符串
int fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  
  // 使用 copyinstr，它会：
  // 1. 验证每个页面的映射
  // 2. 检查 '\0' 终止符
  // 3. 限制最大长度
  if(copyinstr(p->pagetable, buf, addr, max) < 0)
    return -1;
  
  return strlen(buf);
}

int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  
  if(got_null){
    return 0;
  } else {
    return -1;  // 字符串未正常终止
  }
}
```

---

## 3. 核心系统调用实现解析 (Key Implementation Details)

### 3.1 `sys_write` - 文件写入的完整路径

```c
// sysfile.c
uint64 sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  // 步骤 1：提取参数
  // a0 = fd (文件描述符)
  // a1 = buf (用户缓冲区地址)  
  // a2 = n (写入字节数)
  argaddr(1, &p);
  argint(2, &n);
  
  // 步骤 2：通过 argfd 验证并获取 file 结构
  if(argfd(0, 0, &f) < 0)
    return -1;
  
  // 步骤 3：调用文件系统层写入
  return filewrite(f, p, n);
}
```

**`argfd` - 文件描述符验证与查找：**

```c
static int argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  
  // 边界检查 1：fd 必须非负
  // 边界检查 2：fd 必须小于 NOFILE (16)
  if(fd < 0 || fd >= NOFILE)
    return -1;
  
  // 边界检查 3：进程的 ofile 数组中该位置必须有效
  if((f = myproc()->ofile[fd]) == 0)
    return -1;
  
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}
```

**`filewrite` - 分发到具体设备：**

```c
// file.c
int filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  // 检查文件是否可写
  if(f->writable == 0)
    return -1;

  if(f->type == FD_PIPE){
    // 管道写入
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    // 设备写入（如控制台）
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // 普通文件写入
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();  // 开始日志事务
      ilock(f->ip);
      
      // writei 会调用 copyin 从用户空间读取数据
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      
      iunlock(f->ip);
      end_op();  // 提交事务

      if(r != n1)
        break;
      i += r;
    }
    ret = (i == n ? n : -1);
  }
  return ret;
}
```

**数据流图：**

```
用户调用 write(fd, buf, n)
         │
         ▼
    sys_write()
         │
         ├─ argfd() ─────────────────────────┐
         │   验证 fd 范围 [0, NOFILE)         │
         │   查找 proc->ofile[fd]            │
         │                                   ▼
         │                          struct file
         │                          ├─ type (PIPE/DEVICE/INODE)
         │                          ├─ ref (引用计数)
         │                          ├─ readable/writable
         │                          ├─ ip (inode 指针)
         │                          └─ off (文件偏移)
         │
         ▼
    filewrite(f, addr, n)
         │
         ├─── FD_DEVICE ───▶ devsw[major].write()
         │                   └─ 如 consolewrite()
         │
         ├─── FD_PIPE ─────▶ pipewrite()
         │
         └─── FD_INODE ────▶ writei(ip, user=1, addr, off, n)
                                    │
                                    ▼
                            copyin() 安全拷贝用户数据
                                    │
                                    ▼
                            bread()/bwrite() 磁盘 I/O
```

### 3.2 `sys_sbrk` - 进程堆内存管理

```c
// sysproc.c
uint64 sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);  // 获取增长/收缩的字节数
  addr = myproc()->sz;  // 保存当前堆顶地址
  
  // 调用 growproc 调整进程大小
  if(growproc(n) < 0)
    return -1;
  
  return addr;  // 返回调整前的堆顶
}
```

**`growproc` - 进程内存调整的核心：**

```c
// proc.c
int growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  
  if(n > 0){
    // 增长内存
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    // 收缩内存
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  
  p->sz = sz;
  return 0;
}
```

**`uvmalloc` - 分配并映射新页面：**

```c
// vm.c
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  // 将 oldsz 向上取整到页边界
  oldsz = PGROUNDUP(oldsz);
  
  for(a = oldsz; a < newsz; a += PGSIZE){
    // 步骤 1：调用物理内存分配器获取一页
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    
    // 步骤 2：清零新页面
    memset(mem, 0, PGSIZE);
    
    // 步骤 3：在页表中建立映射
    // 权限：可读 | 用户可访问 | 传入的额外权限（如可写）
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}
```

**内存分配流程图：**

```
sys_sbrk(n=4096)  // 请求增加一页
      │
      ▼
  growproc(4096)
      │
      ▼
  uvmalloc(pagetable, sz, sz+4096, PTE_W)
      │
      ├─────────────────────────────────────────┐
      │                                         │
      ▼                                         ▼
  kalloc()                              mappages()
  从空闲链表取一页                        修改页表
      │                                         │
      ▼                                         ▼
  返回物理地址 pa                         walk() 遍历三级页表
      │                                   创建/更新 PTE
      │                                         │
      ▼                                         ▼
  memset(pa, 0, 4096)                     PTE = PA | PTE_V | PTE_R | 
  清零新页面                                     PTE_U | PTE_W

进程页表变化：
┌─────────────────┐
│ 代码段 (.text)   │ 0x0
├─────────────────┤
│ 数据段 (.data)   │
├─────────────────┤
│ 原堆区           │ ← 原 p->sz
├─────────────────┤
│ 新分配页面       │ ← 新 p->sz = 原sz + 4096
└─────────────────┘
```

**`uvmdealloc` - 收缩内存并释放页面：**

```c
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
    // 参数 1 表示释放物理页面 (kfree)
  }

  return newsz;
}
```

---

## 4. 安全性与边界处理 (Security & Edge Cases)

### 4.1 非法指针检测

**场景 1：用户传递内核空间地址**

```c
// vm.c - walkaddr 中的检查
uint64 walkaddr(pagetable_t pagetable, uint64 va)
{
  // 关键安全检查：地址不能超过 MAXVA
  // MAXVA = (1L << (9 + 9 + 9 + 12 - 1)) = 0x4000000000
  // 内核空间从 KERNBASE (0x80000000) 开始
  if(va >= MAXVA)
    return 0;  // 拒绝访问
  
  pte_t *pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  
  // 关键检查：页面必须有 PTE_U 标志（用户可访问）
  // 内核页面没有这个标志
  if((*pte & PTE_U) == 0)
    return 0;
  
  return PTE2PA(*pte);
}
```

**为什么 PTE_U 检查能防护内核地址？**

在 `kvminit()` 中，内核映射内核页面时**不设置 PTE_U 位**：

```c
// vm.c
void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  // 注意：这里没有 PTE_U
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}
```

而用户页面映射时会设置 PTE_U：

```c
// vm.c - uvmalloc
if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0)
```

**场景 2：用户传递未映射的地址**

```c
// copyin/copyout 的防护
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    
    // 如果地址未在用户页表中映射，walkaddr 返回 0
    if(pa0 == 0)
      return -1;  // 安全拒绝，返回错误而非崩溃
    
    // ...
  }
  return 0;
}
```

### 4.2 文件描述符边界检查

```c
// sysfile.c - argfd 的完整检查
static int argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  
  // 检查 1：负数文件描述符
  if(fd < 0)
    return -1;
  
  // 检查 2：超过最大值 (NOFILE = 16)
  if(fd >= NOFILE)
    return -1;
  
  // 检查 3：该 fd 槽位是否有有效的 file 结构
  if((f = myproc()->ofile[fd]) == 0)
    return -1;
  
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}
```

**实际应用示例：**

```c
uint64 sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  
  // 如果 argfd 返回 -1，整个系统调用返回 -1
  if(argfd(0, 0, &f) < 0)
    return -1;
  
  return fileread(f, p, n);
}

uint64 sys_close(void)
{
  int fd;
  struct file *f;

  // 不仅验证，还获取 fd 值用于清空 ofile 槽位
  if(argfd(0, &fd, &f) < 0)
    return -1;
  
  myproc()->ofile[fd] = 0;  // 安全清空
  fileclose(f);
  return 0;
}
```

### 4.3 系统调用号验证

```c
// syscall.c
void syscall(void)
{
  int num;
  struct proc *p = myproc();

  num = p->trapframe->a7;
  
  // 三重验证：
  // 1. num > 0: 排除非法的 0 和负数
  // 2. num < NELEM(syscalls): 不超过数组边界
  // 3. syscalls[num] != 0: 该位置有有效函数指针
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    p->trapframe->a0 = syscalls[num]();
  } else {
    printf("%d %s: unknown sys call %d\n",
            p->pid, p->name, num);
    p->trapframe->a0 = -1;  // 返回错误码
  }
}
```

### 4.4 字符串长度限制

```c
// sysfile.c - sys_exec 中的路径长度检查
uint64 sys_exec(void)
{
  char path[MAXPATH];  // MAXPATH = 128
  // ...

  // fetchstr 内部使用 copyinstr，最多复制 MAXPATH 字节
  if(argstr(0, path, MAXPATH) < 0) {
    // ...
    return -1;
  }
  // ...
}

// syscall.c
int argstr(int n, char *buf, int max)
{
  uint64 addr;
  argaddr(n, &addr);
  return fetchstr(addr, buf, max);  // max 限制拷贝长度
}
```

**copyinstr 的终止检测：**

```c
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  // ...
  int got_null = 0;

  while(got_null == 0 && max > 0){
    // ...
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      }
      // ...
      --max;  // 递减剩余可拷贝字节数
    }
  }
  
  if(got_null){
    return 0;
  } else {
    return -1;  // 未找到终止符，字符串过长
  }
}
```

---

## 总结

### 设计原则

| 方面 | 实现策略 | 目的 |
|------|----------|------|
| **地址空间隔离** | Trampoline 双重映射 | 页表切换时代码连续性 |
| **参数传递** | 通过 Trapframe 读取 a0-a5 | 标准化接口 |
| **内存安全** | walkaddr + PTE_U 检查 | 防止内核内存泄露 |
| **边界检查** | argfd 多重验证 | 防止数组越界 |
| **错误处理** | 统一返回 -1 | 用户态可感知错误 |

### 关键寄存器角色

| 寄存器 | 用途 |
|--------|------|
| `a0-a5` | 系统调用参数 |
| `a7` | 系统调用号 |
| `a0` | 返回值 |
| `sepc` | 保存/恢复用户 PC |
| `scause` | 陷入原因 (8 = 用户态 ecall) |
| `stvec` | 陷阱处理入口地址 |
| `satp` | 页表基址 |
| `sscratch` | 临时寄存器保存 |

找到具有 1 个许可证类型的类似代码