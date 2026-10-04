# kernel-task 待修项（2026-09-24）

## 3. `suspend_or_kill()`'s Sleeping branch wakes a sleeper it should leave alone, with the wrong status

**Severity**
Medium: wrong behaviour, on a path no system call reaches yet.

**Symptom**
`sleep.test` prints "the early-woken sleep answered Ok". A suspend or kill of a thread in `Thread::Current::sleep_etc()` wakes it even when it sleeps with `Interruptible::No`, and an interruptible sleeper woken this way reads `Ok`, as if it had slept its full time.

**Cause**
The Sleeping branch calls `m_waiter_state.unsleep(this, Status())` without the `is_interruptible()` check the Parked branch makes, and with a default-constructed status (`Ok`) rather than the kill or retry status the Parked branch now passes. A non-interruptible sleep is a promise that only the deadline ends it; the kernel's own bring-up sleeps rely on that.

**Change**
As in the Parked branch: return with only the signal set when the sleeper is not interruptible, and otherwise unsleep with `wakeup_status` (`InternalIrqKilled` or `InternalIrqRetry`).

**Reference**
Zircon `Thread::SuspendOrKillInternal()` (kernel/kernel/thread.cc): one interruptible check for `THREAD_BLOCKED` and `THREAD_SLEEPING` alike, then `wait_queue_state_.Unsleep(this, wakeup_status)`.

**Acceptance**
`sleep.test` checks that an interruptible sleeper woken early answers `InternalIrqRetry`, and that a non-interruptible one is not woken before its deadline.

## 4. `Thread::Current::tid()` 读的是 `%gs:0x70`，不是当前线程的 tid（已交接给独立会话）

它把 `&(CURRENT_INSTANCE->m_tid)` 交给 `CpuLocal::read()`；`CURRENT_INSTANCE` 是从不被写入的
每 CPU 模板里的空指针，地址折叠成常量 `offsetof(Thread, m_tid)` = 0x70，于是生成 `movq %gs:0x70, ...`。
每个 prof 样本的 tid 都是垃圾值。改为 `Thread::Current::get()->m_tid`。`PmNode::Current::hard_affinity()`
（`kernel/mm/include/ours/mm/pm_node.hpp`）是同一个错误，属于 mm。
