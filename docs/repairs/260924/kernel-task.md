# kernel-task 待修项（2026-09-24）

## 2. `suspend_or_kill()` and `handle_timeout()` call through a `wait_queue()` that may be null

**Severity**
Medium: a null dereference, but only in a race no caller can start today.

**Symptom**
Latent, cannot be triggered today. A suspend, a kill or a timeout that targets a parked thread in the window after a queue-driven wake has claimed it dereferences null (dbg: `handle_timeout()`'s `DEBUG_ASSERT(wq)` first).

**Cause**
`Event::signal()` dequeues its waiters under the queue lock, which clears each waiter's `m_waitqueue`, then drops the queue lock and only afterwards takes each thread's PI lock to unpark it (`Scheduler::unpark_thread(UnparkList)`). In between, the thread is still `Parked` but in no queue. `Thread::suspend_or_kill()`'s Parked branch and `WaitQueue::handle_timeout()` both hold the PI lock, see `Parked`, read `wait_queue()` and call `wq->unpark_thread()` without checking for null. `unpark_thread()` itself already copes with a waiter claimed by someone else (`dequeue_waiter()` returns false), but it cannot be reached through a null `wq`.

**Change**
A null `wait_queue()` means the wake is already claimed: do nothing. The signal is set already, and the waker that holds the claim wakes the thread, which then meets the signal. Zircon's timeout handler returns on `wq == nullptr` in the same way.

**Reference**
Zircon `WaitQueue::TimeoutHandler()` (kernel/kernel/wait.cc), `if (wq == nullptr) { ... return Done; }`.

**Acceptance**
`waitqueue.test` and `boot_check.py` still pass; the null case is argued, not tested, as the window is a few instructions wide.

## 5. A waiter woken by a suspend or a kill reads `Status::Ok`

**Severity**
Medium: a wrong result, on a path no system call reaches yet.

**Symptom**
`waitqueue.test` prints "the suspended wait answered Ok": a thread suspended or killed while parked interruptibly returns from its wait as if the wait had been satisfied.

**Cause**
`Thread::suspend_or_kill()`'s Parked branch calls `wq->unpark_thread(this)` with the default status, `Status::Ok`. The waiter cannot tell an interruption from the event it waited for; an exception-channel wait would read a kill as "the handler closed the exception". The preamble of the same wait already answers a signal pending at park time with `InternalIrqKilled` or `InternalIrqRetry` (`Thread::check_kill_or_suspend_signal()`), so every caller handles those two already.

**Change**
Wake with `InternalIrqKilled` for a kill and `InternalIrqRetry` for a suspend, as the preamble does.

**Reference**
Zircon `Thread::SuspendOrKillInternal()` (kernel/kernel/thread.cc), `wakeup_status`.

**Acceptance**
`waitqueue.test` checks that the suspended wait answers `InternalIrqRetry`.

## 3. `suspend_or_kill` 的 Sleeping 分支与睡眠定时器竞争（已交接给独立会话）

Sleeping 分支不看 `is_interruptible()`、以 `Status::Ok` 唤醒；`sleep_etc` 的定时器回调在
`trylock_or_cancel` 成功后不检查线程是否仍在 Sleeping。提前唤醒之后定时器仍挂着，到期时会去
unpark 一个已是 Ready/Running 的线程（dbg_1 触发 `scheduler.cpp` 的状态断言，dbg_0 重复入队）。
改法照 Zircon：不唤醒不可中断的睡眠者；`HandleSleep()` 在 `state() != THREAD_SLEEPING` 时直接返回。

## 4. `Thread::Current::tid()` 读的是 `%gs:0x70`，不是当前线程的 tid（已交接给独立会话）

它把 `&(CURRENT_INSTANCE->m_tid)` 交给 `CpuLocal::read()`；`CURRENT_INSTANCE` 是从不被写入的
每 CPU 模板里的空指针，地址折叠成常量 `offsetof(Thread, m_tid)` = 0x70，于是生成 `movq %gs:0x70, ...`。
每个 prof 样本的 tid 都是垃圾值。改为 `Thread::Current::get()->m_tid`。`PmNode::Current::hard_affinity()`
（`kernel/mm/include/ours/mm/pm_node.hpp`）是同一个错误，属于 mm。
