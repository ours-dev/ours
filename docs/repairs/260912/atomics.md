# 原子量 — 待修

暂无。

第 1 条已修：内核里最后一个使用者 lockdep 在 260926 迁到了 `std::atomic`（见 `docs/changes/260926/lockdep.md`），
`ustl/sync/atomic.hpp` 顶部写明了本内核不再使用它、新代码用 `std::atomic`（见 `docs/changes/260926/ustl.md`）。
这层封装本身留着：ustl 是独立的库，要不要删是 ustl 自己的取舍，不是缺陷。
