# x86-mm 改动记录（2026-09-24）

## SRAT 的 CPU→节点绑定改用 `set_nid_early`（2026-09-24）

**文件**
- `kernel/arch/x86/mm/srat.cpp` — `bind_node_to_cpu()` 里 `set_nid` → `set_nid_early`

**症状**
只在 `mm_numa = true` 且机器给了 SRAT 时出现（默认构建不编译 srat.cpp）。
用 `NUMA=1`（QEMU 4 节点、8 CPU）启动，两个镜像都在把内存交给 buddy 之后立刻缺页：

```
Released 16 regions, 255699 frames (998 MiB) to the buddy allocator
Unhandled page fault: vector=14, error_code=0x2
  cr2=0xffffff8800016fff
  rip=0xffffff8840025605 (+0x25605)      -> memmove + 0x65
  rcx=0x17000  rdx=0x17000  rdi=0x0
```

**原因**
`PmNode::Current` 有两张 CPU→节点表：`STATIC_CPU_TO_NODE`（固件说的"家"，用来建
每个节点的 hard affinity）和 `DYNAMIC_CPU_TO_NODE`（运行时 CPU 当前归属）。两者都以
`NUMA_MAX_NODE` 作"未知"哨兵。

- `set_nid_early()` 两张表一起写，不碰任何节点对象——给启动早期用的。
- `set_nid()` 是运行时把一个 CPU 从一个活着的节点挪到另一个：只写动态表，然后去
  旧节点、新节点上改 `m_work_cpus` 并发布每 CPU 指针。

SRAT 解析跑在 `PlatformInitLevel::PrePmm + 1`，此时一个 `PmNode` 都还没建。调用
`set_nid()` 的结果是：动态表写上了，`PmNode::node(nid)` 为空所以后半段全跳过，**静态表
留在哨兵上**。接着 `PmNode::bootstrap()` 的 `claim_cpu` 看动态表已经不是哨兵，认为
"有拓扑源认领过了"，也不写静态表。`PmNode::init()` 按静态表建 hard affinity，于是每个
节点的 hard affinity 都是空的；`CpuLocal::Mapper::build_map_info()` 拿不到任何亲和信息，
只能把 CPU 单元轮转分到各节点组里。

从"hard affinity 全空"到那次 `memmove` 写到 physmap 基址附近，中间的调用链没有追完：
故障地址 `physmap + 0x16fff`、长度 `0x17000`、`rdi=0` 像是把一段模板拷到一个从空分配
换算出来的地址上，但没有回溯可以证实。能确定的是：两次运行只差这一行，改前两个镜像
都在这里崩，改后都越过了这里。

这是"早期"与"运行时"两个接口被用错时机的典型：`set_nid()` 自身没有错，它的"新旧相同
就返回""节点不存在就不管"在运行时都是对的；错在它被拿来做它不负责的首次登记。

**改法**
改前：

```cpp
            mm::PmNode::Current::set_nid(static_cast<CpuNum>(count), NnId{binding.domain});
```

改后：

```cpp
            mm::PmNode::Current::set_nid_early(static_cast<CpuNum>(count), NnId{binding.domain});
```

注释写明为什么这里只能用 early 版本。

**参照**
`Linux acpi_numa_processor_affinity_init()` 只把 `apicid → node` 记进 `__apicid_to_node[]`，
`init_cpu_to_node()` 随后用 `numa_set_node()` 写进 `early_per_cpu(x86_cpu_to_node_map)`——
都是纯表写入，不碰任何 `pg_data_t`。本项目的 `set_nid_early()` 对应的就是这一层。

**验证**
- 默认构建（`out/bringup.x86`，`mm_numa = false`）：构建通过，两个镜像启动，init↔shell 照常。
- NUMA 构建（`out/numa.x86`，`mm_numa = true`，`NUMA=1`）：
  - 改前：两个镜像都在 buddy 释放之后缺页（见上）。
  - 改后：越过该点，8 个 CPU 全部上线，到达 `kshell>`；随后在 userboot 之前停住，
    80 秒内没有进一步输出。这是被原来的崩溃挡在后面的另一个问题，已记入
    `docs/repairs/260924/x86-mm.md` 第 1 条，没有在这一条里修。

**故意没做**
- `count`（SRAT 条目顺序）被直接当成 CPU 号，没改。CPU 号由 MADT 解析分配，而 MADT 在
  `InitLevel::Irq` 才跑，比这里晚得多；正确做法是先记 `apicid → node`，等 MADT 分好号
  再映射，属于另一件事（待修项第 2 条）。
- 没有处理 SRAT 指向无内存节点的 CPU（待修项第 3 条）。
