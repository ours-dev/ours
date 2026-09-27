# kernel/mm 待修项（2026-09-27）

## 1. 创建映射全程不持锁：挑空位、挂进父 VMA、挂进 VMO 的映射链表

**现象**：潜伏。目前没有两个线程同时在同一个地址空间里创建映射，也没有两个地址空间
同时映射同一个 VMO，所以触发不到。第一个会碰上的是多线程用户进程并发 `vmar_map`，
以及 K16（所有进程共用一个 vDSO VMO，各自映射它）。

**原因**：`VmArea::create_mapping()` → `create_subaom_preamble()` → `VmMapping::create()`
→ `activate()` 这一路没有取任何锁（`vm_area.cpp` 里只有 `protect`、`unmap` 两处 `Guard{get_lock()}`）：

- `create_subaom_preamble()` 里的 `alloc_spot()` 先读父 VMA 的 `m_subvmas` 找空位，
  `VmAreaOrMapping::activate()` 再 `m_parent->m_subvmas.insert(*this)`。两个线程可能挑中
  同一个空位；插入还会并发修改同一棵树。
- `VmArea::m_num_mappings += 1` 是非原子的读改写。
- `VmMapping::activate()` 调 `m_vmo->add_mapping(*this)`，改的是 `VmObject::m_mappings`。
  一个 VMO 可以被多个地址空间映射，所以即使补上地址空间锁，这个链表也还缺一把属于
  VMO 的锁。`destroy()` 里的 `remove_mapping()` 同样如此。

**照谁改**：
- Zircon `VmAddressRegion::CreateVmMapping()` 经 `CreateSubVmarInternal()`，在
  `Guard<CriticalMutex> guard{lock()}`（地址空间锁）下完成挑空位、构造和激活
  （`zircon/kernel/vm/vm_address_region.cc`）。
- Zircon `VmMapping::Activate()` 取 `Guard<CriticalMutex> guard{object_->lock()}`，再进
  `ActivateLocked()`，由它调用 `VmObject::AddMappingLocked()`。后者声明为
  `TA_REQ(lock())`，即必须持对象锁（`vm_mapping.cc`、`include/vm/vm_object.h`）。

锁序与 `260925/mm.md` 原第 1 条的修法一致：地址空间在外，VMO 在内。
`create_mapping()` 整体包进地址空间锁时要注意，`map_with_vmo()` 随后调用的
`VmMapping::map()` 现在会自己拿这把锁，所以不能在同一把锁下调用它。

**必须在 K16 和多线程用户进程之前修掉。**
