# kernel/mm 待修项（2026-09-27）

## 1. 地址空间锁没有线程安全注解，"调用方必须持锁"只写在注释里

**现象**：没有运行时症状，缺的是编译期检查。以下函数要求调用方持地址空间锁，但编译器不知道：
- `VmArea::create_subaom_preamble()`、`VmAreaOrMapping::activate()`：前者读 `m_subvmas` 找空位，
  后者往里插入；
- `VmMapping::page_fault()`：由 `VmAspace::page_fault_internal()` 持锁调用；
- 所有 `*_locked()`（`unmap_locked()`、`protect_locked()`、`find_mapping_locked()` 等，
  `kernel/mm/include/ours/mm/` 下共 13 处声明）。

今天漏拿锁不会有任何提示。创建映射这条路径就是这样全程无锁跑了很久，直到
`260927/mm.md` 那条修复才补上。

**原因**：两层 `get_lock()` 都没有 `TA_RETURN_CAPABILITY`：
- `VmAspace::get_lock()` 返回 `m_lock`；
- `VmAreaOrMapping::get_lock()` 返回 `m_aspace->get_lock()`。

所以分析器把每一次 `Guard{get_lock()}` 都当成一把来历不明的锁，给任何函数加
`TA_REQUIRES(get_lock())` 都无法和调用方的 Guard 对上。相比之下，VMO 那一侧已经做到了：
`VmObject::lock()` 标了 `TA_RETURN_CAPABILITY(m_lock)`，`make_cursor()` 和 `add_mapping()`
都是 `TA_REQUIRES(m_lock)`，漏拿锁就编译失败。

**怎么修**：
1. 给 `VmAspace::get_lock()` 加 `TA_RETURN_CAPABILITY(m_lock)`，`m_lock` 保护的成员
   （`m_root_vma`、`m_last_fault`、`m_destroyed`）加 `TA_GUARDED(m_lock)`；
2. 给 `VmAreaOrMapping::get_lock()` 加 `TA_RETURN_CAPABILITY(m_aspace->get_lock())`；
3. 给上面列出的函数逐个加 `TA_REQUIRES(get_lock())`，按编译错误把漏掉的地方补齐。

预计第 3 步会报出一批现有调用点的错误，每一处都要判断是真漏了锁还是分析器看不懂，
后者用 `TA_NO_THREAD_SAFETY_ANALYSIS` 并写明理由。

**照谁改**：Zircon 的 `VmAddressRegionOrMapping::lock()` 返回
`aspace_->lock()` 并标 `TA_RET_CAP(aspace_->lock())`，各 `*Locked()` 函数都是
`TA_REQ(lock())`（`zircon/kernel/vm/include/vm/vm_address_region.h`）。
