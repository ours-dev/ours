# kernel-task-sched outstanding repairs

Fair (`fair.hpp`, `fair.cpp`) is out of the build and is the author's. It was renamed onto the current framework on 2026-11-03, mechanically, and its two `deadline()` lines were then given the virtual-clock reading the author chose (`docs/changes/261103/kernel-task-sched.md`). The direction of the weights, the scale of the average, the clock the run queue reads, the queue's zero and its running thread were fixed on 2026-11-03 (`docs/changes/261103/kernel-task-sched.md`); their items 1, 2, 3, 5, 6, 15 and 16 are gone (item 5 is the three helpers that now take their `RunQueue`, item 15 the exact `WeightedTime` sum) and the numbers are not reused, because the change records cite them (so the numbers are not contiguous, and items 13, 14, 17 and 18, which those fixes left open, are at the end). What is left is below. With `SCHED_NORMAL_DISCIPLINE_FAIR` on, `fair.cpp` has two diagnostics (items 4 and 12) and `scheduler.cpp` one (item 4); items 7 to 11 compile and are wrong or missing. Every claim below was read off the code, and the arithmetic was recomputed from the constants; none of it was run, because Fair does not build.

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
Nothing calls `Fair::on_thread_tick()` (`RoundRobin`'s is empty), so `update_thread()` and its deadline test run only from `enqueue_thread()`, `dequeue_thread()`, `put_prev_thread()`, `yield_thread()` and `yield_to_thread()` (and from `override_thread_base_profile()`, which nothing calls). `UpdateThread`, `OverrideThreadBaseProfile` and `MigrateThread` of the interface also have no caller, and `OnThreadDead`'s call is commented out. At the end of a slice the test is reached inside `put_prev_thread()`, after the pick, where `mark_preempt_pending()` is redundant (the pending bit is consumed in `reschedule_common()`) and the replenish is what matters. The same order means the pick reads the running thread's `vruntime` before it is charged, up to one slice old, for the mean and for the eligibility of the waiting threads. A model of the queue that a reviewer ran on 2026-11-04 leaves the long-run shares unchanged (25.69 against 25.71 for priorities 31 and 15) but doubles the worst short-term service error (121 ms against 61 ms) and makes an equal-weight pair run two-slice bursts.

**Cause**
A slice now ends at the CPU's preemption deadline, a slot in its `TimerQueue`, and nothing ticks (`kernel-design`, references/task.md, Time). Fair was written for a tick that charged the running thread and asked for preemption. `on_thread_tick()` also calls `add_pending()` with its default, which names the executing CPU rather than the run queue's (`run_queue.hpp`: say `mark_preempt_pending()`).

**Change**
The author decides how Fair ends a slice: from the deadline the framework sets, as it does now through `put_prev_thread()`, or by charging at another point. Charging the running thread at the top of `evaluate_next_thread()` also removes the stale read at the pick.

**Reference**
None, this project's own design.

**Acceptance**
A Fair boot in which a thread that does not block is preempted when its request is spent.

## 9. A pick restarts the request, and a yield can push the deadline two requests out

**Severity**
Low to Medium: both change what "the request is spent" means.

**Symptom**
`set_next_object()` rewrites `vdeadline` and `m_prev_exec_runtime` at every pick, so the deadline test means "one request since the last pick, placement or replenish", not "the request was served". A thread preempted at 90 % of its slice, again and again, never replenishes through the test. The same restart would happen in the middle of a slice if `Scheduler::Pi::common()`, which calls `put_prev_thread()` and `set_next_thread()` on the running thread, were called; nothing calls it today, and `Scheduler::on_thread_base_profile_overrided()` is empty. `yield_thread()` does `vdeadline += to_virtual_time(slice, weight)` after an `update_thread()` that may just have replenished, so the deadline can sit two requests ahead of `vruntime`. The weight scaling also truncates on a quantised rate, so charging a slice in several pieces can leave `vruntime` short of the deadline by up to a nanosecond per piece, and the test then fires one charge later.

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

## 13. No test shows that CPU shares follow the weights

**Severity**
Low: a missing test. The weight fix is checked for its arithmetic only, so a wrong use of it elsewhere in Fair would not be caught.

**Symptom**
`common.hpp` proves at compile time that `to_virtual_time()` is inversely proportional to the weight at every priority, that `MID_WEIGHT`'s clock reads real time, and the units of `weighted_offset()` against the weight sum. Nothing runs `FairRunQueue` or `Fair::update_thread()`, because Fair does not build, so no test shows that two threads of different weights receive CPU in the ratio of their weights.

**Cause**
The acceptance of the two fixed items was a host-style test of `FairRunQueue`. It cannot be written until Fair compiles (items 4 and 12).

**Change**
Add the test when those are done: two threads that always want to run, at priorities 31 and 15, over a long window, with the ratio of the CPU time each was charged within a stated tolerance of the ratio of their weights (about 25.7); the eligibility case, where threads that have been served in proportion to their weights are all eligible and one that is ahead of the weighted average is not; one thread alone, which has a finite `vdeadline` one request ahead of its `vruntime` and a lag of zero through `lag_of()` at every priority; and a wakeup from another CPU, which leaves `weight_sum_` equal to the sum of the weights in the tree.

**Reference**
None, this project's own test.

**Acceptance**
The test is in `//:default` and passes.

## 14. The real-to-virtual conversion calls `__divti3` on every charge

**Severity**
Low: a cost read off the code and the generated assembly, not measured, and Fair does not build.

**Symptom**
`to_virtual_time()` (`common.hpp`) evaluates `real / (weight / MID_WEIGHT)`. `ffl::Fixed::operator/` shifts the numerator into an `__int128` and divides by an `__int128`. At `-O2` with the kernel's flags a function that calls it contains exactly one `__divti3`, for `real / rate`: clang turns the division by the constant `MID_WEIGHT` into multiplies. `mul_shr.hpp` says such a call is a software loop costing "tens to hundreds of cycles". It runs at every charge (`update_thread()`), pick (`set_next_object()`) and placement (`adjust_vlag()`), and in the replenish and the yield, and no thread skips it: the fast path for `weight == MID_WEIGHT` is taken only by a priority-16 thread, and the spawn sites use `DEFAULT_PRIORITY` (15). The factor `weight / MID_WEIGHT` depends on the weight alone, so it is recomputed with the same answer for a thread's every call.

**Cause**
It was written in the plainest form with the library's operators and nothing was optimised, on purpose.

**Change**
Performance work goes through the author, in depth (CLAUDE.md). Measure first: cycles per call of `to_virtual_time()` in a bench under `benches/`; the function is header-only, so this can run before Fair builds. Then weigh the candidates. A table of rates per priority, computed at compile time like the weight table, saves the multiplies and keeps the `__divti3`. Only a reciprocal removes it, and a reciprocal table is the `wmult` table that `kernel-design` excludes and that this project built and removed (3e0f49ce), so it is a departure for the author to decide. Not decided.

**Reference**
Linux `calc_delta_fair()` and `__calc_delta()` replace the division with a multiply by a precomputed reciprocal (from memory, not checked).

**Acceptance**
A report in `docs/perf/` with the raw samples of the baseline and of each stage.

## 17. Tree membership is taken from `is_active()`

**Severity**
Low to Medium: none of the paths is reachable today (Fair is off, `SCHED_DELAY_DEQUEUE` is off, nothing calls `override_thread_base_profile()`), and each corrupts the run queue's sums once it is.

**Symptom**
`put_prev_thread()`, `set_next_object()` and `override_thread_base_profile()` use `runqueue_state().is_active()` for "this thread is in the tree", but it is also true of the running thread and of a thread queued on another CPU. (a) `Fair::yield_to_thread()` accepts any active thread and `pick_next_thread()` returns the nomination without `evaluate_next_thread()`; `set_next_object()` then calls `dequeue_object()` on this CPU's tree for a node that may be in no tree or in another CPU's, which corrupts `weight_sum_` and `total_vruntime_`, and `set_current()` then points `current_` at a thread that is not running here, which `sums()` adds. (b) A delayed dequeue returns `Status::Delayed` and leaves the thread active and in the tree; `put_prev_thread()` inserts it again, and a later wake runs `adjust_vlag()` on it, rewriting `vruntime` while it is in the tree, and then `enqueue_object()` a second time. `adjust_vlag()` is the one writer of the clock that `exec_runtime` did not have, so the new clock is safe only for a thread that is out of the tree. (c) Called on the running thread, `override_thread_base_profile()` would erase a node that is not in the tree (item 11 has the charge) and then insert it, leaving `current_` set: the thread is in `sums()` twice. (d) `Scheduler::Pi::common()` on a running thread calls `put_prev_thread()`, which inserts it with its old weight, changes the effective profile, and calls `set_next_thread()`, which removes it with the new one: `weight_sum_` and `total_vruntime_` end wrong by the difference. A model with the weight changed from 0.0389 to a larger one left `weight_sum_` negative. Nothing calls `Pi::common()` and `recompute_effective_profile()` has no definition.

**Cause**
`is_active()` says the thread is on this discipline's accounting, not that its node is in this CPU's tree, and nothing else records the second.

**Change**
The author's: a state that says "in this CPU's tree", or a check of the intrusive hook, and the delayed-dequeue wake needs a decision before the feature is enabled.

**Reference**
Linux EEVDF tracks `se->on_rq` and `cfs_rq->curr` separately for this (from memory, not checked). Its `reweight_entity()` takes the entity off the sums and puts it back around a weight change.

**Acceptance**
A test that nominates a thread queued on another CPU, and one that wakes a delayed thread, both leave `weight_sum_` equal to the sum of the weights in the tree, and a profile change on a running thread does too.

## 18. The lag a thread carries is not rescaled when it is placed

**Severity**
Low: the effect is a fraction of a request of service, and Fair does not build.

**Symptom**
`Fair::adjust_vlag()` places a thread at `avg_vruntime() - vlag`, and the block meant to adjust `vlag` (`if (vlag != SchedDuration{0}) { // Adjust the vlag. }`) is empty. The lag was measured against the mean with the thread in it (`update_vlag()` before the dequeue) and is applied against the mean without it, which is shifted toward the other threads: a thread that left with a lag of -6.1 ms beside one equal-weight thread comes back with -3.1 ms, exactly `W / (W + w)` of it, in a model that a reviewer ran on 2026-11-04. Read and modelled, not run.

**Cause**
The stub was left for the author. The mean moves when the thread joins, so the lag in terms of the new mean is the old one scaled by `(W + w) / W`.

**Change**
The author's: scale `vlag` by `(W + w) / W` before applying it, with `W` the weight of what is in the mean, as the reference does, and bound it by a lag limit.

**Reference**
Linux EEVDF, `place_entity()` (`lag *= (load + weight) / load`) and `entity_lag()` for the bound (`kernel/sched/fair.c`, local checkout, v7.2-rc5 plus 31 commits; the formula is from memory).

**Acceptance**
In the test of item 13, a thread that leaves and returns while the others run is placed at the lag it left with, within a nanosecond.
