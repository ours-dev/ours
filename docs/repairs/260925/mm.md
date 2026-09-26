# kernel/mm 待修项（2026-09-25）

## 1. VMO 的页仓库由两把不同的锁保护：缺页走地址空间的 SpinLock，读写与提交走 VMO 的 Mutex

**现象**：潜伏。一个 VMO 只被一个地址空间映射、且不被 `read()`/`write()`/`commit_range()` 同时访问时触发不到。

**原因**：`VmCowPages::m_pages`（一棵 B 树）被两条路径改写：

- 缺页：`VmAspace::page_fault_internal()` 持 `VmAspace::m_lock`（`sync::SpinLock`，`vm_aspace.cpp:165`），
  经 `VmMapping::fault()` 调 `paged->make_cursor()` 和 `require_page()`（`vm_mapping.cpp:485-495`），分配并插入页；
  `VmMapping::map_paged()` 同样直接用 cursor（`vm_mapping.cpp:191`）。
- 提交、读、写：`VmObjectPaged::commit_range_internal()` 和 `read_write()` 持 `VmObjectPaged::m_mutex`。

两把锁互不相干，所以：同一个 VMO 被两个地址空间映射时，两边同时缺页会并发插入同一棵树；一边缺页、一边
`write()`，同理。文件系统计划里第一个会碰上的是 K16（所有进程共用一个 vDSO VMO），其次是 K17/K21
（launcher `vmo_write` 一个 VMO 再映射它）。

**不能直接在缺页路径里拿 VMO 的 Mutex**：缺页持的是 SpinLock，在 SpinLock 下拿会睡眠的 Mutex 是错的。

**照谁改**：Zircon 的缺页在碰页之前先拿 VMO 的锁：`VmMapping::PageFault()`
（`zircon/kernel/vm/vm_mapping.cc:1376`）对分页 VMO 取 `Guard<CriticalMutex>{paged->lock()}`，再进
`PageFaultLockedObject()`。地址空间的锁和 VmCowPages 的锁都是可睡眠的 `CriticalMutex`
（`zircon/kernel/vm/include/vm/vm_aspace.h:336`，`vm_cow_pages.h:176`）。所以要么把 `VmAspace::m_lock` 换成
可睡眠的锁（缺页本来就在可睡眠上下文里），缺页时再取 VMO 的锁；要么让 VmCowPages 自带一把自旋锁专管
m_pages。前者与 Zircon 一致，推荐。

**必须在 K16 之前修掉。**

## 2. `alloc_page_table()` 在 buddy 分支上丢掉 `node`

```cpp
auto alloc_page_table(usize size, usize alignment, NnId node) -> PhysAddr {
    if (pmm_enabled()) {
        PhysAddr phys;
        auto *frame = alloc_frames(GAF_KERNEL | Gaf::Zero, &phys, get_order(size / PAGE_SIZE));
        return frame ? phys : PhysAddr{0};
    }
    auto *table = EarlyMem::allocate<u8>(size, alignment, node);   // 只有这条用了 node
```

EarlyMem 分支按 `node` 分配，buddy 分支不按——它调的是不带节点的重载，节点由
`PmNode::Current` 决定。于是「给我节点 N 上的一页页表」这个请求在交接前后拿到不同的答案，
而 NUMA 下页表页放在哪个节点是有意义的（walk 它的是本节点的 CPU）。

**现象**：潜伏，而且当前一个调用者都没有——`kernel/arch/x86/mm/page_table.cpp` 的
`alloc_table()` 用的是两参数重载，`node` 取默认的 `bootmem::ANY_NODE`。所以这条修完
也不会改变任何现有行为；它是一个公开接口静默丢弃调用者传进来的值，这件事本身是错的。

**改法**：`alloc_frames(node, GAF_KERNEL | Gaf::Zero, order)`。那个重载在
`nid >= NUMA_MAX_NODE` 时自己退回不带节点的版本，所以 `ANY_NODE` 的行为一字不变，
包括仍然经过 `PmNode::Current::inherit_thread_context()`。

**注意不要改成「永远用 `current_node()` 解析」**：那会绕过 `inherit_thread_context()`，
也就是绕过 `MemoryAllocationState` 那个「这段区间里不许分配」的计数器。
禁止分配是调用者的明确意图，页表也不该例外。

**顺带记一笔**（另一条，还没查）：`alloc_frames` 这一族里，不带节点的重载经
`PmNode::Current::alloc_frames()`，会检查 `MemoryAllocationState`；带节点的重载
（`alloc_frames(nid, ...)`）直接走 `PmNode::node(nid)->alloc_frames()`，**不检查**。
`build_dense()` 等指定节点的调用者因此完全不受那个计数器约束。这个不对称是有意的还是漏的，
要问清楚再动。
