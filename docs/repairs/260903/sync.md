# sync / lockdep — 待修

已交给另一个 agent 处理。这里只记清楚缺陷本体和受影响面，避免各处各自绕开。

---

## 1. `Guard<sync::SpinLock, IrqSave>` 不关中断

`ours::IrqSave`（`kernel/include/ours/lockdep.hpp:11`）是个**空 tag struct**。
`lockdep::Guard<LockType, Options...>` 只是把它塞进 `std::tuple<bool, Options...>`，
没有任何地方读；`Guard::guard()` 调的是普通 `m_lock.acquire()`。

`kernel/include/ours/sync/spin_lock.hpp` 底部有

```cpp
LOCKDEP_STATE_SAVER(SpinLock, lockdep::SavedStateOption::IrqSave, ::arch::AutoInterruptDisabler);
```

它声明了本该让这个组合变真的映射，但 `Guard` 从不消费 `StateSaver`——**而且它命名的是
`lockdep::IrqSave`，和所有调用点传的 `ours::IrqSave` 是两个不同类型**。

后果：全树每一处写着 `Guard<..., IrqSave>` 的临界区，中断都是开着的。凡是「线程上下文拿锁 +
中断处理里拿同一把锁」的地方都可能同 CPU 自死锁。已知受影响：

- `kernel/kernel/time/timer_queue.cpp` — `activate_timer()` / `Timer::cancel()` 对 tick
- `kernel/kernel/task/sched/scheduler.cpp:118` — thread PI 锁
- `kernel/mm/page_queues.cpp`
- `kernel/kernel/task/thread.cpp` — `sleep_etc()` 的 PI 锁

已经踩到并单独绕过的一处：`ClockSource::Default::read()` 原来为读一个字拿这把锁，
而 APIC 校准会紧密轮询它几十毫秒——那是最容易撞上窗口的地方。已改成
`std::atomic<Ticks>` 的无锁读（`docs/repairs-20260830.md` 第三十六节第 2 步），
这是去掉了锁本身，不是绕开缺陷。

正确的修法是让 `Guard` 真正消费 `StateSaver`，并把 `ours::IrqSave` 和 `lockdep::IrqSave`
这两个同名不同源的类型统一。**其他模块不要本地分叉一个自己的 guard**——
试过 `TimerQueue::LockGuard`，已按要求撤回。
