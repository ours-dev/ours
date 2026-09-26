# 原子量 — 待修

---

## 1. `ustl/sync/atomic.hpp` 在内核里已经没有使用者，这层封装留不留

`system/lib/ustl/include/ustl/sync/atomic.hpp` 定义的 `Atomic`、`AtomicRef`、`MemoryOrder` 和两个 fence，内核里已经一处都
不用了：`kernel/`、`system/public/` 在 260912 迁到了 `std::atomic`（见 `docs/changes/260912/atomics.md`），最后一个使用者
lockdep 在 260926 跟着正本迁了过去（见 `docs/changes/260926/lockdep.md`）。`ninja -t deps` 里依赖这个头文件的只剩 ustl
自己的 `atomic-test`。（2026-09-26 核实）

没有顺手删掉：ustl 是独立的 freestanding 库，它的头文件不必只服务于本内核。要不要删，取决于 ustl 自己是否还想保留这层
封装——如果保留，至少该在文件顶部写清楚"本内核不再使用它，新代码用 `std::atomic`"，否则下一个人会照着它写。
