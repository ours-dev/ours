# kernel-task-sched outstanding repairs

Fair (`fair.hpp`, `fair.cpp`) is out of the build and is the author's. It was renamed onto the current framework on 2026-11-03 (`docs/changes/261103/kernel-task-sched.md`), mechanically and no further. What is left needs the author's decision, and the first item is in the way of the rest: with `SCHED_NORMAL_DISCIPLINE_FAIR` on, `fair.cpp` has 23 diagnostics and `scheduler.cpp` 3, nearly all of them its cascade.

## 1. `SchedEntity::deadline()` no longer exists, and the two ways to answer it mean different things

**Severity**
Medium: Fair does not compile until it is answered, and one answer changes how Fair orders and preempts.

**Symptom**
`fair.hpp` (the `Lesser` lambda that orders `FairRunQueue`) and `Fair::update_thread()` (`ctx.deadline() <= ctx.exec_runtime()`) call `deadline()`. Because the first is in the definition of `FairRunQueue`, every use of the queue fails too: `queue<Discipline::Normal>()` cannot deduce its return type, which is most of the 23 diagnostics.

**Cause**
The framework replaced `deadline()` with `virtual_deadline()`, which returns `vdeadline`, because `deadline()` returned a field that nothing writes (`sched_entity.hpp`, the comment on `virtual_deadline()`). So the old code ordered every thread by zero and tested "zero <= exec_runtime", which is always true. The rename to `virtual_deadline()` compiles, and turns the ordering into a real order and the test into a comparison of a virtual-time value with real execution time, since `SchedTime` and `SchedDuration` are the same type. That is EEVDF semantics, so it was not applied.

**Change**
The author decides what the test compares and what orders the queue. This item first, since it makes the compiler's list readable.

**Reference**
None, this project's own design.

**Acceptance**
`python3 -I <fair_check> kernel/kernel/task/sched/fair.cpp` (the syntax check with the Fair switch on) reports no `deadline` diagnostic.

## 2. `FairRunQueue` has no `for_each`

**Severity**
Medium: a compile error, invisible until item 1 is answered because the compiler does not report a missing member of a class that has an invalid one.

**Symptom**
`RunQueue::for_each_thread()` (`run_queue.hpp`) calls `m_normal_queue.for_each(visit)`; `RrRunQueue` has one (`visit(SchedEntity &) -> bool`) and `FairRunQueue` does not.

**Cause**
The framework asks every discipline's queue for it after the run queue's fields were sealed. What is in the queue, and whether the running thread counts, is Fair's to say.

**Change**
Add `for_each` to `FairRunQueue` with the shape of `RrRunQueue::for_each`.

**Reference**
`rr_run_queue.hpp`.

**Acceptance**
No `for_each` diagnostic with the Fair switch on.

## 3. Three helpers reach for the calling CPU's queue

**Severity**
Medium: wrong across CPUs once Fair runs, and the half-converted file mixes two queues in one call chain.

**Symptom**
`is_object_eligible()`, `update_vlag()` and `set_next_object()` call `Scheduler::Current::get().fair_run_queue()`, which no longer exists. Their callers (`dequeue_thread()`, `evaluate_next_thread()`, `try_to_delay_dequeue()`, `override_thread_base_profile()`) now use the queue of the `RunQueue` they were handed.

**Cause**
The helpers have no `RunQueue` parameter. The framework hands a discipline the run queue of the CPU it is working on, and a wakeup runs on the waker's CPU against the target's queue; the "calling CPU's queue" is the accident `rr.cpp` records.

**Change**
Either the callers pass the queue they have to the three helpers, or the helpers keep the local CPU's and the limit is stated. The first is the one that is right for a wakeup.

**Reference**
`rr.cpp`, the comment on `RoundRobin::charge()`.

**Acceptance**
No `fair_run_queue` diagnostic; a cross-CPU wakeup test of Fair, which does not exist yet.

## 4. `set_next_thread()` re-arms the preemption timer the framework already arms

**Severity**
Low: dormant, and compiled out unless `SCHED_NOHZ` is on, but it does not compile.

**Symptom**
`runtime.arm_preempt_timer(remaining_timeslice)` under `if constexpr (SCHED_NOHZ)` has no counterpart: the member is gone.

**Cause**
`Scheduler::set_next_thread()` now sets the preemption deadline at every pick from `remaining_slice()`, after the discipline's own `set_next_thread()` and without a `SCHED_NOHZ` condition (`scheduler_inline.hpp`). The block's `consumed_timeslice` is `m_exec_runtime - m_prev_exec_runtime` computed right after `set_next_object()` made them equal, so it is zero and the block would arm the same full slice twice.

**Change**
Delete the block, or say what it adds.

**Reference**
None, this project's own design.

**Acceptance**
No `arm_preempt_timer` diagnostic.

## 5. Fair's charging runs from a tick that nothing delivers

**Severity**
Medium: it compiles, and changes what Fair does under the tickless scheduler.

**Symptom**
Nothing calls `Fair::on_thread_tick()` (`RoundRobin`'s is empty), so `update_thread()` and its deadline test run only at enqueue, dequeue, put_prev and yield. `UpdateThread`, `OverrideThreadBaseProfile` and `MigrateThread` of the interface also have no caller, and `OnThreadDead`'s call is commented out.

**Cause**
A slice now ends at the CPU's preemption deadline, a slot in its `TimerQueue`, and nothing ticks (`kernel-design`, references/task.md, Time). Fair was written for a tick that charged the running thread and asked for preemption. `on_thread_tick` also adds `add_pending()` with its default, which names the executing CPU rather than the run queue's (`run_queue.hpp`: say `mark_preempt_pending()`).

**Change**
The author decides how Fair ends a slice: from the deadline the framework sets, or by charging at another point.

**Reference**
None, this project's own design.

**Acceptance**
A Fair boot in which a thread that does not block is preempted when its request is spent.

## 6. Found by reading, not run: the queue count drifts and the first thread gets saturated times

**Severity**
Low to Medium: both are older than the adaptation, were read off the code, and have not been run, because Fair does not build.

**Symptom**
(a) `FairRunQueue::m_num_queued` gains one for every park of a running thread: a woken thread does `inc_queued()`; `set_next_object()` takes the running one off the tree without `dec_queued()`; when it parks, `Fair::dequeue_thread()` sees it is current and skips `dec_queued()`, but `Scheduler::dequeue_thread()` still subtracts one. `num_queued()` and the scheduler's count then differ, so `yield_thread()`'s `!rq.num_queued()` guard stops firing and the fast path in `pick_next_thread()` goes false. (b) With one runnable thread the tree is empty and `weight_sum_` is zero (`current_` is only read, never written), `avg_vruntime()` divides by it, `Fixed::operator/` answers the maximum on a zero divisor, and `adjust_vlag()` stores saturated `vruntime` and `vdeadline`, so the thread sorts after every other.
Also still open: `Fair::unpark_preempt()` takes its two `Thread *` in the reverse of the interface's order (item 2 of `docs/repairs/260917/kernel-task-sched.md`).

**Cause**
(a) The count is kept in two places that the dequeue path updates unequally. (b) `current_` has no writer.

**Change**
The author's: both are in Fair's bookkeeping.

**Reference**
None, this project's own design.

**Acceptance**
A host-style test of `FairRunQueue` for each, which does not exist yet.
