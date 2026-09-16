# CpuLocal — 改动记录

---

## `Dyn::owns()` 改成无锁，解开一条 debug 构建里的自锁环（2026-09-15）

**文件**
- `kernel/mm/cpu_local.cpp` — `Dyn` 新增一张发布式的 extent 表，`owns()` 不再取 `m_lock`；
  `Mapper::NUM_SLOTS` 提到 public 供 Dyn 定表长。

**症状**
潜伏，当前触发不到。触发条件是任何一次落在动态区、且需要 `grow()` 的 per-CPU 分配，
发生在 `KERNEL_DEBUG_LEVEL` 非零的构建里。chunk 0 给每个 CPU 留了
`DYN_FIRST_CHUNK_SIZE`（16 KiB），现在还没有用满过，所以 `grow()` 至今没被调到。
一旦调到，单 CPU 上就是死锁，多 CPU 上是同一把锁的自旋。

**原因**
`CpuLocal::resolve()` 的两个重载都以 `DEBUG_ASSERT(holds(handle), ...)` 开头，而
`holds()` 对不在静态区的 handle 会问 `Dyn::owns()`，后者原本第一行就是
`Guard guard{m_lock}`。于是：

```
Dyn::allocate()                     取 m_lock
  → grow()                          （仍持锁）
    → Mapper::alloc_chunk()
      → alloc_frames()
        → PmZone::alloc_frame()
          → m_frame_cache.with_current(...)   m_frame_cache 是 PerCpu<PcpuCache>
            → PerCpu::ref_mut()
              → CpuLocal::resolve(handle)
                → DEBUG_ASSERT(holds(handle))
                  → Dyn::owns()     再取 m_lock   ← 环闭合
```

环能闭合，是因为 buddy 分配器自己也用 per-CPU 存储：`PmZone::m_frame_cache`。
也就是说「per-CPU 分配器向下要内存」和「内存分配器向上取 per-CPU 缓存」这两条路
在同一把锁上撞了。

但更值得记的不是这条环，而是**一个只在 debug 下存在的断言取了一把全局锁**。
`resolve()` 压在每一次 per-CPU 访问下面，给它加一把自旋锁，等于让调试构建的并发行为
和 release 构建不一样——而调试构建存在的意义恰恰是复现 release 的行为。断言可以变慢，
不可以改变锁的拓扑。

**改法**
让 `owns()` 无锁。依据是一条本来就成立、只是没被写出来的不变量：**chunk 只增不减**。
`grow()` 是唯一的写者，只往 `m_dynchunks` 尾部 `push_back`，全树没有任何地方摘链；
一个 chunk 的 `base`/`size` 一旦定下也不再变。

所以加了一张 `m_extents` 数组和一个 `std::atomic<usize> m_num_extents`：写侧在持锁时
先写条目、再用 release 存计数；读侧用 acquire 读计数，然后扫描——计数以下的条目必然
已经写完且不再变。表长 `Mapper::NUM_SLOTS + 1`（窗口能容纳的 chunk 数，加上 Scattered
布局那唯一的 chunk 0）。

没有改成无锁遍历 `m_dynchunks` 本身：侵入式双向链表的 `push_back` 要改动尾节点和头哨兵
两处，并发读者可能看到半截状态。一张只增的数组 + 一个计数，是同样的语义里最不需要论证的
那种写法。

**参照**
`Linux pcpu_chunk_addr_search()`（`mm/percpu.c`）——它读 chunk 列表同样不取 `pcpu_lock`，
理由也是列表只增不减。

**验证**
- `ninja -C out/bringup.x86`：通过。
- `clang-tidy -p out/bringup.x86 kernel/mm/cpu_local.cpp`：本次改动行上无新增诊断。
- QEMU 实测两个镜像：`kernel.dev.obi` 一路启动到 ring 3 的 shell 并列出命令表；
  `kernel.dev.dbg_1.obi`（`KERNEL_DEBUG_LEVEL` 非零，断言是活的）正常启动并跑到线程调度。
- **没有复现过死锁本身**：它要求 `grow()` 被调到，而那需要先把 chunk 0 每 CPU 的 16 KiB
  动态区用满。这次改的是把环拆掉，不是修一个观察到的挂起。

**故意没做**
- 没动 `Dyn::allocate()` 持 `m_lock` 跨 `grow()` 这件事——那是 `docs/repairs/260912/mm-cpu-local.md`
  的第 2 条，要先让 `allocate_locked()` 变成 lock-free，是另一件事。这次只保证 `owns()` 这条
  读路径不再参与那把锁。
- 没有把 `m_dynchunks` 换掉。分配和释放仍然走链表、仍然持锁，那条路不在热路径上。
- 没有给「chunk 只增不减」加静态检查。它现在是注释里的不变量；如果将来真要支持收缩动态区，
  `owns()` 这套就得跟着改成带 generation 的方案。
