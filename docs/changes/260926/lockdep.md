# lockdep 改动记录（2026-09-26）

## lockdep 改用 `std::atomic`：本树的 lockdep 快照前进到正本提交 `d3417e1`（2026-09-26）

**文件**
- `system/lib/lockdep/`：本 worktree 里未初始化的子模块目录，放的是一份没有版本的快照，本仓库不跟踪其中的文件，所以这部分改动
  只落在磁盘上、不进本仓库的提交。整体从 lockdep 正本的 `d3417e1^` 前进到 `d3417e1`：
  - `include/lockdep/lock_class_state.hpp` — 图节点的原子量从 `ustl::sync` 换成 `std::atomic`，注册标志从 `AtomicU32` 改成 `std::atomic<bool>`
  - `include/lockdep/{guard,lock_class,lock_context,lock_traits}.hpp` — 同一提交里的 `ValidationControl` → `ValidationOptions` 改名、唯一一处三元表达式改成 `if`、补 `[[nodiscard]]`
  - `tests/` → `test/`；`run_tests.py` 每组测试同时用宿主编译器和树里的 clang 23 各构建一次
  - `README.md`、`docs/interface.md`
- `docs/repairs/260912/atomics.md` — lockdep 那一半做完了，剩下 ustl 这层封装的去留
- `docs/repairs/260917/kernel-task-sched.md` — 第 6 条里 lockdep 主机测试的路径跟着改名

**症状**
内核里最后一处用 `ustl::sync` 原子量的是 lockdep：`lock_class_state.hpp` 的注册表指针、注册标志、依赖计数、依赖数组都是
`ustl::sync` 类型，`ninja -t deps` 显示 179 个内核目标文件经它依赖 `ustl/sync/atomic.hpp`。仓库的规矩是：标准写法在 freestanding
下可用的地方，一律用标准写法；内核其余部分 260912 已经迁到了 `std::atomic`。

**原因**
为什么偏偏剩下 lockdep：260912 的迁移只清点了 `kernel/` 和 `system/public/`，没查 `system/lib`。而 `system/lib/lockdep` 在本
worktree 里不是 lockdep 仓库的检出，只是一份快照，不会随正本前进。正本（`git@github.com:ours-dev/lockdep.git`，
`refactor/zircon-interface` 分支）在 `d3417e1`（2026-09-25）已经迁到了 `std::atomic`，快照一直停在它的父提交上。

也就是说，这次不是 lockdep 还没迁，而是本树没有跟上已经迁好的 lockdep。

**改法**
动手前先核对三件事：
- 快照逐文件等于正本的 `d3417e1^`，一个文件都不差，所以前进到 `d3417e1` 恰好就是正本的那一次提交。
- 内核没有引用这次提交改掉的名字（`ValidationControl`、`LockValidationControlV`）。
- lockdep 那边验证这次提交时用的 ours `3e6a4a15` 是本分支 HEAD 的祖先。它的提交说明写着：把这棵树放进
  `system/lib/lockdep` 后默认配置全部构建通过，两个镜像都启动到 kshell。

然后把快照整体换成 `d3417e1` 的树（`git archive d3417e1` 解包进去），删掉改名前的 `tests/`，再逐文件核对，确认和 `d3417e1`
完全一致。

改前（快照，节选）：

```cpp
#include <ustl/sync/atomic.hpp>
...
using ustl::sync::MemoryOrder;
if (m_registered.load(MemoryOrder::Acquire)) {
    return;
}
if (m_registered.exchange(1, MemoryOrder::AcqRel)) {
    return;
}
...
static inline ustl::sync::Atomic<Self *> m_registry{nullptr};
ustl::sync::AtomicU32 m_registered{0};
ustl::sync::AtomicUsize m_dependency_count{0};
ustl::sync::Atomic<Self *> m_dependencies[MAX_DEPENDENCIES]{};
```

改后（正本 `d3417e1`）：

```cpp
#include <atomic>
...
if (m_registered.load(std::memory_order_acquire)) {
    return;
}
if (m_registered.exchange(true, std::memory_order_acq_rel)) {
    return;
}
...
static inline std::atomic<Self *> m_registry{nullptr};
std::atomic<bool> m_registered{false};
std::atomic<usize> m_dependency_count{0};
std::atomic<Self *> m_dependencies[MAX_DEPENDENCIES]{};
```

内存序逐一对应，没有放松也没有收紧：Acquire → `memory_order_acquire`，AcqRel → `memory_order_acq_rel`，Relaxed、Release 同理。
注册标志只有"注册过/没注册过"两个值，本来就是个 `bool`，用 `AtomicU32` 装着只是因为那层封装没有 `bool` 的别名。

否掉的方案：
- **在快照上手工只改原子量那几行。**快照会变成一个正本里不存在的版本，下次同步时得人工辨认哪些是本地改的；而正本那次提交
  已经做了同样的事，还在 ours 里验证过构建和启动。
- **同步到正本分支最新（`117e1c1`）。**`d3417e1` 之后的提交引入了 `thread_lock_state.hpp`、per-thread/per-CPU 两套状态等，
  要改内核这边的接口，那是 lockdep 接入内核的另一件事，不是"改用 `std::atomic`"。
- **顺手把超级仓库的子模块指针（`1232d0c`）挪到 `d3417e1`。**`d3417e1` 只在 `refactor/zircon-interface` 上、不在 main；而且指针
  本来就比快照落后一个提交，本仓库的做法是快照目录承载工作、指针不跟。挪指针会影响别处初始化子模块的检出。

**参照**
lockdep 正本 `d3417e1`（"refactor(lockdep): name the validation bits for what they are, and test under the tree's compiler"）；
内核其余部分的同类迁移见 `docs/changes/260912/atomics.md`。

**验证**
- 踩到一个坑，记下来给以后同步快照的人：`git archive | tar -x` 保留的是提交时间（9 月 25 日），比已有的目标文件还早。第一次
  构建 ninja 认为一切都是最新的，一个文件都没重编，依赖表里仍是 179 个目标文件依赖 `ustl/sync/atomic.hpp`。把快照里的文件
  `touch` 一遍之后，才重编了 204 个编译单元。
- `prebuilt/ninja/ninja -C out/bringup.x86` 成功；`ninja -t deps`：依赖 `ustl/sync/atomic.hpp` 的只剩 ustl 自己的
  `atomic-test`，内核目标文件从 179 个降到 0。
- `system/lib/lockdep/test/run_tests.py`：10 组构建全部通过。宿主编译器下是 smoke 的三个特性级别，加上 threaded 的普通、
  ThreadSanitizer、AddressSanitizer+UndefinedBehaviorSanitizer 三种；树里的 clang 23 下是 smoke 三级加 threaded。
- `clang-format` 无差异。`clang-tidy -p out/bringup.x86`（跑 `mutex.cpp`、`thread.cpp`，`--header-filter` 覆盖 lockdep 头文件）
  对 lockdep 头文件零告警；`thread.cpp` 自身原有 5 条命名告警，与此无关。
- QEMU：debug 镜像 60 秒、release 镜像 30 秒，都起到 shell，libc 自检通过；没有 panic/assert/lockdep 报告，被终止的进程只有
  故意缺页的 fault_probe。注意默认配置是 `lockdep_enable = false`，所以这次启动验证的是"编得过、行为不变"，没有走到 lockdep
  的校验路径；那条路要等内核补上两个钩子，见 `docs/repairs/260917/kernel-task-sched.md` 第 6 条。

**故意没做**
- 子模块指针没动，正本 `d3417e1` 之后的提交没带进来，理由见上。
- `ustl/sync/atomic.hpp` 的去留：内核里已经没有使用者了，见 `docs/repairs/260912/atomics.md`。
