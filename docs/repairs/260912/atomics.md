# 原子量 — 待修

---

## 1. `ustl/sync/atomic.hpp` 在内核侧已无使用者

`system/lib/ustl/include/ustl/sync/atomic.hpp` 定义的 `Atomic`、`AtomicRef`、`MemoryOrder`
和两个 fence，在 `kernel/`、`system/public/` 里已经一个调用点都没有了
（见 `docs/changes/260912/atomics.md`）。

没有顺手删掉：ustl 是独立的 freestanding 库，它的头文件不必只服务于本内核。要不要删，
取决于 ustl 自己是否还想保留这层封装——如果保留，至少该在文件顶部写清楚「内核不再使用它，
新代码用 `std::atomic`」，否则下一个人会照着它写。
