# 原子量 — 待修

---

## 1. lockdep 仍在用 `ustl::sync` 的原子量，是内核里最后一处

`system/lib/ustl/include/ustl/sync/atomic.hpp` 定义的 `Atomic`、`AtomicRef`、`MemoryOrder` 和两个 fence，在 `kernel/`、
`system/public/` 里已经一个调用点都没有了（见 `docs/changes/260912/atomics.md`）。但这份清点当初没查 `system/lib`：
`system/lib/lockdep/include/lockdep/lock_class_state.hpp` 仍然用 `ustl::sync::Atomic<Self *>`、`AtomicU32`、`AtomicUsize`
和 `MemoryOrder`（注册表指针、依赖计数、依赖数组、代号计数器），而 lockdep 的头文件被大量内核编译单元带进来：
`ninja -t deps` 显示 `out/bringup.x86` 里有 179 个内核目标文件依赖 `ustl/sync/atomic.hpp`。（2026-09-26 核实）

所以原来写的两条路都走不通：删掉这个头文件内核就编不过；在它顶部写"内核不再使用它"也是错的。

要做的：把 `lock_class_state.hpp` 里的这几处换成 `std::atomic`，和 `docs/changes/260912/atomics.md` 对内核其余部分的迁移
一致。之后再回到原来的问题：ustl 要不要保留这层封装，保留的话在文件顶部写明"本内核不再使用它，新代码用 `std::atomic`"。

认领前注意：`system/lib/lockdep` 是未初始化的子模块，目录里是未入库的工作，本仓库提交不了其中的文件，改动要走 lockdep
自己那条线。
