# 实验7：文件系统 - 深度技术报告

## 1. 文件系统分层架构图解 (Layered Architecture)

### 磁盘布局结构视图

```
+--------+-------+----------+----------+-------------+-------------+
| Boot   | Super |   Log    |  Inode   |   Bitmap    |    Data     |
| Block  | Block |  Blocks  |  Blocks  |   Blocks    |   Blocks    |
+--------+-------+----------+----------+-------------+-------------+
|   0    |   1   | 2..31    | 32..57   |    58       |   59...     |
         ↑
         sb.logstart=2, sb.inodestart=32, sb.bmapstart=58
```

### 数据流追踪：从 `write()` 到磁盘

当用户调用 `write(fd, buf, n)` 时，数据流经以下各层：

```
用户空间: write(fd, "hello", 5)
    ↓
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    ↓ 系统调用入口
┌─────────────────────────────────────────┐
│ 1. 系统调用层 (sysfile.c)               │
│    sys_write() → filewrite()            │
│    职责: 参数验证、文件描述符查找        │
└─────────────────────────────────────────┘
    ↓ struct file *f
┌─────────────────────────────────────────┐
│ 2. 文件描述符层 (file.c)                │
│    filewrite() → writei()               │
│    职责: 管理打开文件表、偏移量维护      │
└─────────────────────────────────────────┘
    ↓ struct inode *ip, off, n
┌─────────────────────────────────────────┐
│ 3. Inode 层 (fs.c)                      │
│    writei() → bmap() → log_write()      │
│    职责: 逻辑块→物理块映射、元数据管理   │
└─────────────────────────────────────────┘
    ↓ block number
┌─────────────────────────────────────────┐
│ 4. 日志层 (log.c)                       │
│    log_write() → 记录到日志             │
│    职责: 事务原子性、崩溃恢复保证        │
└─────────────────────────────────────────┘
    ↓ struct buf *
┌─────────────────────────────────────────┐
│ 5. 缓存层 (bio.c)                       │
│    bread()/bwrite() → virtio_disk_rw()  │
│    职责: 减少磁盘I/O、LRU缓存管理        │
└─────────────────────────────────────────┘
    ↓ 物理块数据
┌─────────────────────────────────────────┐
│ 6. 磁盘驱动层 (virtio_disk.c)           │
│    virtio_disk_rw()                     │
│    职责: 与硬件通信、DMA传输             │
└─────────────────────────────────────────┘
```

### 各层职责深度解析

#### **为什么有了 Buffer Cache 还需要 Log 层？**

这是一个关键的设计问题，两者解决的是**完全不同的问题域**：

| 层次 | 解决的问题 | 核心代码体现 |
|------|-----------|-------------|
| **Buffer Cache** | **性能问题** - 磁盘I/O慢，通过缓存减少物理读写次数 | `bio.c` 中 `bget()` 先查缓存，命中则直接返回 |
| **Log 层** | **一致性问题** - 系统崩溃时保证操作的原子性 | `log.c` 中 `commit()` 确保"全做或全不做" |

**具体场景说明**：

假设创建文件需要修改3个块：
1. 分配新 inode（修改 inode 块）
2. 添加目录项（修改父目录数据块）
3. 更新 bitmap（修改位图块）

如果只有 Buffer Cache，在步骤2完成后断电：
- 目录指向了一个未完全初始化的 inode
- **文件系统处于不一致状态**

有了 Log 层（`log.c`）：
```c
// kernel/log.c: commit() 的三阶段提交
static void commit() {
    if (log.lh.n > 0) {
        write_log();         // 1. 先写日志区（安全备份）
        write_head();        // 2. 原子性标记（n > 0 表示有效事务）
        install_trans(0);    // 3. 安装到真实位置
        log.lh.n = 0;        // 4. 清除日志
        write_head();        // 5. 标记事务完成
    }
}
```

---

## 2. 核心机制逻辑剖析 (Core Mechanisms Implementation)

### 2.1 Buffer Cache 与 LRU 策略 (`bio.c`)

#### 数据结构

```c
// kernel/buf.h
struct buf {
    int valid;         // 数据是否有效（从磁盘读取过）
    int disk;          // 是否正在进行磁盘I/O
    uint dev;          // 设备号
    uint blockno;      // 块号
    struct sleeplock lock;  // 睡眠锁：保护 buf 内容
    uint refcnt;       // 引用计数
    struct buf *prev;  // LRU 双向链表
    struct buf *next;
    uchar data[BSIZE]; // 实际数据（1024字节）
};

// kernel/bio.c
struct {
    struct spinlock lock;      // 自旋锁：保护链表结构
    struct buf buf[NBUF];      // 30个缓存块（param.h: NBUF=30）
    struct buf head;           // LRU 链表头（哨兵节点）
} bcache;
```

#### `bget()` 函数逻辑分析

```c
// kernel/bio.c
static struct buf* bget(uint dev, uint blockno) {
    struct buf *b;
    acquire(&bcache.lock);  // 获取自旋锁保护链表

    // ========== 阶段1: 缓存命中检查 ==========
    // 从 head.next（最近使用）向 head.prev（最久未用）遍历
    for(b = bcache.head.next; b != &bcache.head; b = b->next){
        if(b->dev == dev && b->blockno == blockno){
            b->refcnt++;        // 增加引用计数
            release(&bcache.lock);
            acquiresleep(&b->lock);  // 获取睡眠锁准备读写
            return b;
        }
    }

    // ========== 阶段2: 缓存未命中，LRU 替换 ==========
    // 关键：从 head.prev（链表尾部）向前遍历，找到 refcnt==0 的块
    // 这就是 LRU 策略的核心！
    for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
        if(b->refcnt == 0) {  // 找到无人使用的最旧块
            b->dev = dev;
            b->blockno = blockno;
            b->valid = 0;      // 标记数据无效，需要从磁盘读取
            b->refcnt = 1;
            release(&bcache.lock);
            acquiresleep(&b->lock);
            return b;
        }
    }
    panic("bget: no buffers");  // 所有缓存都被占用
}
```

#### LRU 链表维护 - `brelse()` 函数

```c
// kernel/bio.c
void brelse(struct buf *b) {
    // ...
    releasesleep(&b->lock);
    acquire(&bcache.lock);
    b->refcnt--;
    if (b->refcnt == 0) {
        // 关键：将释放的 buf 移动到链表头部
        // 从当前位置摘除
        b->next->prev = b->prev;
        b->prev->next = b->next;
        // 插入到 head 之后（最近使用位置）
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        bcache.head.next->prev = b;
        bcache.head.next = b;
    }
    release(&bcache.lock);
}
```

**LRU 链表状态示意**：
```
使用后释放的 buf 移到头部：
         最近使用                              最久未用
head <-> [刚释放的buf] <-> [buf2] <-> ... <-> [buf_n] <-> head
  ↑                                               ↑
  head.next (优先保留)               head.prev (优先替换)
```

#### 锁的双重设计分析

| 锁类型 | 变量 | 保护对象 | 为什么选择这种锁？ |
|--------|------|---------|-------------------|
| **自旋锁** | `bcache.lock` | 整个缓存链表结构 | 操作快速（只是遍历/修改指针），不涉及I/O等待 |
| **睡眠锁** | `b->lock` | 单个 buf 的数据内容 | 读写磁盘需要等待I/O完成，可能阻塞很长时间 |

**为什么这样设计？**

```c
// 如果查找缓存时用睡眠锁：
// 1. 进程A持有bcache锁，等待磁盘I/O
// 2. 进程B想查找另一个块，必须等A的I/O完成
// 3. 严重的性能瓶颈！

// 正确的设计（代码实现）：
acquire(&bcache.lock);     // 快速获取自旋锁
// ... 快速遍历链表找到目标buf ...
release(&bcache.lock);     // 立即释放，允许其他进程查找
acquiresleep(&b->lock);    // 只对单个buf持有睡眠锁
// ... 可能阻塞的磁盘I/O操作 ...
```

---

### 2.2 日志系统与崩溃恢复 (`log.c`)

#### 日志数据结构

```c
// kernel/log.c
struct logheader {
    int n;              // 当前事务中的块数量
    int block[LOGSIZE]; // 每个日志块对应的真实块号
};

struct log {
    struct spinlock lock;
    int start;          // 日志区起始块号（来自 sb.logstart）
    int size;           // 日志区大小
    int outstanding;    // 当前活跃的系统调用数量（关键！）
    int committing;     // 是否正在提交
    int dev;
    struct logheader lh;
} log;
```

#### 事务原子性：`begin_op()` 和 `end_op()`

```c
// kernel/log.c
void begin_op(void) {
    acquire(&log.lock);
    while(1){
        if(log.committing){
            // 等待：有事务正在提交，不能开始新操作
            sleep(&log, &log.lock);
        } else if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGSIZE){
            // 等待：日志空间不足，需要先提交现有事务
            sleep(&log, &log.lock);
        } else {
            log.outstanding++;  // 标记"有一个新的操作在进行"
            release(&log.lock);
            break;
        }
    }
}

void end_op(void) {
    int do_commit = 0;
    acquire(&log.lock);
    log.outstanding--;          // 操作完成，计数减1
    if(log.committing)
        panic("log.committing");
    if(log.outstanding == 0){   // 关键判断：所有操作都完成了吗？
        do_commit = 1;          // 是的，可以提交了
        log.committing = 1;
    } else {
        // 还有其他操作在进行，唤醒等待者
        wakeup(&log);
    }
    release(&log.lock);

    if(do_commit){
        commit();               // 真正提交事务
        acquire(&log.lock);
        log.committing = 0;
        wakeup(&log);           // 唤醒等待提交完成的操作
        release(&log.lock);
    }
}
```

**事务判断逻辑图解**：
```
时间线：
                begin_op()              end_op()
进程A:    ────────[outstanding=1]──────────[outstanding=0, commit!]
                        ↓
进程B:           begin_op()     end_op()
          ──────────────[outstanding=2]────[outstanding=1, 不提交]
```

#### 提交过程 `commit()` 详解

```c
static void commit() {
    if (log.lh.n > 0) {
        write_log();      // 步骤1: Write to log
        write_head();     // 步骤2: 标记日志有效
        install_trans(0); // 步骤3: Install to home
        log.lh.n = 0;
        write_head();     // 步骤4: Erase log
    }
}
```

**四步提交的详细分析**：

```
┌──────────────────────────────────────────────────────────┐
│ 步骤1: write_log() - 将修改写入日志区                    │
├──────────────────────────────────────────────────────────┤
│ for (tail = 0; tail < log.lh.n; tail++) {               │
│     struct buf *lbuf = bread(log.dev, log.start+tail+1);│
│     struct buf *dbuf = bread(log.dev, log.lh.block[tail]);│
│     memmove(lbuf->data, dbuf->data, BSIZE);             │
│     bwrite(lbuf);  // 写入日志区块 log.start+1, +2, ... │
│ }                                                        │
│ 磁盘状态: [日志头:n=0] [日志块1:数据] [日志块2:数据]     │
└──────────────────────────────────────────────────────────┘
                           ↓
┌──────────────────────────────────────────────────────────┐
│ 步骤2: write_head() - 原子性标记日志有效                 │
├──────────────────────────────────────────────────────────┤
│ 将 log.lh (n=块数, block[]数组) 写入磁盘 log.start 块   │
│ 磁盘状态: [日志头:n=3,blocks={25,32,58}] [日志数据...]  │
│                                                          │
│ ★ 关键点：一旦这步完成，即使断电，重启后也能恢复！       │
└──────────────────────────────────────────────────────────┘
                           ↓
┌──────────────────────────────────────────────────────────┐
│ 步骤3: install_trans() - 安装到真实位置                  │
├──────────────────────────────────────────────────────────┤
│ for (tail = 0; tail < log.lh.n; tail++) {               │
│     struct buf *lbuf = bread(log.dev, log.start+tail+1);│
│     struct buf *dbuf = bread(log.dev, log.lh.block[tail]);│
│     memmove(dbuf->data, lbuf->data, BSIZE);             │
│     bwrite(dbuf);  // 写入真实块位置                     │
│ }                                                        │
│ 磁盘状态: 真实数据块已更新                               │
└──────────────────────────────────────────────────────────┘
                           ↓
┌──────────────────────────────────────────────────────────┐
│ 步骤4: 清除日志 (log.lh.n = 0; write_head();)           │
├──────────────────────────────────────────────────────────┤
│ 磁盘状态: [日志头:n=0] - 事务完成，日志可重用            │
└──────────────────────────────────────────────────────────┘
```

#### 崩溃恢复 `recover_from_log()`

```c
// kernel/log.c
static void recover_from_log(void) {
    read_head();              // 从磁盘读取日志头
    install_trans(1);         // 如果 log.lh.n > 0，重放日志
    log.lh.n = 0;
    write_head();             // 清除日志
}
```

**崩溃场景分析**：

| 崩溃时机 | 日志头状态 | 恢复行为 |
|---------|-----------|---------|
| 步骤1完成前 | `n = 0` | 无需恢复，事务未开始 |
| 步骤1~2之间 | `n = 0` | 无需恢复，日志未标记有效 |
| 步骤2完成后，步骤3之前 | `n > 0` | **重放日志！** `install_trans(1)` |
| 步骤3~4之间 | `n > 0` | **重放日志** (幂等操作，重复写入无影响) |
| 步骤4完成后 | `n = 0` | 无需恢复，事务已完成 |

**原子性保证的关键**：
- 日志头的更新是**单块写入**，在大多数磁盘上是原子的
- `n > 0` 表示有完整的待提交事务
- `n = 0` 表示没有有效事务

---

### 2.3 Inode 映射与间接块 (`fs.c`)

#### Inode 结构

```c
// kernel/fs.h
#define NDIRECT 12              // 12个直接块
#define NINDIRECT (BSIZE / sizeof(uint))  // 256个间接块 (1024/4)
#define MAXFILE (NDIRECT + NINDIRECT)     // 最大文件块数: 268

struct dinode {
    short type;           // 文件类型
    short major;          // 设备主号
    short minor;          // 设备次号
    short nlink;          // 硬链接数
    uint size;            // 文件大小（字节）
    uint addrs[NDIRECT+1]; // 12个直接块 + 1个间接块指针
};
```

#### `bmap()` 函数深度分析

```c
// kernel/fs.c
static uint bmap(struct inode *ip, uint bn) {
    uint addr, *a;
    struct buf *bp;

    // ========== 情况1: 直接块 (bn < 12) ==========
    if(bn < NDIRECT){
        if((addr = ip->addrs[bn]) == 0)
            ip->addrs[bn] = addr = balloc(ip->dev);  // 按需分配
        return addr;
    }
    bn -= NDIRECT;  // 转换为间接块内的偏移

    // ========== 情况2: 间接块 (bn < 256) ==========
    if(bn < NINDIRECT){
        // 加载间接块（如果不存在则分配）
        if((addr = ip->addrs[NDIRECT]) == 0)
            ip->addrs[NDIRECT] = addr = balloc(ip->dev);
        
        bp = bread(ip->dev, addr);  // 读取间接块内容
        a = (uint*)bp->data;        // 间接块包含256个块号
        
        if((addr = a[bn]) == 0){
            a[bn] = addr = balloc(ip->dev);  // 按需分配数据块
            log_write(bp);
        }
        brelse(bp);
        return addr;
    }
    panic("bmap: out of range");
}
```

**`bmap()` 映射可视化**：

```
文件逻辑视图:
┌───────────────────────────────────────────────────────────────────┐
│ 逻辑块号:  0   1   2  ... 11  │  12   13   14  ...  267          │
│            ↓   ↓   ↓      ↓   │   ↓    ↓    ↓        ↓           │
└───────────────────────────────────────────────────────────────────┘
                                  
Inode addrs[] 数组:
┌──────────────────────────────────────┬─────────────────────┐
│ addrs[0] addrs[1] ... addrs[11]      │ addrs[12] (间接块)  │
│   ↓        ↓            ↓            │     ↓               │
│ 直接块号  直接块号    直接块号        │  ┌──────────────┐   │
│                                      │  │ block[0]=... │   │
│  bn=0 → addrs[0]                     │  │ block[1]=... │   │
│  bn=5 → addrs[5]                     │  │ ...          │   │
│  bn=11 → addrs[11]                   │  │ block[255]=..│   │
└──────────────────────────────────────┴──┴──────────────┴───┘
                                            bn=12 → block[0]
                                            bn=13 → block[1]
                                            bn=267 → block[255]
```

#### 最大文件大小计算

```
直接块数量: NDIRECT = 12
间接块数量: NINDIRECT = BSIZE / sizeof(uint) = 1024 / 4 = 256

最大文件块数: MAXFILE = 12 + 256 = 268 块
每块大小: BSIZE = 1024 字节

最大文件大小 = 268 × 1024 = 274,432 字节 ≈ 268 KB
```

**扩展性分析**：如果要支持更大文件，可以添加：
- **双重间接块**: 256 × 256 = 65,536 块 → +64 MB
- **三重间接块**: 256 × 256 × 256 块 → +16 GB

---

## 3. 并发控制与一致性 (Concurrency & Consistency)

### 3.1 Inode 锁与引用计数

#### `iget()` - 获取 inode 引用

```c
// kernel/fs.c
static struct inode* iget(uint dev, uint inum) {
    struct inode *ip, *empty;
    acquire(&itable.lock);  // 保护全局 inode 表

    empty = 0;
    for(ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++){
        if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
            ip->ref++;          // 找到：增加引用计数
            release(&itable.lock);
            return ip;          // 注意：不持有 ip->lock 返回！
        }
        if(empty == 0 && ip->ref == 0)
            empty = ip;         // 记住空闲槽位
    }

    // 未找到：使用空闲槽位
    ip = empty;
    ip->dev = dev;
    ip->inum = inum;
    ip->ref = 1;
    ip->valid = 0;              // 需要从磁盘读取
    release(&itable.lock);
    return ip;
}
```

#### `iput()` - 释放 inode 引用

```c
// kernel/fs.c
void iput(struct inode *ip) {
    acquire(&itable.lock);
    
    if(ip->ref == 1 && ip->valid && ip->nlink == 0){
        // 最后一个引用，且无硬链接：需要删除
        acquiresleep(&ip->lock);  // 获取锁准备删除
        release(&itable.lock);
        
        itrunc(ip);               // 释放所有数据块
        ip->type = 0;             // 标记 inode 为空闲
        iupdate(ip);              // 写回磁盘
        ip->valid = 0;
        
        releasesleep(&ip->lock);
        acquire(&itable.lock);
    }
    ip->ref--;
    release(&itable.lock);
}
```

#### 引用计数与锁的协作关系

| 字段 | 作用 | 保护范围 |
|------|------|---------|
| `ref` (引用计数) | 内存中的 inode 结构体是否可以释放 | 由 `itable.lock` 保护 |
| `nlink` (硬链接数) | 磁盘上的 inode 是否可以删除 | 持久化在磁盘，由 `ip->lock` 保护修改 |
| `ip->lock` (睡眠锁) | 读写 inode 内容（type, size, addrs[]等） | 单个 inode 的字段 |

#### 并发删除场景分析

```
时间线：
进程A: open("/file") → iget() → ref=1 → read()...
进程B:        unlink("/file") → nlink-- → nlink=0
进程A:                                          → close() → iput()

iput() 中的判断：
if(ip->ref == 1 && ip->valid && ip->nlink == 0)
    ↓
此时 ref=1, nlink=0 → 触发真正的删除！
```

**安全保证**：
1. 进程A 持有 `ref=1`，`iput()` 不会释放内存结构
2. 进程B 的 `unlink()` 只是减少 `nlink`
3. 只有当 `ref=1` (最后一个使用者) **且** `nlink=0` (无硬链接) 时才真正删除

---

### 3.2 路径解析与交接锁 (Hand-over-Hand Locking)

#### `namex()` 函数分析

```c
// kernel/fs.c
static struct inode* namex(char *path, int nameiparent, char *name) {
    struct inode *ip, *next;

    // 确定起始目录
    if(*path == '/')
        ip = iget(ROOTDEV, ROOTINO);    // 绝对路径：从根开始
    else
        ip = idup(myproc()->cwd);        // 相对路径：从当前目录开始

    // 逐级解析路径组件
    while((path = skipelem(path, name)) != 0){
        ilock(ip);                       // 锁定当前目录
        
        if(ip->type != T_DIR){
            iunlockput(ip);              // 不是目录，解锁并释放
            return 0;
        }
        
        if(nameiparent && *path == '\0'){
            // 找父目录模式，且已到达最后一个组件
            iunlock(ip);
            return ip;
        }
        
        // 在当前目录中查找下一级
        if((next = dirlookup(ip, name, 0)) == 0){
            iunlockput(ip);
            return 0;
        }
        
        iunlockput(ip);                  // ★ 关键：先解锁当前
        ip = next;                       // ★ 再切换到下一级
    }
    
    // ... 返回结果
}
```

#### 交接锁机制图解

解析路径 `/a/b/c`：

```
状态转换:

1. ip = iget(root)           [root: ref=1, unlocked]

2. ilock(root)               [root: ref=1, LOCKED]
   next = dirlookup(root, "a")
   
3. iunlockput(root)          [root: ref=0, unlocked] → 可能被回收
   ip = iget("/a")           [/a: ref=1, unlocked]

4. ilock(/a)                 [/a: ref=1, LOCKED]
   next = dirlookup(/a, "b")
   
5. iunlockput(/a)            [/a: ref=0, unlocked]
   ip = iget("/a/b")         [/a/b: ref=1, unlocked]

6. ilock(/a/b)               [/a/b: ref=1, LOCKED]
   next = dirlookup(/a/b, "c")
   
7. iunlockput(/a/b)          [/a/b: ref=0, unlocked]
   ip = iget("/a/b/c")       [/a/b/c: ref=1, unlocked]

8. 返回 ip                    [/a/b/c: ref=1, unlocked]
```

**交接锁 (Hand-over-Hand) 的关键特性**：

```
错误做法（全程持锁）：
lock(root) → lock(/a) → lock(/a/b) → lock(/a/b/c)
问题：持有多个锁，容易死锁！

正确做法（交接锁）：
lock(root) → unlock(root), lock(/a) → unlock(/a), lock(/a/b) → ...
优点：任何时刻只持有一个目录锁
```

**交接锁的作用**：

1. **防止死锁**：避免同时持有多个目录锁
2. **提高并发**：释放父目录锁后，其他进程可以访问父目录
3. **保证一致性**：在持有锁期间，目录内容不会被修改

---

## 总结：分层设计的智慧

```
┌─────────────────────────────────────────────────────────────┐
│                    文件系统设计哲学                          │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  问题分解:                                                  │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
│  │ 性能问题    │  │ 一致性问题  │  │ 并发问题    │         │
│  │             │  │             │  │             │         │
│  │ Buffer Cache│  │ Log 层      │  │ 多级锁设计  │         │
│  │ LRU 替换    │  │ 事务原子性  │  │ 引用计数    │         │
│  └─────────────┘  └─────────────┘  └─────────────┘         │
│                                                             │
│  设计原则:                                                  │
│  1. 分层抽象：每层只关注一个问题                            │
│  2. 原子操作：日志保证"全做或全不做"                        │
│  3. 细粒度锁：提高并发度，避免全局阻塞                      │
│  4. 惰性操作：按需分配块，写时复制                          │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    解析 "/home/user/file.txt" 完整过程                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  初始状态：                                                                  │
│  path = "/home/user/file.txt"                                               │
│  ip = iget(ROOTDEV, ROOTINO) = 根目录 inode (inum=1)                        │
│                                                                             │
├─────────────────────────────────────────────────────────────────────────────┤
│  第1轮循环：                                                                 │
│  ┌─────────────────────────────────────────────────────────────────────────┐│
│  │ skipelem("/home/user/file.txt") → name="home", path="user/file.txt"    ││
│  │                                                                         ││
│  │ ilock(ip)           // 锁定根目录                                       ││
│  │ ip->type == T_DIR   // 检查通过                                         ││
│  │                                                                         ││
│  │ dirlookup(根目录, "home") :                                             ││
│  │   遍历根目录内容:                                                        ││
│  │   ┌────────┬──────────┐                                                 ││
│  │   │ inum=1 │ "."      │                                                 ││
│  │   │ inum=1 │ ".."     │                                                 ││
│  │   │ inum=3 │ "home" ★ │ → 找到！返回 iget(dev, 3)                        ││
│  │   │ inum=4 │ "etc"    │                                                 ││
│  │   └────────┴──────────┘                                                 ││
│  │                                                                         ││
│  │ next = home 目录的 inode (inum=3)                                       ││
│  │ iunlockput(ip)      // 解锁并释放根目录                                  ││
│  │ ip = next           // 切换到 home 目录                                  ││
│  └─────────────────────────────────────────────────────────────────────────┘│
│                                                                             │
├─────────────────────────────────────────────────────────────────────────────┤
│  第2轮循环：                                                                 │
│  ┌─────────────────────────────────────────────────────────────────────────┐│
│  │ skipelem("user/file.txt") → name="user", path="file.txt"               ││
│  │                                                                         ││
│  │ ilock(ip)           // 锁定 home 目录                                   ││
│  │ ip->type == T_DIR   // 检查通过                                         ││
│  │                                                                         ││
│  │ dirlookup(home目录, "user") :                                           ││
│  │   ┌────────┬──────────┐                                                 ││
│  │   │ inum=3 │ "."      │                                                 ││
│  │   │ inum=1 │ ".."     │                                                 ││
│  │   │ inum=5 │ "user" ★ │ → 找到！返回 iget(dev, 5)                        ││
│  │   │ inum=8 │ "guest"  │                                                 ││
│  │   └────────┴──────────┘                                                 ││
│  │                                                                         ││
│  │ next = user 目录的 inode (inum=5)                                       ││
│  │ iunlockput(ip)      // 解锁并释放 home 目录                              ││
│  │ ip = next           // 切换到 user 目录                                  ││
│  └─────────────────────────────────────────────────────────────────────────┘│
│                                                                             │
├─────────────────────────────────────────────────────────────────────────────┤
│  第3轮循环：                                                                 │
│  ┌─────────────────────────────────────────────────────────────────────────┐│
│  │ skipelem("file.txt") → name="file.txt", path=""                        ││
│  │                                                                         ││
│  │ ilock(ip)           // 锁定 user 目录                                   ││
│  │ ip->type == T_DIR   // 检查通过                                         ││
│  │                                                                         ││
│  │ dirlookup(user目录, "file.txt") :                                       ││
│  │   ┌────────┬──────────────┐                                             ││
│  │   │ inum=5 │ "."          │                                             ││
│  │   │ inum=3 │ ".."         │                                             ││
│  │   │ inum=10│ "file.txt" ★ │ → 找到！返回 iget(dev, 10)                   ││
│  │   │ inum=12│ "notes.md"   │                                             ││
│  │   └────────┴──────────────┘                                             ││
│  │                                                                         ││
│  │ next = file.txt 的 inode (inum=10)                                      ││
│  │ iunlockput(ip)      // 解锁并释放 user 目录                              ││
│  │ ip = next           // 切换到 file.txt                                   ││
│  └─────────────────────────────────────────────────────────────────────────┘│
│                                                                             │
├─────────────────────────────────────────────────────────────────────────────┤
│  第4轮循环：                                                                 │
│  ┌─────────────────────────────────────────────────────────────────────────┐│
│  │ skipelem("") → 返回 0 (NULL)                                            ││
│  │                                                                         ││
│  │ while 循环退出                                                           ││
│  └─────────────────────────────────────────────────────────────────────────┘│
│                                                                             │
├─────────────────────────────────────────────────────────────────────────────┤
│  返回结果：                                                                  │
│  return ip;  // file.txt 的 inode (inum=10, ref=1, 未锁定)                  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```