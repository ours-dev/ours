# kernel-task-sched outstanding repairs

Fair (`fair.hpp`, `fair.cpp`) is the author's, and off by default (`sched_normal_discipline` is `"rr"`). It was renamed onto the current framework on 2026-11-03, mechanically, and its two `deadline()` lines were then given the virtual-clock reading the author chose (`docs/changes/261103/kernel-task-sched.md`). The direction of the weights, the scale of the average, the clock the run queue reads, the queue's zero and its running thread were fixed on 2026-11-03 (`docs/changes/261103/kernel-task-sched.md`); their items 1, 2, 3, 5, 6, 15 and 16 are gone (item 5 is the three helpers that now take their `RunQueue`, item 15 the exact `WeightedTime` sum), and items 4 and 12, `FairRunQueue::for_each` and the preemption-timer block, were fixed on 2026-11-04. The numbers are not reused, because the change records cite them (so they are not contiguous, and items 13, 14, 17 and 18, which those fixes left open, item 20, found while the Fair lane was brought up, and items 22 to 26, 30, 31 and 33 to 35, left by the idle-thread steal (item 27, the audit of the callers of `CpuLocal::access()`, was done and became items 28 to 33; items 28, 29 and 32 were fixed the same day in their own commits), are at the end; item 21, work stealing, was fixed on 2026-11-05). What is left is below. With `SCHED_NORMAL_DISCIPLINE_FAIR` on, `fair.cpp` and `scheduler.cpp` have no diagnostics: Fair compiles, links and boots in a lane built with `--args 'sched_normal_discipline="fair"'` (`tools/set.py bringup.x86 --out out/fair-on ...`). Items 7 to 11 compile; they are wrong or missing, or (item 8) a question for the author. Every claim below was read off the code, and the arithmetic was recomputed from the constants, except those marked observed, which were run in that lane.

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
`Fair::adjust_vlag()` places a thread at `avg_vruntime() - vlag`, and the block meant to adjust `vlag` (`if (vlag != SchedDuration{0}) { // Adjust the vlag. }`) is empty. The lag was measured against the mean with the thread in it (`update_vlag()` before the dequeue) and is applied against the mean without it (and, for the threads of one `steal()` batch, against a victim's mean that shrinks with each dequeue and a thief's that grows with each enqueue, so two threads that were level can arrive apart by the lag of the first: read from `Fair::adjust_vlag()` and `dequeue_thread()`, not run), which is shifted toward the other threads: a thread that left with a lag of -6.1 ms beside one equal-weight thread comes back with -3.1 ms, exactly `W / (W + w)` of it, in a model that a reviewer ran on 2026-11-04. Read and modelled, not run.

**Cause**
The stub was left for the author. The mean moves when the thread joins, so the lag in terms of the new mean is the old one scaled by `(W + w) / W`.

**Change**
The author's: scale `vlag` by `(W + w) / W` before applying it, with `W` the weight of what is in the mean, as the reference does, and bound it by a lag limit.

**Reference**
Linux EEVDF, `place_entity()` (`lag *= (load + weight) / load`) and `entity_lag()` for the bound (`kernel/sched/fair.c`, local checkout, v7.2-rc5 plus 31 commits; the formula is from memory).

**Acceptance**
In the test of item 13, a thread that leaves and returns while the others run is placed at the lag it left with, within a nanosecond.

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

## 22. Busy CPUs are never balanced against each other

**Severity**
Medium: an idle CPU is served now, but a split among CPUs that all have work stays as it is; round-robin and Fair alike.

**Symptom**
`Scheduler::Current::steal()` runs only when a CPU has nothing to run (the idle loop), so a CPU that runs one thread never looks at the others. In `hotplug.test`, eight spinners on four CPUs, one CPU offline and back: the survivors hold 3, 3 and 2, the returning CPU takes one and the counts are 2, 3, 2, 1 (by inference from the code, not observed: the test does not print them), and they stay so for as long as the spinners run, where an even split is 2, 2, 2, 2. A pick-time pull does not exist either: `pick_next_thread()` still calls the stub `steal_work_from()`.

**Cause**
The idle loop was chosen as the place for a pull because it holds no run queue lock and is entered on every path a CPU becomes idle (including `IdleThread::come_back()`). That serves the CPU that has nothing; it does not serve one that has less than its neighbour.

**Change**
The author's, with options to put to the user: a periodic balance from the tick (Linux `sched_balance_domains()` and `sched_balance_rq()`), a pull at the pick (Zircon `Scheduler::DequeueThread()` calls `StealWork()` inside `queue_guard.CallUnlocked(...)`, with its own queue lock dropped and interrupts still off; it saves a switch to the idle thread and back, and does not run at come-back time), or a push when a queue grows. Each needs the own lock dropped inside the pick, or a tick-side step with its own cost; none is chosen.

**Reference**
Linux `sched_balance_domains()` and `sched_balance_rq()` (`kernel/sched/fair.c`, local checkout; names confirmed, bodies not read for this item); Zircon `Scheduler::StealWork()` (`scheduler.cc`).

**Acceptance**
In `hotplug.test`, after `bring_cpu_online()` the spinner counts per online CPU differ by at most one (2, 2, 2, 2) within a second; `steal.test` still passes.

## 23. An idle CPU that found nothing looks again only when something wakes it

**Severity**
Low: a queue that reaches the threshold after the CPU halted is served late, not never, since a CPU's own wakeups and the first timer tick bring it back.

**Symptom**
An idle CPU that finds no queue deep enough halts with no tick (`IdleThread::run()`) and looks again only when an interrupt wakes it. `steal.test` sends the reschedule request by hand for that reason: after the test widens the spinners' affinity, nothing else tells the thief that the victim's queue is now stealable. Read off the code, shown by the test, not measured for how long a real case waits.

**Cause**
`steal()` is called once per pass through the idle loop, and the loop blocks in `hlt`. Nothing wakes an idle CPU when another CPU's queue grows past the threshold: placement (`select_target_cpu()`) prefers the least loaded CPU, so new threads go to idle CPUs, and the remaining cases are a thread whose affinity was widened, a thread that wakes onto a busy CPU because it last ran there, and threads that were pinned.

`steal()` also tries only the busiest eligible CPU of each tier, so a queue of threads that may not run here (all pinned to it) hides a shallower one that has a thread to give: the idle CPU halts with that thread waiting a slice behind the other CPU's running one. Seen on 2026-11-05 in `steal.test` with four highest-priority threads pinned to a third CPU of the thief's node: that CPU was the busiest, nothing on it could move, and `took 0 of 3` from the real victim (with one such thread the CPU does not qualify, and the round passes). A fix tries the next busiest CPU of the tier when the first gives nothing, bounded.

**Change**
The author's: when a thread is queued on a CPU that already runs one, kick an idle CPU that could take it (Linux has a kick for idle balance, `nohz_balancer_kick()`, from memory; Zircon sends a reschedule IPI from `Scheduler::Insert()` to a CPU picked by `FindTargetCpu()` for the same purpose, from memory), or poll from a tick that the idle CPU keeps while another CPU has a queue. Neither is chosen.

**Reference**
Linux `nohz_balancer_kick()`; Zircon `Scheduler::Insert()`, `Scheduler::FindTargetCpu()` (names from memory, not read for this item).

**Acceptance**
A test that widens the affinity of a waiting thread with no reschedule request sent by hand sees the thread on the idle CPU within a bounded time, and says what the bound is.

## 24. The steal thresholds and the node preference are first guesses, unmeasured

**Severity**
Low: a performance choice with a safe direction, since a threshold that is too high steals less and a too-low one only moves a thread early.

**Symptom**
`STEAL_MIN_RUNNABLE_SAME_NODE = 2` and `STEAL_MIN_RUNNABLE_OTHER_NODE = 3` (`domains.cpp`, the `min_runnable` of the two tiers `sched_domain::build_domains()` makes) count the running thread, so one waiting thread is enough on the same node and a second is asked for from another. They were chosen by reasoning, not by data. The QEMU machine used for all runs has no remote-memory latency (TCG), so no run to date can show a difference; `steal.test` checks which side of the node boundary the count decides, not whether the number is right, nor the order (the same node before another). The count also ignores how long a thread has waited, which Zircon's cross-cluster test uses (`kInterClusterThreshold`, two milliseconds of queued time). `STEAL_MAX_BATCH_SAME_NODE` and `STEAL_MAX_BATCH_OTHER_NODE` (4 each), the most threads one look takes, are guesses of the same kind, and the load itself is the run queue's weight sum, an instantaneous value (a sampled, decayed load is the candidate to measure it against).

**Cause**
No estimate of queued time exists in the scheduler, and no real NUMA machine has run this kernel.

**Change**
Per CLAUDE.md, before changing anything: take the measurement first and put each candidate to the user, with its mechanism and cost. The measurement is the time to the first run of a stolen thread and the throughput of a memory-bound workload split over two nodes, for thresholds of 2, 3 and 4, batches of 1, 2, 4 and 8, the weight sum against a sampled load, and a queued-time gate; the report goes in `docs/perf/<YYMMDD>/sched-steal-threshold.md` with its raw samples (`scripts/perf_stats.py`).

**Reference**
Zircon `Scheduler::StealWork()` and its search set ordered by cache distance (`scheduler.cc`); Linux `sched_balance_newidle()`, which gives up when `this_rq->avg_idle < sd->max_newidle_lb_cost` (`kernel/sched/fair.c`, local checkout, read on 2026-11-05).

**Acceptance**
The report exists, shows the throughput and the latency distribution per threshold on hardware with at least two nodes, and names the value kept.

## 25. A thread whose affinity is narrowed while it is queued stays where it is

**Severity**
Medium: a hard affinity can be broken, for a time, by a thread that is queued or in the middle of a move.

**Symptom**
`Scheduler::set_cpu_affinity()` installs the new mask without any run queue lock and, if the CPU the thread is on is no longer allowed, calls `Scheduler::migrate_thread()`, which is an empty stub. A thread that is queued on a CPU its new hard affinity excludes therefore stays queued and runs there. The same hole is wider since `steal()`: the affinity is checked once under the victim's lock, and a mask installed after that, before the thread is queued on the thief, leaves it on a CPU the mask excludes. `deactivate()` has it too (it re-selects the destination, so the window is shorter). Read off the code, not run.

**Cause**
`migrate_thread()` was never written, and affinity is not under a lock that the run queue lock orders against.

**Change**
The author's: write `migrate_thread()` for a Ready thread (take it off its queue, place it again) and for a Running one (a reschedule request), and decide how a thread in the Migrating window is told about a new mask, for instance by re-checking the mask under the thief's lock in `steal()` and handing the thread to `find_active_scheduler_for()` when it fails.

**Reference**
Zircon `Scheduler::ChangeCpuAffinity` path (names from memory); Linux `__set_cpus_allowed_ptr()` and `affine_move_task()` (names from memory).

**Acceptance**
A test narrows the hard affinity of a queued spinner to another CPU and sees it run there and never again on the first, with `steal.test` and `hotplug.test` passing.

## 26. The priority-inheritance walk does not ask whether the thread is queued where its CPU says

**Severity**
Medium: not reached today, since nothing calls `Scheduler::Pi::common()`.

**Symptom**
Latent, cannot be triggered today: nothing calls `Scheduler::Pi::common()`. (A drift in `m_num_queued_threads` or `m_total_weight` hurts more since `steal()`: they used to be written and not read to decide anything, and now choose a victim and an amount. The window is also up to `MAX_BATCH` threads wide, and a thread waits there for the ones before it to be queued on the thief.) It reads the thread's `curr_cpu()`, takes that CPU's run queue lock and, for a Ready thread, dequeues and re-enqueues it. A thread in the Migrating window (taken off a queue by `deactivate_thread()` and not yet queued again) still names its old CPU, and is Ready, so the walk would dequeue it from a queue it is not in: `Scheduler::dequeue_thread()` decrements `m_num_queued_threads` and `m_total_weight` without a membership test, and the re-enqueue would put the thread on the old CPU's queue while it is activated on another. Read off the code, not run. The window is `deactivate()`'s and `steal()`'s.

**Cause**
`assert_thread_in_scheduler()` checks the CPU, not the queue, and the Migrating transient state is not looked at.

**Change**
Look at the transient state under the lock (the thread's `RunQueueState`) and skip the dequeue and re-enqueue when it is Migrating, leaving the new priority to be read when the thread is activated.

**Reference**
None, this project's own design.

**Acceptance**
A test changes the priority of a thread while another CPU moves it (a stress with `steal()` running) and ends with the queue counts equal to the queues' contents.

## 30. `DpcRunner::enqueue()` resolves the CPU twice

**Severity**
Medium: not reached today, its only production caller holds interrupts off.

**Symptom**
Latent, cannot be triggered today: the one caller outside tests holds both disablers. `DpcRunner::enqueue()` (`kernel/kernel/task/dpc.cpp`) reads `mm::CpuLocal::current_cpu()` and then `Current::get()`. A caller that can be preempted between them and taken by another CPU queues the `Dpc` on one runner and records the other CPU as its owner, and `cancel()` then locks one CPU's queue to erase from the other's list. `Thread::Current::exit()` is the only caller outside tests and holds both disablers; `dpc.test` and `hotplug.test` pin their threads, so pinning is all that keeps them safe. A reviewer's reading, not run.

**Cause**
The public function says "the calling CPU" without making it so.

**Change**
Take the interrupt (or preemption) disabler first in `enqueue()`, or take the CPU from the runner.

**Reference**
None, this project's own design.

**Acceptance**
`enqueue()` does not depend on its caller's context; `dpc.test` passes.

## 31. `ChainLockTransaction::current()` reads a per-CPU slot with no protection

**Severity**
Medium: not reached today, nothing calls it.

**Symptom**
Latent, cannot be triggered today: nothing calls it. `ChainLockTransaction::current()` (`kernel/lib/kconcurrent/src/chain_lock_transaction.cpp`) is documented for an assertion in code handed no transaction, which is the caller that is not inside one and may be preemptible. It uses the two-load `CpuLocal::access()`, so such a caller can be moved between the loads and read another CPU's transaction. The constructor and destructor are safe (the interrupt disabler is their first member). A reviewer's reading, not run.

**Cause**
The function does not say, or make, the context it needs.

**Change**
Document that it needs interrupts off, or take the disabler.

**Reference**
None, this project's own design.

**Acceptance**
The function states or enforces its context, with a caller of it as the reason.

## 33. A remote write of a CPU's preemption state is not atomic

**Severity**
Low: speculative, Fair lane only as far as the reviewer found, and a few instructions wide.

**Symptom**
In the Fair lane, `Fair::dequeue_thread()` calls `update_thread()` on the victim's running thread, whose deadline test can call `RunQueue::mark_preempt_pending()`, which calls `PreemptionState::add_pending()` on that thread: a load, an or and a store with no atomic read-modify-write (`preemption_state.hpp`). The thread's own CPU, outside the run queue lock, can be doing the same on another bit (a remote wakeup that requests an IPI); the interleaving loses one bit, and a lost bit is a reschedule request that is never sent, so a woken thread waits for the next interrupt. Needs the Fair lane, a steal from a CPU whose running thread has spent its request, and a remote wakeup by that thread in the same few instructions. A reviewer's reading, not run; round-robin never calls `mark_preempt_pending()` from a dequeue.

**Cause**
`add_pending()` was written for the CPU that owns the state. The existing remote callers (`unpark_preempt()` on a waker's behalf) use the same function.

**Change**
The author's: make `add_pending()` an atomic read-modify-write (`fetch_or`) if the state may be written from another CPU, or keep the remote write off the running thread's state.

**Reference**
None, this project's own design.

**Acceptance**
`add_pending()` is a single atomic operation, and a stress of remote wakeups against steals in the Fair lane loses no request.

## 34. `preempt.test` fails when `tickreplace.test` ran before it

**Severity**
Low: a test that depends on the order of the tests before it; the kernel is not shown to be wrong.

**Symptom**
`python3 scripts/kshell_run.py --out out/<dir> tickreplace.test preempt.test` ends with `preempt.test: FAIL  no turn runs past its slices` and `FAIL  most turns are a whole number of slices` (one run: 12 turns, 2 of them a whole number of slices, the longest 69937 us, the probe timer 1320 us late), in the default, Fair and four-node lanes alike. After `idle`, `sleep`, `threadjoin`, `timer`, `timershare` or `mutex.stress` instead, it passes, and alone it passes. With `Scheduler::Current::steal()` returning false at its top it fails the same, so it is not the work stealing. Observed on 2026-11-05 while a batch was being rerun; `preempt.test` was fixed on the same day for a different failure (`docs/changes/261103/kernel-task-sched.md`).

**Cause**
Not established. `tickreplace.test` replaces the tick device of the CPUs; if it leaves a different device or mode behind (a coarser timer, a longer lateness), a turn that `preempt.test` expects to end within its slice plus the lateness it measures runs longer. A guess from the symptom, not read.

**Change**
Read what `tickreplace.test` restores at its end and compare with what `preempt.test` assumes (`late by at most` in its first log line is measured before the spinners start); either restore fully in the first or measure in the second after the state it finds.

**Reference**
None, this project's own tests.

**Acceptance**
`kshell_run.py --out out/<dir> tickreplace.test preempt.test` passes in the default, Fair and four-node lanes, three runs in a row.

## 35. The scheduling domains are built once and do not follow a CPU that moves to another node

**Severity**
Medium: not reached today, since nothing moves a CPU between nodes at run time.

**Symptom**
Latent, cannot be triggered today: nothing calls `PmNode::set_cpu_node()`. `domains_of(cpu)` (`kernel/kernel/task/sched/domains.cpp`) is built once, at `InitLevel::Threading`, from `mm::CpuNodeMap`. `PmNode::set_cpu_node()` (`kernel/mm/pm_node.cpp`) moves a CPU between two live nodes at run time and updates the node table; it has no caller, and nothing tells the scheduler. After such a move a CPU would still be in the tier its old node gave it: `steal()` would count a CPU as near that is far, and the other way round. Read off the code, not run.

**Cause**
The table is read-only after boot, so that `steal()` takes no lock to read it, and the memory manager has no hook through which a move could tell the scheduler (a call from `kernel/mm` into the scheduler would be a new edge; mm already includes task headers, `ours/task/thread.hpp` for one).

**Change**
The author's, with options: a generation count in `CpuNodeMap` that the scheduler compares at each look and rebuilds on a change (a rebuild that has to be safe against `steal()` readers on other CPUs); or a hook list that `set_cpu_node()` runs and the scheduler adds itself to, registered at `Threading`; or leaving it until a caller of `set_cpu_node()` exists, and making that caller rebuild. None is chosen.

**Reference**
Linux rebuilds its sched domains on a topology change with `rebuild_sched_domains()` (`kernel/cgroup/cpuset.c`, local checkout; name confirmed, body not read for this item).

**Acceptance**
A test moves a CPU to another node through `PmNode::set_cpu_node()` and `steal.test` sees the new tier of that CPU, or the item is closed by the first caller of `set_cpu_node()` doing so.
