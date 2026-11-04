# kernel-task-sched outstanding repairs

Fair (`fair.hpp`, `fair.cpp`) is out of the build and is the author's. It was renamed onto the current framework on 2026-11-03, mechanically, and its two `deadline()` lines were then given the virtual-clock reading the author chose (`docs/changes/261103/kernel-task-sched.md`). What is left is below. With `SCHED_NORMAL_DISCIPLINE_FAIR` on, `fair.cpp` has five diagnostics (items 4, 5 and 12) and `scheduler.cpp` one (item 4); items 1 to 3 and 6 to 11 compile and are wrong or missing. Every claim below was read off the code, and the arithmetic was recomputed from the constants; none of it was run, because Fair does not build.

## 1. The weights run the wrong way

**Severity**
High: with Fair on, a heavier thread gets less of the CPU, not more.

**Symptom**
`calc_weighted_time(duration, weight)` (the top of `fair.cpp`) returns `duration * (weight / MID_WEIGHT)`. `update_thread()` advances `m_fair.vruntime` by it, and `adjust_vlag()`, `update_thread()`'s replenish and `set_next_object()` build `vdeadline` as `vruntime + calc_weighted_time(slice, weight)`. A `Weight` is a share in (0, 1] (`MAX_WEIGHT` is 1, `MID_WEIGHT` about 0.0476), so a thread at the top priority advances about 21 times faster than real time and has its deadline about 21 times further away. By hand: a thread of weight 1 and one of `MID_WEIGHT` have each run 10 ms; the first has advanced `vruntime` by about 210 ms and the second by 10 ms, and, placed at the same `vruntime`, their deadlines are 64 ms times 21 and 64 ms ahead, so the light one is picked about 21 slices before the heavy one runs again.

**Cause**
A virtual-time delta divides by the weight (a heavier thread's clock runs slower, so it is owed more real time before its deadline), and the helper multiplies. The same helper is also used for the numerator of the weighted average, where multiplying is right (item 2), so one function serves two opposite directions.

**Change**
Split it: a conversion from real to virtual time that divides by the weight, for `vruntime` and the request, and the weighted term of the average that multiplies. The deadline test chosen on 2026-11-03 does not depend on this while a thread's weight stays constant (the factor is on both sides), so the order of the tree and the average are what it changes.

**Reference**
Linux EEVDF, `calc_delta_fair()` and `update_deadline()` in `kernel/sched/fair.c` (a reviewer read them in the first 100 kB of upstream master on 2026-11-03).

**Acceptance**
A host-style test of `FairRunQueue` with two threads of different weights, which does not exist yet: over a long window the ratio of their execution times approaches the ratio of their weights.

## 2. The average and the eligibility test are about 21 times too large

**Severity**
High: eligibility is true for nearly every thread, so the discipline picks by earliest deadline alone.

**Symptom**
`FairRunQueue::calc_weighted_time(object)` is `calc_weighted_time(exec_runtime - min_vruntime_, weight)`, which is `(exec - min) * weight / MID_WEIGHT`, and `total_vruntime_` sums it. `avg_vruntime()` divides the sum by `weight_sum_`, which is in absolute `Weight` units. By hand: two threads of `MID_WEIGHT`, `exec_runtime` 100 ms and 0, `min_vruntime_` 0: the sum is 100 ms and `weight_sum_` is about 0.095, so the average is about 1050 ms where the true mean is 50 ms. `is_object_eligible()` tests `total_vruntime >= (exec - min) * weight_sum`, that is 100 against about 9.5 for the first thread, and 100 against 0 for the second: both are eligible.

**Cause**
The numerator should be the sum of `weight * (v - v0)`; it is that sum divided by `MID_WEIGHT`. The denominator is right.

**Change**
The weighted term of the average multiplies by the weight and does not divide by `MID_WEIGHT`. Done with item 1, since it is the same helper.

**Reference**
Linux EEVDF, `avg_vruntime()` and `entity_eligible()` (`vruntime_eligible()`) in `kernel/sched/fair.c`: `sum_w_vruntime` is the sum of `key * weight`, eligible when `avg >= key * load`.

**Acceptance**
A host-style test: for threads that have received service in proportion to their weights, every thread is eligible, and a thread ahead of its share is not.

## 3. `exec_runtime()` stands in for `vruntime` in four places

**Severity**
High: Fair runs on two clocks until these are changed, and the decision of 2026-11-03 makes the change mandatory before Fair is enabled.

**Symptom**
`FairRunQueue::lag_of()`, `FairRunQueue::calc_weighted_time(object)`, `FairRunQueue::update_min_vruntime()` and `Fair::is_object_eligible()` read `exec_runtime()`, real time that starts at zero and is not scaled by the weight. `m_fair.vruntime`, the virtual clock, is read nowhere except to build `vdeadline` and by the deadline test. `adjust_vlag()` then computes `vruntime = avg_vruntime() - vlag`, with both operands on the real clock, and the tree is ordered by `vdeadline` values built on that, so two threads' deadlines have different origins.

**Cause**
The framework's own `exec_runtime()` comment says so: every caller uses it where the virtual quantity is meant (`sched_entity.hpp`).

**Change**
Those four read `m_fair.vruntime`. `FairRunQueue` is not a friend of `SchedEntity`, so it needs an accessor or a friend declaration. The real-time uses stay: the charge in `update_thread()`, `m_prev_exec_runtime` in `set_next_object()` and `remaining_slice()`.

**Reference**
Linux EEVDF keeps one clock, `se->vruntime`, for the average, the lag, eligibility, the deadline and the expiry test.

**Acceptance**
No call of the accessor `exec_runtime()` in `fair.hpp` or `fair.cpp` (the charge, the slice mark and the NOHZ block read `m_exec_runtime` directly); the tests of items 1 and 2.

## 4. `FairRunQueue` has no `for_each`

**Severity**
Medium: a compile error.

**Symptom**
`RunQueue::for_each_thread()` (`run_queue.hpp`) calls `m_normal_queue.for_each(visit)`; `RrRunQueue` has one (`visit(SchedEntity &) -> bool`) and `FairRunQueue` does not. It is the only diagnostic in `scheduler.cpp`.

**Cause**
The framework asks every discipline's queue for it after the run queue's fields were sealed. The contract is in `run_queue.hpp` (`for_each_thread()`): every queued thread is visited in the queue's order until `visit` answers false, and the running thread is not visited, so Fair's must walk `queue_` and not `current_`.

**Change**
Add `for_each` to `FairRunQueue` with the shape of `RrRunQueue::for_each`.

**Reference**
`rr_run_queue.hpp`.

**Acceptance**
No `for_each` diagnostic with the Fair switch on.

## 5. Three helpers reach for the calling CPU's queue

**Severity**
Medium: wrong across CPUs once Fair runs, and the half-converted file mixes two queues in one call chain.

**Symptom**
`is_object_eligible()`, `update_vlag()` and `set_next_object()` call `Scheduler::Current::get().fair_run_queue()`, which no longer exists. The helpers are called from `dequeue_thread()` and `override_thread_base_profile()` (`update_vlag()`), from `evaluate_next_thread()` and `try_to_delay_dequeue()` (`is_object_eligible()`) and from `set_next_thread()` (`set_next_object()`). All but `try_to_delay_dequeue()`, which takes a `RunQueue &` and ignores it, use the queue of the `RunQueue` they were handed.

**Cause**
The helpers have no `RunQueue` parameter. The framework hands a discipline the run queue of the CPU it is working on, and a wakeup runs on the waker's CPU against the target's queue; the "calling CPU's queue" is the accident `rr.cpp` records.

**Change**
The callers pass the queue they have to the three helpers.

**Reference**
`rr.cpp`, the comment at the top of the file (in the anonymous namespace), which says why the queue has to come from the `RunQueue` the entry point was handed.

**Acceptance**
No `fair_run_queue` diagnostic; a cross-CPU wakeup test of Fair, which does not exist yet.

## 6. `min_vruntime_` never advances, `current_` is never written, and an empty tree divides by zero

**Severity**
Medium: the average is anchored at zero for the life of the queue and never includes the running thread.

**Symptom**
`FairRunQueue::update_min_vruntime()` has no caller, so `min_vruntime_` stays at zero; and it could not advance it if it were called: it takes the smaller of the running thread's clock and `min_vruntime_`, so its `delta > 0` test is never true. `current_` has no writer, so `total_weight()` and `avg_vruntime()` never add the running thread. With an empty tree `weight_sum_` is zero, `Fixed::operator/` answers its maximum on a zero divisor, and `adjust_vlag()` stores a saturated `vruntime` and `vdeadline` for the first thread. It then sorts after every other thread, and because both sit at the maximum `vruntime >= vdeadline` holds on every charge: a preemption request each time, and a replenish that changes nothing. Under the comparison against real time that the rename alone would have given, the same thread would never have expired.

**Cause**
The function has no caller and its body only ever lowers the minimum (the TODO in it says the search of the run queue is missing); `current_` was declared and never written; the empty tree was not considered.

**Change**
The author's: make `update_min_vruntime()` take the larger of `min_vruntime_` and the smaller of the running thread's `vruntime` and the leftmost of the tree, and call it where the running thread is charged; set `current_` in `set_next_object()` and clear it in `put_prev_thread()`; and place the first thread on the average of an empty queue by some rule (its own start, or zero lag). The function reads `exec_runtime()`, so it is changed with item 3.

**Reference**
Linux EEVDF, `update_zero_vruntime()` and `place_entity()`.

**Acceptance**
A host-style test of `FairRunQueue`: one thread alone has a finite `vdeadline` one request ahead of its `vruntime`.

## 7. A thread that was never enqueued has a slice of zero

**Severity**
Low to Medium: the deadline test is true on every charge for it, so it is marked preempt-pending each time.

**Symptom**
`Timeslice::m_base` is set only in `adjust_vlag()`, which runs from `Fair::enqueue_thread()`; `Fair::on_thread_init()` is empty. A thread the scheduler never enqueues, such as an idle thread made by `init_first_thread()`, has a request of zero: `vdeadline` equals `vruntime`, so `vruntime >= vdeadline` always holds and `update_thread()` calls `mark_preempt_pending()` on every charge of it. The framework does not use the zero slice for an idler (`Scheduler::set_next_thread()` clears the preemption deadline for `m_idle_thread`), so a deadline in the past would arise only for a non-idle thread that is picked without ever being enqueued, which does not happen today.

**Cause**
`RoundRobin::on_thread_init()` sets the slice (`rr.cpp`) and Fair's was left empty.

**Change**
Set the slice in `on_thread_init()`, or give `Timeslice` a default.

**Reference**
`rr.cpp`, `RoundRobin::on_thread_init()`.

**Acceptance**
A boot with Fair on in which the idle threads are not marked preempt-pending on every charge.

## 8. Fair's charging runs from a tick that nothing delivers

**Severity**
Medium: it compiles, and changes what Fair does under the tickless scheduler.

**Symptom**
Nothing calls `Fair::on_thread_tick()` (`RoundRobin`'s is empty), so `update_thread()` and its deadline test run only from `enqueue_thread()`, `dequeue_thread()`, `put_prev_thread()`, `yield_thread()` and `yield_to_thread()` (and from `override_thread_base_profile()`, which nothing calls). `UpdateThread`, `OverrideThreadBaseProfile` and `MigrateThread` of the interface also have no caller, and `OnThreadDead`'s call is commented out. At the end of a slice the test is reached inside `put_prev_thread()`, after the pick, where `mark_preempt_pending()` is redundant (the pending bit is consumed in `reschedule_common()`) and the replenish is what matters.

**Cause**
A slice now ends at the CPU's preemption deadline, a slot in its `TimerQueue`, and nothing ticks (`kernel-design`, references/task.md, Time). Fair was written for a tick that charged the running thread and asked for preemption. `on_thread_tick()` also calls `add_pending()` with its default, which names the executing CPU rather than the run queue's (`run_queue.hpp`: say `mark_preempt_pending()`).

**Change**
The author decides how Fair ends a slice: from the deadline the framework sets, as it does now through `put_prev_thread()`, or by charging at another point.

**Reference**
None, this project's own design.

**Acceptance**
A Fair boot in which a thread that does not block is preempted when its request is spent.

## 9. A pick restarts the request, and a yield can push the deadline two requests out

**Severity**
Low to Medium: both change what "the request is spent" means.

**Symptom**
`set_next_object()` rewrites `vdeadline` and `m_prev_exec_runtime` at every pick, so the deadline test means "one request since the last pick, placement or replenish", not "the request was served". A thread preempted at 90 % of its slice, again and again, never replenishes through the test. The same restart would happen in the middle of a slice if `Scheduler::Pi::common()`, which calls `put_prev_thread()` and `set_next_thread()` on the running thread, were called; nothing calls it today, and `Scheduler::on_thread_base_profile_overrided()` is empty. `yield_thread()` does `vdeadline += calc_weighted_time(slice, weight)` after an `update_thread()` that may just have replenished, so the deadline can sit two requests ahead of `vruntime`. The weight scaling also rounds to nearest on a quantised rate, so charging a slice in several pieces can leave `vruntime` a few nanoseconds either side of the deadline.

**Cause**
Fair keeps the request in two places that are written independently.

**Change**
The author's: keep the deadline across picks, as the algorithm does, and let only the replenish and the placement write it.

**Reference**
Linux EEVDF, `update_deadline()`: the deadline is written at placement and when `vruntime` reaches it.

**Acceptance**
A host-style test: a thread preempted before its request is spent keeps its deadline when it is picked again.

## 10. The queue count drifts, and `unpark_preempt` has its parameters reversed

**Severity**
Low to Medium: both are older than the adaptation.

**Symptom**
(a) `FairRunQueue::m_num_queued` gains one for every park of a running thread: a woken thread does `inc_queued()`; `set_next_object()` takes the running one off the tree without `dec_queued()`; when it parks, `Fair::dequeue_thread()` sees it is current and skips `dec_queued()`, but `Scheduler::dequeue_thread()` still subtracts one. `num_queued()` and the scheduler's count then differ, so the fast path in `pick_next_thread()` goes false. The guard in `yield_thread()` (`!rq.num_queued()`) is unreliable for a second reason: `m_num_queued` counts the running thread, which `RoundRobin`'s count does not, so for an enqueued running thread the guard cannot mean "nothing else is queued". (b) `Fair::unpark_preempt()` takes its two `Thread *` in the reverse of the interface's order (item 2 of `docs/repairs/260917/kernel-task-sched.md`).

**Cause**
(a) The count is kept in two places that the dequeue path updates unequally.

**Change**
The author's.

**Reference**
None, this project's own design.

**Acceptance**
A host-style test of `FairRunQueue` for (a); the parameters in the interface's order for (b).

## 11. `override_thread_base_profile()` charges the wrong thread

**Severity**
Low: nothing calls it yet.

**Symptom**
It calls `update_thread(runtime, thread)` on a queued thread; `RunQueue::charge_current()` returns the interval of the CPU's running thread, so the running thread loses that charge for good and the queued one is charged for time it did not run. If the thread is the running one, `dequeue_object()` is then called on a thread that is not in the tree.

**Cause**
`update_thread()` charges "the current thread" whatever it is passed.

**Change**
Charge the running thread, or skip the charge for a queued one; take the running thread off the tree by the same rule as `dequeue_thread()`.

**Reference**
None, this project's own design.

**Acceptance**
A host-style test of a profile change on a queued thread and on the running one.

## 12. `set_next_thread()` re-arms the preemption timer the framework already arms

**Severity**
Low: dormant at run time (`SCHED_NOHZ` defaults to false), but it does not compile with `SCHED_NOHZ` on or off, because the discarded branch of an `if constexpr` in a non-template function is still checked.

**Symptom**
`runtime.arm_preempt_timer(remaining_timeslice)` under `if constexpr (SCHED_NOHZ)` has no counterpart: the member is gone.

**Cause**
`Scheduler::set_next_thread()` now sets the preemption deadline at every pick of a thread other than the idle thread (the idle pick clears it) from `remaining_slice()`, after the discipline's own `set_next_thread()` and without a `SCHED_NOHZ` condition (`scheduler_inline.hpp`). The block's `consumed_timeslice` is `m_exec_runtime - m_prev_exec_runtime` computed right after `set_next_object()` made them equal, so it is zero and the block would arm the same full slice twice.

**Change**
Delete the block, or say what it adds.

**Reference**
None, this project's own design.

**Acceptance**
No `arm_preempt_timer` diagnostic.
