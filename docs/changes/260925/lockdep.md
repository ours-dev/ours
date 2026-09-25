# lockdep 改动记录（2026-09-25）

## SpinLock 上不带选项的 Guard 改为编译错误（2026-09-25）

**文件**
- `system/lib/lockdep/include/lockdep/state_saver.hpp` — 新增 `lockdep::OptionRequired<Lock>`：锁经 ADL 声明
  `lockdep_option_required(Lock *)` 即表示"Guard 必须带选项"
- `system/lib/lockdep/include/lockdep/guard.hpp` — `Guard` 里 `static_assert(sizeof...(Options) > 0 || !OptionRequired<LockType>)`
- `system/lib/lockdep/tests/smoke.cpp` — `fake::StrictLock`，两个方向检查 `OptionRequired`，外加带选项的 guard 用例
- `system/lib/lockdep/README.md`、`docs/interface.md` — 写明新钩子；interface.md 的策略示例原来把 `Guard<SpinLock>`
  映射到 `NoIrqSavePolicy`，正是这次拒绝的写法，改为显式 `NoIrqSave`
- `kernel/include/ours/sync/spin_lock.hpp` — `SpinLock` 声明 `lockdep_option_required()`；`DEFAULT_OPTIONS` 的注释更新
- `kernel/kernel/sync/spin_lock.cpp` — "holding one of these disables nothing" 那段改为指向新规则
- `system/lib/lockdep`（gitlink）— f711ced → 1232d0c（ours-dev/lockdep 分支 `feat/guard-option-required`）

**症状**
2026-09-25 观察到：一个线程持 `PmZone::m_lock` 时被 tick 抢占，同一 CPU 上下一个线程调 `PmZone::free_frame()` 拿同一个
zone 的锁，dbg_1 在 `SpinLock::acquire()`（`kernel/kernel/sync/spin_lock.cpp:147`）的 `DEBUG_ASSERT(!is_held())` 停下；
release 版会一直自旋到自己也被抢占为止。mm 会话先把 PmZone 改成了 `Guard<sync::SpinLock, IrqSave>`
（`fix(mm): take the zone lock with interrupts off`）。树里另有 65 处同样写法的 SpinLock guard，逐个子系统修掉之后，
本条让这种写法再也写不出来。

**原因**
三件事叠在一起：

```cpp
template <typename LockType, typename... Options>
class TA_SCOPED_CAPABILITY Guard {
    ...
    [[no_unique_address]]
    std::tuple<StateSaver<LockType, Options>...> m_savers;   // 每个选项一个 saver
```

```cpp
template <typename LockType, typename... Option>
Guard(LockType &, Option...) -> Guard<LockType, Option...>;  // Guard guard{lock} -> Guard<Lock>
```

```cpp
    constexpr static auto DEFAULT_OPTIONS = Option::DisablePreempt;  // 只是标签，没人读
```

最自然的写法 `Guard guard{spin_lock}` 推导成 `Guard<SpinLock>`，saver 元组为空；`SpinLock::acquire()` 本身只做 CAS，
不碰中断和抢占（`spin_lock.cpp` 的 "Why the lock leaves preemption to the Guard" 一节：这件事被有意交给了 Guard）。
所以这种 guard 持锁时中断、抢占全开，和 `Guard<SpinLock, IrqSave>` 在字面上只差一个模板参数，读代码看不出区别。

自旋锁的正确性前提是"持有者不会在持锁期间让出 CPU"。持有者被抢占或被中断、而同 CPU 上接下来运行的代码要同一把锁时，
它等的是一个不会再跑的持有者——这正是 PmZone 的现场。

**改法**
框架层有两条路，选了第二条：

1. **让不带选项的 Guard 遵从 `DEFAULT_OPTIONS`（DisablePreempt → `task::AutoPreemptDisabler`）。** 否掉，三个理由：
   - 只关抢占对"也在中断里拿"的锁是错的：运行队列锁（tick）、PI 锁（tick 出口的 `preempt()`、睡眠超时）、等待队列锁
     （超时回调）、IPI mailbox 锁（IPI 处理函数）。默认值只会把审计从"每个调用点"变成"那些悄悄需要更多的调用点"，而后者
     更难找。
   - 启动早期没有线程：`AutoPreemptDisabler` 经 `Thread::Current::get()->preemption_state()` 找计数，`InitLevel::Threading`
     之前无处可找；而节点锁（`InitLevel::CpuLocal + 1`）、页表锁（`init_vmm()`）都在那之前被拿。默认值要么在那里崩，要么
     需要一个运行时判断。
   - Zircon 不这么做。
2. **让 SpinLock 上不带选项的 Guard 编译失败。** 采用。与 Zircon 同：Zircon 的 `SpinLock` 用
   `LOCK_DEP_POLICY_OPTION(SpinLock, IrqSave, ...)` 注册，这个宏同时声明
   `::lockdep::AmbiguousOption LOCK_DEP_GetLockPolicyType(lock_type*, void*)`，于是 `Guard<SpinLock>` 的策略是
   `AmbiguousOption`，被 `Guard` 的 `static_assert` 拒绝（"The Option argument of Guard<LockType, Option> must always be
   specified..."）。

实现：

```cpp
template <typename Lock>
concept OptionRequired = requires(Lock *lock) { lockdep_option_required(lock); };
```

```cpp
    static_assert(sizeof...(Options) > 0 || !OptionRequired<LockType>,
                  "this lock must be guarded with an option: Guard<Lock, lockdep::IrqSave> "
                  "(or IrqSaveAndPreemptDisable where the scope may reschedule); without one "
                  "the Guard holds it with interrupts and preemption both enabled");
```

```cpp
    template <Option Options>
    auto lockdep_option_required(SpinLock<Options> *) -> void;
```

钩子没有照搬 Zircon 的 `void` 选项写法（`lockdep_get_state_saver(SpinLock *, void *) -> Sentinel`），因为 ours 的
`StateSaver<Lock, Option>` 是用 `static_cast<Option *>(nullptr)` 做重载决议的：一个 `void *` 形参会经隐式的 `T* -> void*`
转换吞掉所有"锁没有声明 saver 的选项"，把本应编译失败的情况悄悄解析成哨兵类型。独立命名的钩子不会被意外命中。
`LocalLock` 不声明它：`LocalLock::acquire()` 自己关抢占，不带选项的 guard 是安全的。`Mutex` 也不声明。

调用点的规则（各子系统的记录里逐处列了理由）：

- 自旋锁一律 `Guard<SpinLock, IrqSave>`。
- 持锁范围会进 `reschedule_common()` 或会唤醒线程的，`Guard<SpinLock, IrqSaveAndPreemptDisable>`。

调用点的修改按子系统分别提交，每个都单独构建、启动过：`driver-clock.md`（i8253）、`sync.md`（Mutex、Event）、
`kernel-task.md`（线程、等待队列、IPI、CPU stopper，含原 `repairs/260924/kernel-task.md` 第 1 条 `do_suspend`）、
`kernel-task-sched.md`（调度器）、`mm.md`「mm 其余的自旋锁也都写明 IrqSave」、`x86-mm.md`（页表）。
最后加上 `static_assert` 重新全量构建，没有剩下的调用点。

**参照**
- Zircon `LOCK_DEP_POLICY_OPTION`、`AmbiguousOption`、`LockPolicyType`（`fuchsia/src/lib/lockdep/include/lockdep/lock_policy.h`）；
  `Guard` 的 `static_assert(!std::is_same<LockPolicy<LockType, Option>, AmbiguousOption>::value, ...)`（同目录 `guard.h`）。
- Zircon `kernel/include/kernel/spinlock.h`：`LOCK_DEP_POLICY_OPTION(SpinLock, NoIrqSave, ...)`、`(SpinLock, IrqSave, ...)`；
  `SpinLock::Acquire()` 里 `DEBUG_ASSERT(arch_ints_disabled())`——Zircon 的自旋锁一律关中断持有。

**验证**
- 全量构建（232 步）通过：加上 `static_assert` 后没有任何编译错误，即树里不再有不带选项的 SpinLock guard。
- 反向编译测试：用 i8253.cpp 的编译参数对一个临时文件 `-fsyntax-only`，`Guard guard{spin_lock}` 报出上面的
  static_assert；`Guard<sync::SpinLock, IrqSave>` 和 `Guard guard{mutex}` 照常编译。
- `system/lib/lockdep/tests/run_tests.py --suite smoke`：Apple clang 21 下三个特性级别全过。用树里的 clang 23 编不过，原因是
  f711ced 上就有的 `-Wunused-but-set-global`（`g_last_violation`），与本改动无关。
- clang-format 干净。
- QEMU：dev 与 dbg_1 各启动 3 次，外加 dbg_1 `SMP=1`、`CPU=max` 各 1 次：dev 都到 `init: shell closed its channel`、
  `--- end ---`；dbg_1 都到 `shell: probes done (11 killed, 0 survived)`、`--- end ---`，无断言。

**故意没做**
- **没有推送** ours-dev/lockdep 的 `feat/guard-option-required`：提交只在本机的模块仓库里，gitlink 指向它。推送与合并
  要由维护者决定。
- 没有加 Zircon 的 `NoIrqSave`（断言中断已关、不保存）：嵌套在已关中断区域里的 IrqSave 只多一次 pushf/popf；它和
  eager-resched 选项一起属于 `docs/interface.md` 的 LockPolicy 改造。
- 没有让 `SpinLock::acquire()` 像 Zircon 那样断言中断已关：那会把 IrqSave 从"约定"变成"强制"，但也会禁止
  PreemptDisable-only 的持有方式，是另一个决定。
- clang 23 下 smoke 测试的 `-Wunused-but-set-global` 未修，另案。
