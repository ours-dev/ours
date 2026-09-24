# kernel-task 待修项（2026-09-24）

## 1. `do_suspend` 在普通 Guard 下拿自己的 PI 锁再调度

`kernel/kernel/task/thread.cpp` 的 `Thread::Current::do_suspend()` 用 `Guard guard{current_thread->get_pi_lock()}`
（不关中断、不关抢占）拿自己的 PI 锁，然后在锁内调用 `Scheduler::Current::reschedule(current_thread)`。

这与 `suspend_or_kill` 修之前是同一类问题（`docs/changes/260924/kernel-task.md` 第二节）：持有自己的
PI 锁时来一个时钟中断，中断出口的 `Thread::Current::preempt()` 在同一颗 CPU 上再拿这把锁，自锁死。
也违反了调度器的约定——调用 `reschedule_common` 的地方都在 `Guard<SpinLock, IrqSave...>` 下持有当前
线程的锁。

`process_pending_signals()` 进 `do_suspend()` 前已经 `local_irq_enable()`，所以这条路径上中断确实是开的。

P1-A 没有任何代码设置 Suspend 信号，所以够不到；第一个 `task_suspend`（P1-B 之后）落地之前要改：
Guard 换成 `Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable>`，并核对 `reschedule` 返回之后锁与
中断的状态约定。
