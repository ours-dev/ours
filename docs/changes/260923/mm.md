## 每个可能的 CPU 在 AP 启动前都认领节点；AP 上的 kmalloc 不再挂死（2026-09-23）

**文件**
- `kernel/mm/pm_node.cpp`、`include/ours/mm/pm_node.hpp` — 新增 `PmNode::Current::claim_possible_cpus()`（`InitLevel::Irq + 1` 的初始化钩子）；
  `init()` 里的 `publish` lambda 提成私有静态函数 `publish(CpuNum)`，并在其中补上节点的 hard affinity

**症状**
AP 上的第一次内核分配永不返回。shell 跑在 CPU1 上，`channel_create` 里的 `kmalloc(160)` 在
`MemCache::allocate()` 里空转，日志里什么都没有（`docs/repairs/260923/mm.md` #1，本次删除）。

**原因**
QEMU monitor 抓现场：CPU1 空转的「锁」地址是 `0xffffff8840082f48`；把 kmalloc-256 这个 `MemCache` 对象整个 dump 出来，
`+0x70` 是 `m_node_caches[0]`（正常），`+0x78` 是 `m_managed_hook.next`，值恰好就是 `0xffffff8840082f48`。
也就是说代码读的是 `m_node_caches[1]`——非 NUMA 构建里这个数组只有一个元素——越界拿到了下一个成员里的链表指针，
把它当成 `NodeCache *`，在它的「锁」上永远自旋。

`current_node()` 在 CPU1 上返回 1（`NUMA_MAX_NODE`，「尚未认领」的哨兵）。`bootstrap()` 只认领了它自己运行的那个
CPU：可能 CPU 的掩码要到 `InitLevel::Irq` 的 MADT 解析才填，远在 PMM 之后；此后唯一的认领者是 SRAT 解析，而非
NUMA 构建根本不编译它。`bootstrap()` 里的注释描述的正是这个故障，只是只补了启动 CPU 这一个。

早先 `per-CPU CpuCache 解析到 .bss` 的判断是错的：句柄在创建时和挂死时都完好（`0xffffff88400d30a8`，`holds()` 为真）。
当时把空转的锁当成了 `CpuCache` 的锁，其实是 `NodeCache` 的。

**改法**
照 Linux 在 ACPI 解析之后、`smp_init()` 之前做的 `init_cpu_to_node()`：在 MADT 填好可能 CPU 的掩码之后、`x86_bring_aps()`
启动它们之前，由启动 CPU 一次性认领：

```cpp
auto PmNode::Current::claim_possible_cpus() -> void {
    auto const fallback = nid(CpuLocal::current_cpu());
    DEBUG_ASSERT(fallback < NUMA_MAX_NODE, "the boot CPU has no node");

    Cpu::possible_mask().for_each([fallback](CpuNum const cpu) {
        if (nid(cpu) == NUMA_MAX_NODE) {
            set_nid_early(cpu, fallback);
        }
        publish(cpu);
    });
}
GKTL_INIT_HOOK(ClaimPossibleCpus, PmNode::Current::claim_possible_cpus, gktl::InitLevel::Irq + 1);
```

没有拓扑来源的 CPU 去启动 CPU 所在的节点——即 `bootstrap()` 选的回退节点（第一个拥有内存的节点），无 SRAT 的机器上
也就是唯一的那个。已经被 SRAT 分配过的 CPU 不动，只发布。

`publish()` 除了原先的 `m_work_cpus` 与 per-CPU 指针，还把 CPU 加进节点的 `m_hard_affinity`：`PmNode::init()` 按
`STATIC_CPU_TO_NODE` 建这个掩码时，被认领的只有启动 CPU，晚认领的 CPU 要在这里补上，节点的掩码才和表一致。

**参照**
Linux `init_cpu_to_node()`（`arch/x86/mm/numa.c`）与 `numa_set_node()`；`dummy_numa_init()`（无拓扑时全部放在节点 0）。

**验证**
- 两个镜像都跑通 shell 的 channel 自环测试（`channel checks passed`），此前卡在第一次 `channel_create`。
- 反向：保留 trampoline 修复（`eb028b2`）、临时撤掉本修复，dbg_1 镜像现在会清楚地报出
  `Assertion failed on nid < 1 with message: current_node() returned an unusable node`（cpu1）。在 trampoline 修复
  之前，这里是静默挂死：`panic()` 先要停下其他 CPU，而那些关着中断的内核线程收不到 IPI。
- NUMA 构建（SRAT 路径）未实测。

**故意没做**
没有改 `CpuLocal::Mapper::build_map_info()`：它在 CpuLocal 初始化时按节点的 hard affinity 给单元分组，那时 AP 还没有
节点；非 NUMA 下只有一组，不受影响，NUMA 下的分组问题另记。
