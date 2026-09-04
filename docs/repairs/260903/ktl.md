# kernel/lib/ktl 与 unwind — 待修

按严重程度排。来源：2026-09-03 追 `prof_init()` 挂死时发现，改动记录见
`docs/changes/260903/prof.md` 第 1 条。

---

## 1. `ktl::Allocator::allocate()` 在 `kmalloc` 返回空指针时直接在空指针上构造

`kernel/lib/ktl/include/ktl/allocator.hpp` 的 `allocate(n)` 是
`new (ours::mm::GAF_KERNEL) Element[n]()`。这个 placement `operator new[]`（`ktl/new.cpp`）
没有 `noexcept`，语言规则允许编译器假定它不返回空，于是分配失败后紧接着对地址 0 做
值初始化。`ustl::collections::Vec::reserve()` 里的 `if (!buffer)` 永远轮不到执行。

任何一次 `ktl::Vec` 扩容失败（内存耗尽，或下面第 2 条的 4 KiB 上限）都会变成缺页而不是
`Error::OutOfMemory`。prof 的 FDE 索引就是这么挂死整个启动的。

修法：分配和构造分开——`kmalloc` 拿到 `nullptr` 就返回 `nullptr`，拿到内存再
`std::uninitialized_value_construct_n` / 逐个 `std::construct_at`；或者给这个
`operator new[]` 加 `noexcept`，让编译器自己生成空检查。前者更直白。

## 2. `ktl::kmalloc()` 超过 4 KiB 返回空指针，没有页分配器回退

`kernel/lib/ktl/kmalloc.cpp`：十个 size class 到 `MAX_MALLOC_SIZE = 4096` 为止，再大就
`return nullptr`（注释 "Now we do not support to allocate frames from kmalloc"）。
对照 Linux `kmalloc()`：超过 `KMALLOC_MAX_CACHE_SIZE` 转 `kmalloc_large()`，用
`alloc_pages()` 拿 2^order 页，`kfree()` 靠 `PageSlab` 标志分辨归属。这里 `kfree()` 也要
能识别整页分配，否则回退只做一半。

## 3. `unwind/cfi.hpp` 的有符号 `DW_EH_PE_*` 常量与规范不符

`DW_EH_PE_SLEB128 = 0x05`、`SDATA2 = 0x06`、`SDATA4 = 0x07`、`SDATA8 = 0x08`；规范
（LSB / GCC `dwarf2.h`）是 0x09、0x0A、0x0B、0x0C——0x08 是符号位，不是一个格式。
`cursor.cpp` 里对 `0x0B` 的特判和 README 里"psABI 的怪癖"那段其实都是在补这个错。
内核自己的 `.eh_frame` 只用 0x1B（pcrel|sdata4），所以现在不触发；换个工具链或
遇到 `sleb128`/`sdata8` 编码就会把合法节判成畸形。改常量、删特判、改 README，一条提交。
