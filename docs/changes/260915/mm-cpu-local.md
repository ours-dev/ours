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

---

## 按 CPU 号解析时检查该 CPU 真的有存储（2026-09-15）

**文件**
- `kernel/mm/include/ours/mm/cpu_local.hpp` — 新增私有的 `offset_of(CpuNum)`，
  `resolve(handle, cpu)` 和 `access(t, cpunum)` 都改走它。

**症状**
潜伏，当前触发不到。触发条件是在 `CpuLocal::init()` 之前，用一个还没有 unit 的 CPU 号去
`resolve()` 或 `access()`。现在的启动路径不会这么做，但这件事**不会以任何方式报错**——
既不 fault、也不被 `holds()` 拦住，只是静静地读写错地方。

**原因**
`init_early()` 把 `CPU_OFFSETS` 整张表清成 0，只填 boot CPU 那一项；其余要等
`init()` 才有值。而 offset 为 0 意味着 `handle + 0 == handle`，也就是解析回**模板自身**。

模板是那份由链接器发射、谁都不许写的 `CPU_LOCAL` 镜像。它住在
`.init.data.cpu_local` 里，`reclaim_init_area()` 启动后会把整个 `.init` 区还给分配器——
所以往这里写，早期是污染"默认值"，晚期是往别人已经拿走的内存里写。

之所以完全静默，是因为**每一步都是合法的**：CPU 号在范围内，handle 在静态区内，
`holds()` 因此答"是"，相加得到的地址也确实映射着。没有任何一层能看出这是错的。
唯一能看出来的信息是 offset 本身为零，而那正是之前没有人检查的那一项。

**改法**
把两处 `CPU_OFFSETS[cpunum]` 的直接下标换成 `offset_of(cpunum)`，它在 debug 下断言
CPU 号合法**且 offset 非零**。offset 不可能合法地为零：unit 来自 EarlyMem 或 buddy，
而 `origin()` 在内核镜像的 `.init.data` 里，两者不会重合。

没有用已有的 `has_unit()` 做这个断言，尽管名字更贴切：`has_unit()` 说的是某个 CPU 已经
**attach 到**它的 unit，这比 unit 存在要晚。`init()` 先用一趟循环填完所有 offset，再用
下一趟通过 `access()` 写每个 unit 自己的记账字段（`CURRENT_CPU_OFFSET`、`CURRENT_CPUNUM`），
那时候没有任何一个 CPU attach 过——拿 `has_unit()` 断言会在启动路径上直接打死。

**参照**
无直接对应物。Linux 的 `per_cpu_ptr()` 在 `CONFIG_DEBUG_PER_CPU_MAPS` 下会走
`per_cpu_ptr_to_phys()` 之外的检查路径，但它查的是 cpu 是否 possible，而不是 offset
是否已填——因为 Linux 的 `__per_cpu_offset[]` 在 `setup_per_cpu_areas()` 里一次填满，
不存在 ours 这种"boot CPU 先有、其余后有"的中间态。这个中间态是 `init_early()` 带来的，
断言也就只能按它来写。

**验证**
- `ninja -C out/bringup.x86`：通过。
- `clang-tidy`：改动行上无新增诊断。
- QEMU 两个镜像都实测：`kernel.dev.dbg_1.obi`（断言是活的）正常启动并跑到线程调度，
  说明启动路径上没有一处踩到零 offset；`kernel.dev.obi` 启动到 ring 3 的 shell。

**故意没做**
- 没有在 release 下也检查。这是一条纯粹的编程错误断言，不是运行时可能发生的状况，
  `DEBUG_ASSERT` 是对的层级。
- 没有动 `resolve(handle)`（无 CPU 号的那个重载）。它走 `%gs`，读的是当前 CPU 自己的
  offset，而一个正在执行的 CPU 必然已经 attach 过——`attach_to_unit()` 是它跑起来的前提。

---

## 删掉 `PerCpu::handle()`（2026-09-15）

**文件**
- `kernel/mm/include/ours/mm/cpu_local.hpp` — 移除 `PerCpu::handle()`。

**症状**
无。全树零调用者，纯粹是把一件类注释宣称做不到的事又开了个口子。

**原因**
`PerCpu` 的类注释写着：

> Linux relies on sparse and a `__percpu` annotation to stop callers dereferencing the
> equivalent value; here the type simply never offers a way to.

这句话是这个类的全部价值主张——它不靠外部工具，靠自己的接口形状保证 handle 不会被
当指针用。但 `handle()` 返回 `VirtAddr`，拿到手 `reinterpret_cast` 一下就解引用了，
落点正是链接器那份模板，也就是上一条改动刚加断言去防的那块内存。

一个"类型上不可能"的性质，只要有一个访问器把内部值原样交出去就不再成立。

原先唯一的调用者是 `prof_console.cpp`，拿它跟 `NOWHERE` 比较判空；那处后来改用
`operator bool` 了，这个函数就再没人用过。

**改法**
删掉。判空有 `operator bool`，要计数有 `count()`，没有第三种正当用途。

**参照**
`Linux` 的 `void __percpu *` 靠 sparse 的 `__percpu` 注解拦截解引用；ours 没有 sparse，
所以只能靠类型本身不提供出口。这次是把出口关上。

**验证**
- `ninja -C out/bringup.x86`：通过（确认确实没有调用者）。
- QEMU 实测 `kernel.dev.obi`：启动到 ring 3 的 shell。

**故意没做**
没有改成返回一个不可解引用的包装类型。那会是另一套设计（真正的 `PercpuPtr<T>`），
而现在没有任何调用者需要把 handle 拿出去——不需要的东西不必先造出来再包好。
