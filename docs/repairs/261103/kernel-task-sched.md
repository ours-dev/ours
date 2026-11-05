# kernel-task-sched outstanding repairs

Fair (`fair.hpp`, `fair.cpp`) is the author's, and off by default (`sched_normal_discipline` is `"rr"`). It was renamed onto the current framework on 2026-11-03, mechanically, and its two `deadline()` lines were then given the virtual-clock reading the author chose (`docs/changes/261103/kernel-task-sched.md`). The direction of the weights, the scale of the average, the clock the run queue reads, the queue's zero and its running thread were fixed on 2026-11-03 (`docs/changes/261103/kernel-task-sched.md`); their items 1, 2, 3, 5, 6, 15 and 16 are gone (item 5 is the three helpers that now take their `RunQueue`, item 15 the exact `WeightedTime` sum), and items 4 and 12, `FairRunQueue::for_each` and the preemption-timer block, were fixed on 2026-11-04. The numbers are not reused, because the change records cite them (so they are not contiguous, and items 13, 14, 17 and 18, which those fixes left open, and items 19 and 20, which the first Fair lane showed, are at the end). What is left is below. With `SCHED_NORMAL_DISCIPLINE_FAIR` on, `fair.cpp` and `scheduler.cpp` have no diagnostics: Fair compiles, links and boots in a lane built with `--args 'sched_normal_discipline="fair"'` (`tools/set.py bringup.x86 --out out/fair-on ...`). Items 7 to 11 compile; they are wrong or missing, or (item 8) a question for the author. Every claim below was read off the code, and the arithmetic was recomputed from the constants, except those marked observed, which were run in that lane.

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
Low: it compiles and works, and is a question of design under the tickless scheduler.

**Symptom**
Nothing calls `Fair::on_thread_tick()` (`RoundRobin`'s is empty), so `update_thread()` and its deadline test run only from `enqueue_thread()`, `dequeue_thread()`, `put_prev_thread()`, `yield_thread()`, `yield_to_thread()` and, since 2026-11-04, `evaluate_next_thread()` (and from `override_thread_base_profile()`, which nothing calls). `UpdateThread`, `OverrideThreadBaseProfile` and `MigrateThread` of the interface also have no caller, and `OnThreadDead`'s call is commented out. At the end of a slice the deadline test fires in the charge at the pick (`evaluate_next_thread()`), where the replenish is what matters and `mark_preempt_pending()` is redundant (the pending bit is consumed in `reschedule_common()` by `take_pending_local()` right after the pick); the charge in `put_prev_thread()` that follows adds nothing when the pick went through `Fair::evaluate_next_thread()`, because the scheduler clock is not read again between the pick and the hand-over (a pick that returns early, for an idle transition or a `yield_to` nomination, makes that charge the first).

**Cause**
A slice now ends at the CPU's preemption deadline, a slot in its `TimerQueue`, and nothing ticks (`kernel-design`, references/task.md, Time). Fair was written for a tick that charged the running thread and asked for preemption. `on_thread_tick()` also calls `add_pending()` with its default, which names the executing CPU rather than the run queue's (`run_queue.hpp`: say `mark_preempt_pending()`).

**Change**
The author decides how Fair ends a slice: from the deadline the framework sets, charged at the pick and at the hand-over as it is now, or by charging at another point.

**Reference**
Linux EEVDF, `pick_task_fair()` calls `update_curr()` before the pick (`kernel/sched/fair.c`, local checkout).

**Acceptance**
`Fair::on_thread_tick()` and the interface entries with no caller are removed or called from somewhere, any `add_pending()` that stays in them is `mark_preempt_pending()`, and `preempt.test` still passes on the Fair lane (it does since 2026-11-04).

## 9. A pick restarts the request, and a yield can push the deadline two requests out

**Severity**
Low to Medium: both change what "the request is spent" means.

**Symptom**
`set_next_object()` rewrites `vdeadline` and `m_prev_exec_runtime` at every pick, so the deadline test means "one request since the last pick, placement or replenish", not "the request was served". A thread preempted at 90 % of its slice, again and again, never replenishes through the test. The same restart would happen in the middle of a slice if `Scheduler::Pi::common()`, which calls `put_prev_thread()` and `set_next_thread()` on the running thread, were called; nothing calls it today, and `Scheduler::on_thread_base_profile_overrided()` is empty. `yield_thread()` does `vdeadline += to_virtual_time(slice, weight)` after an `update_thread()` that may just have replenished, so the deadline can sit two requests ahead of `vruntime`. The weight scaling also truncates on a quantised rate, so charging a slice in several pieces can leave `vruntime` short of the deadline by up to a nanosecond per piece, and the test then fires one charge later. Observed on 2026-11-04 in the Fair lane, with the running thread charged before the pick: `preempt.test` (two spinners on one CPU, slice 64 ms) still shows turns of two slices, 130 to 132 ms, in more than half of the boots (2 of 4 in the experiment, 10 of 14 after the change, and 13 of 20 in a reviewer's); the round-robin lane has none. A reviewer's exact-integer model of the queue explains them as near-ties, which no run was instrumented to show: a waiter that is ahead of the mean by a sliver, because timer latency gave it a slightly longer slice last turn, is ineligible, the running thread is picked again, and `set_next_object()` and `Scheduler::set_next_thread()` give it a whole new slice though it is owed only the sliver. `preempt.test` allows two slices in a row for this reason.

**Cause**
Fair keeps the request in two places that are written independently.

**Change**
The author's: keep the deadline across picks, as the algorithm does, and let only the replenish and the placement write it. For the re-pick, end the slice when the first waiter becomes eligible instead of arming a whole one: that is the waiter with the smallest `vruntime`, and the running thread's virtual time must advance by `(v_waiter - mean) * W / w_running` for it to reach the mean, `W` being the weight of everything in the mean, the running thread included. Not tried, nor was a tolerance on eligibility; the deadline-ordered tree does not give the smallest `vruntime` without a search.

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

## 13. No test shows that CPU shares follow the weights

**Severity**
Low: a missing test. The weight fix is checked for its arithmetic only, so a wrong use of it elsewhere in Fair would not be caught.

**Symptom**
`common.hpp` proves at compile time that `to_virtual_time()` is inversely proportional to the weight at every priority, that `MID_WEIGHT`'s clock reads real time, and the units of `weighted_offset()` against the weight sum. Nothing checks `FairRunQueue` or `Fair::update_thread()` for it: the kernel's own tests run Fair only in the lane that selects it, and none measures shares, so no test shows that two threads of different weights receive CPU in the ratio of their weights.

**Cause**
The acceptance of the two fixed items was a host-style test of `FairRunQueue`. It was blocked on Fair compiling, which it does now; what is missing is a harness for `FairRunQueue` on the host, or a `*.test` command that spins threads of two priorities and reports their charged time.

**Change**
Add the test: two threads that always want to run, at priorities 31 and 15, over a long window, with the ratio of the CPU time each was charged within a stated tolerance of the ratio of their weights (about 25.7); the eligibility case, where threads that have been served in proportion to their weights are all eligible and one that is ahead of the weighted average is not; one thread alone, which has a finite `vdeadline` one request ahead of its `vruntime` and a lag of zero through `lag_of()` at every priority; a wakeup from another CPU, which leaves `weight_sum_` equal to the sum of the weights in the tree; and the walk, which visits the waiting threads in deadline order, not the running one, and stops when the visitor answers false.

**Reference**
None, this project's own test.

**Acceptance**
The test is in `//:default` and passes.

## 14. The real-to-virtual conversion calls `__divti3` on every charge

**Severity**
Low: a cost read off the code and the generated assembly, not measured, and Fair is off by default.

**Symptom**
`to_virtual_time()` (`common.hpp`) evaluates `real / (weight / MID_WEIGHT)`. `ffl::Fixed::operator/` shifts the numerator into an `__int128` and divides by an `__int128`. At `-O2` with the kernel's flags a function that calls it contains exactly one `__divti3`, for `real / rate`: clang turns the division by the constant `MID_WEIGHT` into multiplies. `mul_shr.hpp` says such a call is a software loop costing "tens to hundreds of cycles". It runs at every charge (`update_thread()`), pick (`set_next_object()`) and placement (`adjust_vlag()`), and in the replenish and the yield, and no thread skips it: the fast path for `weight == MID_WEIGHT` is taken only by a priority-16 thread, and the spawn sites use `DEFAULT_PRIORITY` (15). The factor `weight / MID_WEIGHT` depends on the weight alone, so it is recomputed with the same answer for a thread's every call.

**Cause**
It was written in the plainest form with the library's operators and nothing was optimised, on purpose.

**Change**
Performance work goes through the author, in depth (CLAUDE.md). Measure first: cycles per call of `to_virtual_time()` in a bench under `benches/`; the function is header-only, so it needs no Fair lane. Then weigh the candidates. A table of rates per priority, computed at compile time like the weight table, saves the multiplies and keeps the `__divti3`. Only a reciprocal removes it, and a reciprocal table is the `wmult` table that `kernel-design` excludes and that this project built and removed (3e0f49ce), so it is a departure for the author to decide. Not decided.

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
The author's: a state that says "in this CPU's tree", or a check of the intrusive hook, and the delayed-dequeue wake needs a decision before the feature is enabled, and so does the offline walk, which would hand a delayed sleeper to `Scheduler::Current::deactivate()` (`deactivate_thread()` asserts the thread is not sleeping).

**Reference**
Linux EEVDF tracks `se->on_rq` and `cfs_rq->curr` separately for this (from memory, not checked). Its `reweight_entity()` takes the entity off the sums and puts it back around a weight change.

**Acceptance**
A test that nominates a thread queued on another CPU, and one that wakes a delayed thread, both leave `weight_sum_` equal to the sum of the weights in the tree, and a profile change on a running thread does too.

## 18. The lag a thread carries is not rescaled when it is placed

**Severity**
Low: the effect is a fraction of a request of service, and Fair is off by default.

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

## 19. `hotplug.test` fails on the Fair lane

**Severity**
Medium: a test that passes under round-robin fails under Fair, and the cause is not established. Not reachable in the default build.

**Symptom**
Observed on 2026-11-04 in the Fair lane, in two boots by the author and one by a reviewer: `hotplug.test` fails 2 of its 47 checks, "and every spinner goes on elsewhere", in two of its rounds (`hotplug_test.cpp`: while a CPU is offline for `OFFLINE_FOR`, every spinner's round counter must have advanced). The round-robin lane passes 47 of 47. The other checks pass, among them the ones that walk the run queue (`FairRunQueue::for_each` through `Scheduler::Current::deactivate()`): the CPU goes offline, no spinner runs on it, and the sleeper wakes on time elsewhere.

**Cause**
Not established. The three-slice bursts that the charge at the pick removed from `preempt.test` (change record of 2026-11-04) were not the cause: the test fails 2 of its checks in all six boots of the experiment (base, charged first, and charged first with the running thread competing, two boots each; the harness counted the failed checks and did not record their names), and fails "and every spinner goes on elsewhere" twice in each of the three boots after the change (two with the first form of the test, one with the final code). The bursts were gone in four of the six boots of the experiment (all but the base's two) and in the three after it. The total is not always 47 (one reviewer boot reported 2 of 53), which the investigation should explain. Two-slice turns remain, about 130 ms against an `OFFLINE_FOR` of 100 ms, and the turns in `hotplug.test` were not measured, so the near-ties of item 9 are not excluded. Candidates, each read and not run: a spinner that was moved to another CPU finds two or more spinners already there and one is starved for `OFFLINE_FOR` by the near-ties of item 9; the lag a moved thread carries from the old CPU's mean is applied against the new CPU's mean unscaled (item 18); a moved thread is enqueued with a `vruntime` placed on the other queue's zero.

**Change**
Find the cause first: print each spinner's rounds at the end of the sleep for the Fair lane, and its CPU. Then the author decides.

**Reference**
None, this project's own test.

**Acceptance**
`hotplug.test` passes on the Fair lane.

## 20. `SCHED_NOHZ` has no reader

**Severity**
Low: a build argument that does nothing.

**Symptom**
`sched_nohz` (`kernel_params.gni`) becomes the define `SCHED_NOHZ` (`products/BUILD.gn`), and nothing reads the define since the block in `Fair::set_next_thread()` was deleted on 2026-11-04; `features.cpp` mentions it in a comment to say it is unrelated.

**Cause**
The block was the only reader.

**Change**
Remove the argument and the define, or give it a meaning. The author decides; nothing at run time depends on it.

**Reference**
None, this project's own design.

**Acceptance**
No reference to `SCHED_NOHZ` or `sched_nohz` outside the history.
