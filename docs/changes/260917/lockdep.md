# lockdep — 改动记录

格式见 `docs/changes/TEMPLATE.md`。

---

## 1. lockdep 子模块进版本库：gitlink 指到 `f711ced`，磁盘副本换成同一棵树（2026-09-17）

**文件**
- `system/lib/lockdep`（gitlink）— `f74f998` → `f711ced`，即 `ours-dev/lockdep` 合并
  PR #1 之后的 main。
- `system/lib/lockdep/**`（磁盘副本，不被 ours 跟踪）— 整体换成 `f711ced` 的树。
- `docs/repairs/260903/thread-safety.md` — 删掉「`Guard` 的注解提交不进来，重新 checkout
  会丢」这句前提说明，它从这次起不再成立。

**症状**

两层，一层是版本管理，一层是库本身。

1. **lockdep 的改动一直不在任何版本库里。** `system/lib/lockdep` 是未初始化的子模块
   （`git submodule status` 前缀 `-`），目录里却有内核实际编译用的源码。9 月 3 日修
   `IrqSave`、加线程安全注解的那批改动（见 `docs/changes/260903/sync.md`、
   `thread-safety.md`）全都只在工作区；`thread-safety.md` 里明写着「重新 checkout 会丢」。
   gitlink 指着的 `f74f998` 还是 Bazel 时期的树（`BUILD.bazel`、`src/lock-class.cpp`），
   和磁盘上的东西不是同一个时代。
2. **库本身有几处从来没被编译过的缺陷**，都是潜伏的——内核只用到 `Guard` 和
   `state_saver.hpp`，碰不到它们：
   - feature level 写死成 `Validation`，`lockdep_enable` 这些 GN 参数完全不起作用，
     默认构建里每个 `Thread` 照样带着一个验证用的 `LockContext`；
   - `LOCK_DEP_TRAITS` 展开后引用 `ustl::traits::IntegralConstant` 和
     `ustl::TupleElementT`，ustl 里都不存在，一用就编不过；
   - `LockClass::id()` 只声明没定义，第一个调用者会链接失败；
   - 验证器（依赖图、环检测）只有成员没有代码，`LockContext::validate_acquire()` 是空函数。

**原因**

**第一层：gitlink 让父仓库对目录内容视而不见。** 父仓库在索引里只记一个
`160000 commit <sha>` 条目，目录里的文件它一概不看——既不算已跟踪，也不算未跟踪，
`git status` 里什么都不显示。能管这些文件的只有子模块自己的仓库，而子模块没初始化，
目录里没有 `.git`，于是没人管。这就是为什么 9 月 3 日的修复「提交不进来」：不是权限或
冲突问题，是这个路径在结构上就没有一个能接收提交的仓库。

再叠一层：这份磁盘副本是内核构建的真实输入，所以不同会话一直在原地改它；而
`ours-dev/lockdep` 仓库里的分支停在 8 月 18 日的快照上。两条线从同一份快照分叉，
一边改了命名、格式和 `Guard`，一边补了验证器和测试，谁也看不见谁。

**第二层：写死的 feature level。** `config.hpp` 原来是

```cpp
constexpr auto ENABLED_FEATURE_LEVEL = FeatureLevel::Validation;
```

`kernel/products/BUILD.gn` 的 `config("lockdep")` 明明会按 GN 参数定义
`CONFIG_LOCK_DEP` 和 `LOCK_DEP_ENABLED_FEATURE_LEVEL`，头文件却从来不读。结果是构建
说「不要 lockdep」，头文件说「全开」，而头文件赢了——`IfLockValidationEnabled` 据此把
`Thread::m_lock_context` 选成带侵入式链表的 `priv::LockContext`。

**合并时暴露的第三个问题：`priv` 里的名字查找。** 合并后的 `guard.hpp` 一开始是

```cpp
namespace priv {
    template <bool Enabled>
    struct ContextHolder {
        auto get() const -> LockContext * { ... }
```

在 `namespace lockdep::priv` 里，非限定的 `LockContext` 先找到的是
`lockdep::priv::LockContext` 这个类，而不是 `lockdep::LockContext` 这个按 feature level
在 `priv::LockContext` 和 `priv::NullLockContext` 之间二选一的别名。验证关闭时两者是
不同类型，`if constexpr (LOCK_VALIDATION_ENABLED)` 里被丢弃的分支就类型不匹配。
被丢弃的分支里、不依赖模板参数的表达式仍然要做名字和类型检查：Apple clang 21 放过了，
所以宿主测试全绿；内核用的 clang 23 不放过，真实构建一上来就报错。

**改法**

版本管理上，`ours-dev/lockdep` 的历史是这样拼起来的，没有改写任何已推送的提交：

1. `38c0ccc` — 8 月 18 日把磁盘副本原样导入；
2. `8c07e2e`、`e0f3cc4`、`1e87688` — 头文件编译修复、验证器（依赖图 + Tarjan 环检测）、
   宿主测试；
3. `d97e008`、`c6e2c28` — `docs/interface.md`，参照 Zircon 的接口重设计提案；
4. `14410e4` — 把今天 baseline 上的磁盘副本**逐字节**提交成 `38c0ccc` 的子提交，
   和第 2 步成为兄弟；
5. `975e060` — 手工合并两条线；
6. `2364ca9` — 测试脚本从 `.sh` 改成 `tests/run_tests.py`；
7. `f711ced` — PR #1 合并进 main。

第 4、5 步用「导入 + merge」而不是 rebase，是因为 PR 分支已经推送过；也不能指望 git 自动
合并，因为 baseline 那边把大多数文件从 CRLF 改成了 LF，每一行都算改动，git 认不出改名。
所以 merge 是用 `-s ours` 建立父关系、再逐文件手写结果。

合并的原则是**两边重叠的地方以 baseline 为准**，因为内核已经照着它写了：
`spin_lock.hpp` 按 tag 类型声明 saver，组合选项的 saver 是
`std::tuple<AutoInterruptDisabler, AutoPreemptDisabler>`，调度器在用 `ScopedGuard` 和
`unguard()`。分支上的东西按 baseline 的命名规则重述后保留。有一处主动放弃了分支的做法：
分支原来给 `lockdep_get_state_saver` 加了个模板兜底，锁没声明 saver 时退化成
`NullStateSaver`；baseline 是直接编译失败。以 baseline 为准，并在 `state_saver.hpp` 里写明
理由——「什么都不存」正是当初让 `Guard<SpinLock, IrqSave>` 不关中断的那个错。

`config.hpp` 改成

```cpp
#if defined(CONFIG_LOCK_DEP) && defined(LOCK_DEP_ENABLED_FEATURE_LEVEL)
constexpr auto ENABLED_FEATURE_LEVEL = static_cast<FeatureLevel>(LOCK_DEP_ENABLED_FEATURE_LEVEL);
#else
constexpr auto ENABLED_FEATURE_LEVEL = FeatureLevel::Disabled;
#endif
```

默认构建因此从 Validation 变成 Disabled，`Guard` 里的验证成员和 `Thread::m_lock_context`
都塌缩成空类型。

`guard.hpp` 的名字查找问题改成全限定：

```cpp
auto get() const -> ::lockdep::LockContext * { ... }
```

ours 这边，gitlink 从 `f74f998` 改到 `f711ced`，磁盘副本用 `rsync --delete` 换成同一棵树。
替换前核对过：磁盘副本和 `14410e4` 逐文件一致，也就是说今天之前的每一处改动都已经在
`ours-dev/lockdep` 的历史里，替换不丢东西。

被否掉的做法：

- **`git submodule update --init`。** 子模块仓库在 `ours/.git/modules/system/lib/lockdep`，
  它的 `core.worktree` 指向主检出 `ours/system/lib/lockdep`，不是这个 worktree；在链接
  worktree 里初始化，会和主检出抢同一个模块仓库。而且目录现在是其他会话在用的构建输入，
  换成子模块检出会改变它的性质。这次只延续现有模式：gitlink 记版本，磁盘副本与之一致。
- **把 lockdep 直接变成 ours 里的普通文件。** 能彻底消除「提交不进来」，但这是仓库结构的
  决定，不在这次范围内。

**参照**

- Zircon `src/lib/lockdep/include/lockdep/common.h` 的 `LOCK_DEP_ENABLED_FEATURE_LEVEL`
  和 `kEnabledFeatureLevel`——feature level 由构建给、头文件读，本次的 `config.hpp` 照此改。
- Zircon `lockdep/lock_class.h` 的 `LockClassStateStorage::lock_class_state_`——一个 inline
  静态成员同时充当状态存储和类 id，`LockClass::id()` 照此补上定义。
- Zircon `lockdep/guard.h` 的 `Guard::Release()`——先注销记账、再真正放锁，理由是放锁可能
  重新开中断，中断里再拿同一把锁时不能看到它还「被持有」。本次 `Guard::release_lock()`
  采用同样的顺序。
- Zircon `lockdep/thread_lock_state.h` 的 `ThreadLockState::Acquire()`、
  `lockdep/lock_class_state.h` 的 `SingletonLoopDetector::DetectionPass()`；
  Linux `kernel/locking/lockdep.c` 的 `check_prev_add()`、`check_noncircular()`——
  「获取时记边、只查便宜的，完整找环交给别处」的分工。

**验证**

在临时 worktree 里构建 baseline HEAD `bb472f9`，`system/lib/lockdep` 换成 `f711ced`：

| 配置 | 结果 |
| --- | --- |
| 默认（`lockdep_enable=false`） | `ninja -k 0 default` 全部目标通过 |
| `lockdep_metadata_only=true` | 全部目标通过 |
| `lockdep_enable=true` | 所有编译单元通过；`vmours` 和 `vmours.with-tests` 链接失败，只缺 `lockdep::current_lock_context()` 和 `lockdep::report_violation()` |

最后一行是预期内的：这两个是 lockdep 声明、要由内核定义的接口，内核还没写。

QEMU：默认构建的 `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 都启动到 kshell，日志里没有
panic、assert 失败或异常。合并前用 baseline 自带的 lockdep 在 `1484277` 上做过对照，
`kernel.dev.obi` 的启动日志去掉数字后与合并后完全一致。

共享检出的 `out/bringup.x86`（含其他会话未提交的调度器改动）增量构建通过。

宿主测试 `system/lib/lockdep/tests/run_tests.py`：三个 feature level 的 smoke，
以及 threaded 的普通、ThreadSanitizer、ASan/UBSan 三个变体，全部通过。

**故意没做**

- **内核侧的 `current_lock_context()` / `report_violation()`。** 所以 `lockdep_enable=true`
  目前链接不过。按 `docs/interface.md`，这两个接口会被 Zircon 风格的
  `system_get_thread_lock_state(LockFlags)` 等替换（irq-safe 的锁记在 per-CPU 上），
  现在照旧接口写一遍会马上作废。
- **`docs/interface.md` 的重设计。** 锁按声明点分类、`LockPolicy` 取代 state saver、
  删 `ScopedGuard` 等都还没实施；在那之前打开验证，所有 `SpinLock` 同属一个类，正常嵌套
  会被报成重复获取。
- **初始化子模块、或把 lockdep 并进 ours。** 理由见上面「被否掉的做法」。
