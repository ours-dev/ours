# kernel/task/sched 改动记录（2026-09-25）

## 调度器里的运行队列锁、PI 锁都写明 IrqSave（2026-09-25）

**文件**
- `kernel/kernel/task/sched/scheduler.cpp` — 九处不带选项的 Guard 改为 `Guard<SpinLock, IrqSave>`：批量 unpark 的 PI 锁、
  `tick_thread()` 的 PI 锁与运行队列锁、迁移链表锁、`reschedule_common()`、`do_unpark_thread()`、
  `find_active_scheduler_for()`、`yield_thread()`、`yield_to_thread()` 的运行队列锁
- `kernel/kernel/task/sched/scheduler_pi.cpp` — `Pi::common()` 的运行队列锁
- `kernel/include/ours/task/sched/run_queue.hpp` — `m_rqlock`、`m_migrating_threads_lock` 声明处写明持有规则

**症状**
当前没有观察到，也基本碰不到：这些位置几乎都跑在已关中断的上下文里——`tick_thread()` 在 tick 中断里，`reschedule_common()`
入口断言关中断，`yield_*()` 由持 `IrqSaveAndPreemptDisable` 的 `Thread::Current::yield()` 调进来，批量 unpark 的调用者
`Event::signal()` 显式关了中断。是"碰巧安全"。

**原因**
`Guard guard{lock}` 推导为 `Guard<SpinLock>`，没有 saver，锁本身不碰中断。改之前：

```cpp
    Guard pi_guard{current->get_pi_lock()};
    Guard rq_guard{get_lock()};
```

运行队列锁由 tick 在中断里拿，PI 锁由 tick 出口的 `Thread::Current::preempt()` 在中断里拿。这两把锁的安全完全靠"调用者
恰好已经关了中断"，而写法上与一把裸锁没有区别：哪天从一个开着中断的线程上下文调进来（比如一个新的 `unpark_thread()`
调用者），就是同 CPU 自锁死，而读代码的人看不出这里依赖什么。

**改法**
全部写成 IrqSave：

```cpp
    // Interrupts are already off -- this runs from the tick -- and both guards
    // are IrqSave all the same: each lock is also taken in thread context, and
    // is only ever held with interrupts off.
    Guard<SpinLock, IrqSave> pi_guard{current->get_pi_lock()};
    Guard<SpinLock, IrqSave> rq_guard{get_lock()};
```

嵌套在已关中断的区域里，saver 保存"关"、恢复"关"，代价一次 pushf/cli/popf。

`reschedule_common()` 的那一处要单独说清楚，因为它的 guard 会 `unguard()`：锁经 `switch_context()` 交出去、在切换前被放掉，
但 saver 是这一帧的成员，不跟着锁走。它一直留在这一帧里，直到本线程被切回来、从 `reschedule_common()` 返回时恢复"关"，
再由调用者自己的 guard 恢复它保存的状态。所以 unguard 与 IrqSave 不冲突：

```cpp
    Guard<SpinLock, IrqSave> guard{get_lock()};
    ...
    guard.unguard();

    // Everything be ok. Let's proceed the final context switch
    switch_context(curr, next);
```

批量 `unpark_thread()` 的 PI 锁只要 IrqSave，不加关抢占：它要求的重调度收集在 `to_resched` 里，循环结束、所有 PI 锁都放掉
之后才 `reschedule_mask()`。

**参照**
- Linux `rq->lock`：只在关中断下持有——线程上下文用 `rq_lock_irqsave()` / `task_rq_lock()`，`sched_tick()` 在中断里
  直接拿（`kernel/sched/core.c`）。
- Zircon `Scheduler::queue_lock_`（`zircon/kernel/include/kernel/scheduler.h`），同样只在关中断下持有。

**验证**
- 构建通过；clang-format 干净；clang-tidy 在改动行上无新告警。
- QEMU：dev 到 `init: shell closed its channel`、`--- end ---`；dbg_1 到 `shell: probes done (11 killed, 0 survived)`、
  `--- end ---`，无断言。

**故意没做**
- `fair.*` 没有碰（其中没有不带选项的 Guard）。
- 没有加 Zircon 那种"断言中断已关、不再保存"的 `NoIrqSave` 选项。它能省掉嵌套时的 pushf/popf，但这是 lockdep 的新接口，
  等 `lockdep/docs/interface.md` 的 LockPolicy 改造一起做。
