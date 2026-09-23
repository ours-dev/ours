# mm 待修项（2026-09-23）

## 1. AP 上的第一次 kmalloc 挂死：per-CPU `CpuCache` 解析到内核 `.bss` 里

**严重程度：阻塞。** 任何在 CPU1..3 上执行的内核分配都会挂死，挡住了 P0 的第 7 步——用户态第一个需要
内核分配的系统调用（`channel_create`）。

### 现象

shell 跑在 CPU1 上。`sys_channel_create` → `ChannelDispatcher::create()` 的第一次
`heap::kmalloc(160, GAF_KERNEL)` 永不返回，日志里什么都没有。

QEMU monitor 抓到的现场（`kernel.dev.obi`，dbg.0）：

- CPU1 固定在 `SpinLock<Option 4>::acquire() + 0x20`（`pause; movq (%rdi),%rax; testq; jne`），
  `%rdi = 0xffffff8840081f48`；调用链
  `MemCache::allocate(Gaf, u32, CpuCache *)` ← `MemCache::allocate(Gaf, u32)` ←
  `ChannelDispatcher::create` ← `sys_channel_create` ← `syscall_dispatch` ← `x86_syscall`。
- 那个「锁」在内核镜像的 `.bss` 里：`llvm-nm` 显示它位于 `heap::KMALLOC_CACHE`（`0x81ba0`，大小 `0x550`）
  内部偏移 `0x3a8`。锁字的值是 `0xffffff8840081fd0`——一个指针，不是「持有者 CPU 号 + 1」。
  所以 CPU1 在一块并不是锁的内存上空转。
- 调用点的反汇编：`this->m_cpu_caches`（`0x60(%rdi)`）加上 `%gs` 里的本 CPU 偏移，得到的 `CpuCache *`
  就是那块 `.bss`。`CPU_OFFSETS[0] = 0xffffffffbff41000`、`CPU_OFFSETS[1] = 0xffffffffbff58000`，与两个 CPU
  的 GS 基址一致，`%gs` 本身没问题；有问题的是 kmalloc-256 这个 cache 的 `m_cpu_caches` 句柄——用它在
  CPU0 上解析得到 `.bss` 的 `0x6af48`，在 CPU1 上得到 `0x81f48`，都不在 physmap 里的 per-CPU 单元中。
- 启动期的所有分配都在 CPU0 上，所以一直没暴露；CPU0 上这个句柄同样解析到 `.bss`，意味着 CPU0 上的
  kmalloc-256 分配很可能一直在悄悄改写内核 `.bss` 里的别的对象。

### 已排除

- 不是 syscall 期间开中断（`f34e49b`）引起的：临时撤掉 `local_irq_enable()` 照样挂死。
- 不是本系列 `.bss` 变大（2^12 个句柄槽）引起的：把 arena 缩到 256 槽照样挂死。

### 怀疑方向

`KMALLOC_CACHE` 在 `create_boot()` 阶段建立，时间很可能早于 `CpuLocal::init()` 把 CPU0 从临时单元迁到
正式单元。`cpu_local.cpp` 的注释说动态 chunk 的句柄「原样保留到迁移之后」；要核对 kmalloc 各级 cache 的
`m_cpu_caches` 句柄是否真的相对于 `canonical_base()` 计算，还是相对于临时单元的地址算出来的。

### 复现

在 `docs/changes/260923/syscall.md` 最后一节所述提交之上，打上 shell 自环测试（`channel_create` 起头），启动
`kernel.dev.obi`：shell 打完 `close again = BadHandle` 后再无输出。
