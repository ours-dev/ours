# heap 待修项（2026-09-08）

Item numbers are not reused and the remaining items are not renumbered when one is fixed, so a gap in the sequence means that item has been done and the references to `heap.md #N` elsewhere still point at the right thing. Fixed so far: item 3 (2026-10-27, `docs/changes/261027/mm.md`).

## 1. 过对齐类型经 `new (gaf) T` 会被静默地对齐不足

`kernel/products/BUILD.gn:89` 传了 `-faligned-new=8`，所以任何 `alignof(T) > 8` 的类型
在普通 `new` 下会走 `operator new(size_t, std::align_val_t)`。但 `heap/new.hpp` 的
placement 形式**没有 `align_val_t` 重载**：

```cpp
auto operator new(usize size, ours::mm::Gaf gaf) noexcept -> void *;
// 没有 operator new(usize, std::align_val_t, ours::mm::Gaf)
```

按 `[expr.new]/19`，带额外实参的 new 表达式先尝试把对齐实参加进实参表，找不到匹配就
**去掉它重试**——于是 `new (gaf) alignas(64) T` 悄悄落到上面这个重载，拿到的是
kmalloc 的 `alignof(usize)` 对齐。

现状没有触发者：全树没有任何过对齐类型走堆分配（`llvm-nm` 对 `_ZnwmSt11align_val_t`
零引用）。所以这里记下而不是先加重载——一个没有调用者的重载没法被验证，而
`kmalloc()` 本身也还不接受对齐参数（`ktl/memory_resource.cpp:10` 就是因此拒绝
`alignment > MAX_ALIGNMENT` 的）。

真要修是两件事：`kmalloc` 长出对齐参数，然后补 `align_val_t` 重载族。注意 vmalloc 那
一侧天然满足任何 ≤ PAGE_SIZE 的对齐。

## 2. 多核下 `vfree()` 之后其它 CPU 可能仍持有陈旧 TLB 表项

`heap::vfree()` 逐页 `ArchVmAspace::unmap()`，而 `PageTable::unmap_pages` 只做本地
`invlpg`（`kernel/arch/x86/mm/page_table.cpp`）。单核正确；一旦有第二个 CPU 曾经访问过
那段地址，它的 TLB 里可能还留着翻译，而帧已经被归还并可能已经发给别人。

需要的是 TLB shootdown IPI。这是 `kernel/arch/x86` 的 lane——Linux 的对应物是
`flush_tlb_kernel_range()`，Zircon 是 `ArchVmAspace::Unmap` 里的 `TlbInvalidation`。

在它到位之前，vmalloc 只能算单核正确。

## 4. `VmMapping::activate()` 丢掉 `VmaSet::insert()` 的失败（mm）

```cpp
// kernel/mm/vm_mapping.cpp:122
    Base::activate();          // 返回值被丢弃
    return Status::Ok;         // 无条件
```

`VmaSet::insert()`（`vm_area_or_mapping.cpp:82`）可能返回 `OutOfMem` / `AlreadyExists`
而**没有走到 `value.retain()`**。于是 `VmArea::map_with_vmo()` 交回一个"成功"的映射，
它却不在父区域的集合里，引用计数只有一份——就是调用方手里那份。

对 `heap::vfree()` 的后果：它先丢掉 `BUSY` 里那份 Arc 再调 `WINDOW->unmap()`，而
`unmap_locked` 在树里找不到匹配时返回 `Ok`。于是 `~VmMapping` 在一个 `m_list_hook` 仍然
挂在 VMO 映射链表上的对象上运行——那个 hook 是 SafeLink 模式，析构里
`require(!is_linked())` 会**直接 `__builtin_trap()`**。也就是说症状是 vfree 里一次硬陷阱，
不是静默损坏（成员按声明逆序析构，hook 在 `m_vmo` 之前，所以 `~VmCowPages` 根本不会跑）。

**可达性**：BTree 节点是 256 B，kmalloc 不会因尺寸拒绝；`init_memory_resource()` 也在
`init_vmm()` 之前跑完。唯一的触发条件是 kmalloc 真的耗尽——而那又被本文件第 3 条
（耗尽时 `alloc_frames` 空转而不返回空）挡住了。所以是潜伏项，不是当前故障。

三个反驳者里两个据此判为"当前不可达"；机制本身三个都确认。记在这里是因为一旦第 3 条
修好，这一条就变成可达的。
