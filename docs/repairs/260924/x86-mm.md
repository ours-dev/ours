# x86-mm 待修项（2026-09-24）

## 1. NUMA 构建在 userboot 之前停住

`mm_numa = true` 构建（`out/numa.x86`），`NUMA=1` 启动（QEMU 4 节点、8 CPU）：8 个 CPU
全部上线，打出 `kshell>` 和 `prof: indexed ... unwind frames`，然后再无输出（80 秒观察）。
默认构建同一位置之后紧接着是 `userboot: kernel package holds 7 files`。

之前被 SRAT 绑定用错 `set_nid` 导致的早期缺页挡住，修掉那一条后才暴露
（`docs/changes/260924/x86-mm.md`）。所属模块未定：可能在 userboot 装载，也可能在
NUMA 下的分配路径（节点缓存、CpuLocal 分组）。先用 QEMU monitor 看各 CPU 停在哪。

复现：

```bash
NUMA=1 IMAGE=out/numa.x86/kernel.dev.obi python3 scripts/qemu_boot.py -no-reboot
```

## 2. SRAT 的 CPU 绑定把条目顺序当 CPU 号

`srat.cpp` 的 `bind_node_to_cpu()` 用遍历 SRAT 时的计数 `count` 作为 `CpuNum`。
CPU 号是 MADT 解析时 `assign_cpunum_for_apic()` 按 APIC ID 分配的，两者只在
"SRAT 与 MADT 以同样顺序列出 CPU"时一致——QEMU 恰好如此，真机不保证。

改法照 Linux：SRAT 阶段只记 `apicid → node`（`__apicid_to_node[]`），MADT 分好号之后
在 `claim_possible_cpus()` 那个位置（Linux `init_cpu_to_node()`）按 `cpu → apicid → node`
写 `set_nid_early()`。顺带把同文件里无人读的 `apicids[]`/`cpus[]` 静态数组的用途落实。

## 3. 指向无内存节点的 CPU

SRAT 允许一个 proximity domain 只有 CPU 没有内存。这样的 CPU 被认领到一个 `PmNode`
不存在的节点上，`PmNode::Current::publish()` 里
`DEBUG_ASSERT(node, "CPU {} was claimed by node {}, which does not exist")` 会触发，
release 下则对空指针取锁。

Linux 的做法：`init_cpu_to_node()` 发现节点不在线时调 `init_memory_less_node()`，
建一个没有内存的 `pg_data_t`，分配回退到最近的有内存节点。本项目要么同样建空节点，
要么在认领时把这类 CPU 改挂到距离最近的有内存节点上。

## 4. 从已映射但不可执行的用户页取指，缺页被当成伪缺页无限重试（已交接）

`x86_handle_page_fault()` 只把错误码的 P/W/U 位翻译成 `VmfCause`，丢掉了第 4 位（取指）；`VmMapping::fault()` 只对写检查
权限，页已存在且读写权限一致就回 Ok。用户程序跳进自己的栈（NX）时，同一条取指永远缺页，进程不会被杀。
改法照 Zircon：`VMM_PF_FLAG_INSTRUCTION`，映射没有执行权限时拒绝取指缺页。
