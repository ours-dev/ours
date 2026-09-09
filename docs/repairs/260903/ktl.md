# kernel/lib/ktl — 待修

按严重程度排。来源：2026-09-03 追 `prof_init()` 挂死时发现，改动记录见
`docs/changes/260903/prof.md` 第 1 条。

---

（本文件原有的两条都已修掉，改动记录见 `docs/changes/260908/heap.md`：

- **第 1 条**「`ktl::Allocator::allocate()` 在 `kmalloc` 返回空指针时直接在空指针上构造」
  —— 根因是那个 placement `operator new[]` 没有 `noexcept`，语言规则因此允许编译器
  假定它不返回空。已给 `heap/new.hpp` 的四个 placement 形式加上 `noexcept`，并在
  `new.cpp` 里用 `static_assert(noexcept(...))` 把这条性质钉住。

- **第 2 条**「`kmalloc()` 超过 4 KiB 返回空指针，没有页分配器回退」
  —— 已由 vmalloc 补上：`operator new` 现在按大小在 kmalloc 与 vmalloc 之间路由，
  `operator delete` 按地址路由回去，对标 Linux 的 `kvmalloc()`/`kvfree()`。）

目前无未修项。
