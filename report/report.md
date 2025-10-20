## 设计1：内存布局
### 物理内存
- 低地址空间：设备寄存器（如 UART、VIRTIO、PLIC、CLINT 等）通常位于固定物理地址区间。
- 内核镜像（kernel binary）：linker 将内核放在物理内存0x80000000开始的空间中。
- 物理页分配区：kalloc 从内核镜像 end 之后到 PHYSTOP 的物理内存中分配空闲页（kalloc.c 管理）。
- PHYSTOP：物理内存上限，内核不会使用高于该物理地址的物理内存（memlayout.h 中定义）。

### 虚拟内存
整体是一个三级页表，每级512个条目，页大小：PGSIZE = 4096 字节。每个 PTE 是 64 位：高位存放物理页号(PPN)，低位若干位为标志位（PTE_V, PTE_R, PTE_W, PTE_X, PTE_U 等），实现里用宏 PA2PTE/PTE2PA、PTE_FLAGS 操作这些位。
[63:39] 符号扩展（MAXVA 限制）
[38:30] VPN[2]（level2 索引）
[29:21] VPN[1]（level1 索引）
[20:12] VPN[0]（level0 索引）
[11:0] 页内偏移

### 特殊：内核空间
由于内核空间实际上是直接映射，所以可以直接看作 VA = PA

## 设计2：内存管理
### 物理内存管理
#### 组织
物理内存按照页大小被分为物理页，所有物理页构成一个空闲链表
#### 分配与释放
kalloc从链表中拿出第一个空闲页面并用memset填充垃圾字节，kfree一方面做对齐检查，不小于end且小于PHYSTOP，同时把页面清1并放回链表

### 虚拟内存管理
#### 查找
- walk：给定 pagetable 与虚地址 va，逐层读取 PTE；若需要且 alloc 非 0，会用 kalloc 分配新的页表页并在上层 PTE 中安装（置 PTE_V）。返回指向第 0 级的 PTE 的指针（叶 PTE）。
- walkaddr：仅用于用户地址，查找 va 映射到的物理地址（页对齐偏移保留），并验证该 PTE 对用户可读，最终返回对应的物理地址。
#### 分配
##### 基础操作
- mappages：为从 va 开始长度为 size（必须页对齐）的虚地址区间逐页建立到从 pa 开始的物理页的映射，权限由 perm 指定。0 成功，-1 如果 walk() 在需要时无法分配页表页。
- pagetable_t uvmcreate：分配并返回一个新页表的虚拟地址，若 kalloc 返回 0，则返回 0。
##### 分配
- uvmalloc：把进程从 oldsz 增大到 newsz（不小于 oldsz），为新增空间分配物理页并映射到用户页表。将 oldsz 向上取整为页边界，从 oldsz 到 newsz 每页调用 kalloc()、清零、mappages(..., PTE_R|PTE_U|xperm)；若 kalloc 或 mappages 失败，调用 uvmdealloc 回滚并返回 0。
#### 释放
##### 基础操作
- uvmunmap：对每页调用 walk(..., alloc=0) 获取 PTE（若 PTE 页表页不存在则跳过）；若 PTE_V 清零并在 do_free 时调用 kfree 释放物理页（通过 PTE2PA 获取物理地址）。
- uvmdealloc：缩减进程地址空间大小，从 oldsz 减到 newsz；释放多余页面及其映射。
##### 大规模释放
- freewalk：递归释放一个页表及其所有下层页表页（前提是所有叶映射已被移除）。
- uvmfree：先释放从 0 到 sz 的所有用户页面（如果 sz>0），然后释放页表结构本身（调用 freewalk）。
#### 复制
- uv吗copy：在 fork 时把父进程 old 的前 sz 字节内存复制到子进程 new：为每个已映射页分配新物理页并复制数据，然后在子页表上映射新页。

## 设计3：初始化
- kinit：初始化物理内存，调用freerange初始化所有物理内存的初始状态。
- kvminit：创建初始的内核页表，一方面是构建内核页表，另一方面是完成了初始的一些映射（按照memlayout.h）。
- kvminithart：设置指向页表的指针，以此转向虚拟内存的工作模式。