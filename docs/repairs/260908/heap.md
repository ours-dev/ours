# heap 待修项（2026-09-08）

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
