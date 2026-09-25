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
