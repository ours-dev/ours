# kernel/mm 待修项（2026-09-25）

## 1. `alloc_frames` 的带节点重载不检查 `MemoryAllocationState`，不带节点的检查

**现象**：潜伏。`MemoryAllocationState` 的 `disable()` 计数当前没有调用者，所以两条路径的
行为暂时一致。

**原因**：不带节点的 `alloc_frames(gaf, ...)` 经 `PmNode::Current::alloc_frames()`，
开头就是

```cpp
if (auto const status = inherit_thread_context(gaf); Status::Ok != status) {
    return ustl::err(status);
}
```

而 `inherit_thread_context()` 在 `!mem_state.is_enabled()` 时回 `Status::AccessDenied`，
也就是尊重「这段区间里不许分配」。带节点的 `alloc_frames(nid, gaf, ...)`
（`kernel/mm/pmm.cpp`）在 `nid < NUMA_MAX_NODE` 时直接
`PmNode::node(nid)->alloc_frames(...)`，绕过整个 `Current` 层，那个检查和线程的
resource lock 都不经过。于是 `CpuLocal::Mapper::build_dense()` 这类指定节点的调用者
完全不受计数器约束。

**要先问清楚**：这个不对称是有意的（「指定了节点就是内核基础设施，不受线程策略管」）
还是漏的。如果是漏的，`inherit_thread_context()` 应该提到两条路径的共同上游；
如果是有意的，`MemoryAllocationState` 的注释要写明它只约束哪一条路径，
否则下一个人会以为 `disable()` 能挡住所有分配。

**顺带**：`MemoryAllocationState::disable()` 目前没有任何调用者
（`kernel/mm/include/ours/mm/memory_allocation_state.hpp`），所以这条不急；
但在第一个 `disable()` 落地之前必须定下来，否则那个调用者会以为自己挡住了而实际没挡住。
