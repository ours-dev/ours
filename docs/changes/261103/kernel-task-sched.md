# kernel-task-sched changes

## Title: the scheduler reaches a discipline through a ustl::Ref, and the virtual interface, the registry and the per-thread pointer are gone (2026-11-03)

**Files**
- `kernel/include/ours/task/sched/schedule.hpp` — `ISchedule`, `Schedule<Derived, Discipline>` and `ScheduleEntry` are replaced by the `Schedule` interface (a `ustl::Dyn` of seventeen methods) and one dispatch per method, five required and twelve optional with their defaults beside them
- `kernel/include/ours/task/sched/scheduler.hpp`, `kernel/kernel/task/sched/scheduler.cpp` — `schedule_of(Discipline)`, `call_schedule<>()` and `call_schedule_unlocked<>()` in place of `m_schedules`, `set_schedule()` and `get_schedule()`; every call site goes through them
- `kernel/include/ours/task/sched/scheduler_inline.hpp` — the two calls of `put_prev_thread` and `set_next_thread`
- `kernel/include/ours/task/sched/sched_entity.hpp` — `m_schedule` and `schedule()` removed
- `kernel/include/ours/task/sched/rr.hpp`, `kernel/kernel/task/sched/rr.cpp` — `RoundRobin` is a plain class; the explicit instantiation of its registrar is gone
- `kernel/kernel.ld` — the `.task.schedules` block and its two bounds symbols removed
- `kernel/include/ours/task/sched/types.hpp`, `run_queue.hpp` — a forward declaration and a comment that named `ISchedule`
- `system/lib/ustl/include/ustl/dyn.hpp` — the two macros exempt the dispatch from the thread-safety analysis; two constructor parameters renamed for `clang-tidy`
- `.agents/skills/kernel-design/references/task.md`, `docs/design/260918/kernel-task-sched.md`, `.agents/tasks/sched-facade/DESIGN.md` — the rule, the note that the 260918 seam was superseded, and Fair's adaptation list

**Symptom**
None observable: the boot, the host tests and the scheduling kernel tests behave as before. What it removes is a set of failures the old seam made possible, each of which had already happened once (`schedule.hpp`'s own comments): a registration that the linker's catch-all swallowed, one that `--gc-sections` dropped, one that a class template's static member never emitted, and a call through a thread's `m_schedule` before it was bound, which jumped to address 0.

**Cause**
**The seam promised that adding a discipline needs no change to `Scheduler`, and kept the promise to nobody while charging for it.** `docs/design/260918/kernel-task-sched.md` section 1 records that adding a discipline still meant editing `Scheduler`. What the old design cost: eighteen virtual entry points with runtime defaults (`Status::Unimplemented`) for the ones a discipline did not have; a CRTP base whose only job was to place one shared instance into a linker section; a loop in `Scheduler::init()` that sorted that section into an array by enumerator; and a pointer cached in every thread, null from creation until a bind step nobody could skip. The set of disciplines is the enumerators of `Discipline`, so a `switch` over them says everything the registry said.

Two details came out of the move that were not visible before.

1. **"Outranks" was decided by an address.** `Scheduler::unpark_preempt()` compared `x_schedule > y_schedule`, the addresses of two disciplines' shared instances, which are `static inline` members of different class templates and have no order the language or the linker promises. The branch could never run (one discipline is implemented), so nothing was wrong yet, but it would have decided on a coin the day a second discipline arrived. It now compares the enumerators: a later one outranks an earlier one, Deadline over Normal.
2. **Where the thread-safety check lives.** The first build of this change failed with eight errors of the form `calling function 'enqueue_thread' requires holding SpinLock 'forward(args).m_rqlock'`. Once `RoundRobin`'s functions carry `TA_REQUIRES(rq.lock())` (the old base declared them, so the derived declarations never had to), the analysis looks into the dispatch struct that calls them, finds no lock held, and cannot connect the lock to the argument that names it: the dispatch forwards its arguments without knowing which one is the run queue. So the dispatch is exempt (`[[clang::no_thread_safety_analysis]]` in the two macros), and the check is made one step earlier, in `Scheduler::call_schedule<>()`, which is `TA_REQUIRES(get_lock())` and so makes every caller show that it holds the lock. The erased call itself is the one unchecked hop, as it was always going to be (`.agents/tasks/sched-facade/DESIGN.md`); before, the check was on the virtual declaration, and now it is on the wrapper.

**Change**
The call sites, before and after:

```cpp
// before
auto status = so.schedule()->enqueue_thread(m_run_queue, thread, options);

// after
auto status = call_schedule<EnqueueThread>(so.discipline(), m_run_queue, thread, options);
```

The registry and the binding, before and after:

```cpp
// before: init() sorts the linker section into m_schedules; each thread binds a pointer
for (auto i = 0; i < num_schedule; ++i) { m_schedules[slot] = entry.instance; }
auto *const schedule = self.set_schedule(thread);

// after: a switch over the closed enum
auto Scheduler::schedule_of(Discipline const discipline) -> ustl::Ref<Schedule> {
    switch (discipline) {
    case Discipline::Normal:
        return ustl::Ref<Schedule>{NORMAL_SCHEDULE};
    case Discipline::Deadline:
    case Discipline::Count:
        break;
    }
    return {};
}
```

`NORMAL_SCHEDULE` is a `constinit` object of `RoundRobin`, or of `Fair` where `SCHED_NORMAL_DISCIPLINE_FAIR` selects it, the way `NormalRunQueue` is chosen. Deadline is not implemented and gives an empty reference; `pick_next_thread()` skips it (it used to skip a null slot), and `call_schedule<>()` asserts, in debug builds, that a thread's discipline has a schedule. The per-thread pointer is gone: each call looks the schedule up by `so.discipline()`, an index into a switch of two enumerators.

Rejected: keeping `m_schedule` as a `Ref` per thread (16 bytes per thread, and the null-before-bind problem back), and a generic visitor lambda for the dispatch (the analysis cannot connect the lambda's captured run queue to its dispatcher, as the probe in the design showed).

**Reference**
Linux `struct sched_class` and `sched_class_above()` (`kernel/sched/sched.h`, from memory, not opened): a table of function pointers per class, with the classes' priority taken from their address, which the linker script lays out in order. This project had the address comparison without the linker script that makes it mean something. Zircon has no counterpart: `Scheduler` (`zircon/kernel/include/kernel/scheduler.h:83-85`) is one class for fair and deadline threads. In-tree, `ustl::Dyn` (`docs/changes/261103/ustl.md`).

**Verification**
- Built and tested in a clean worktree (`.worktrees/sched-dyn`, HEAD `f8b72a7c` plus this diff), because the main checkout's build is held up by another session's uncommitted edits.
- The whole tree builds with `-Werror -Wthread-safety`: every caller of `call_schedule<>()` holds the run queue lock, as the analysis requires.
- `python3 .agents/skills/verify/scripts/boot_check.py`: dbg PASS, shell finished at 5.7 s, 46 passed, 5 skipped, 0 failed; rel PASS, 44 passed, 8 skipped, 0 failed, 7/7 required; host-tests PASS, 164 passed. The counts are the same as before the change.
- On the dbg image with four CPUs, `scripts/kshell_run.py` for `waitqueue.test`, `threadjoin.test`, `preempt.test`, `dpc.test`, `timer.test`, `timershare.test` and `hotplug.test` (CPUs going offline and coming back, 47 checks): all ok. `pagereclaim.test` (21 checks) was run once, before two constructor parameters of `dyn.hpp` were renamed.
- `SMP=1` could not be used: it fails on the one-CPU boot's error-level line `CPU[0] tick device I8253 failed ...`, the known item 1 of `docs/repairs/261030/kernel-time.md`, before the scheduler matters.
- `scripts/format_cpp.py` was run on every changed C++ file. `clang-tidy -p out/bringup.x86` on `scheduler.cpp` and `rr.cpp` reports nothing on a changed line; what it reports is older (`sched_entity.hpp` `[[nodiscard]]` hints, `return 0` in `steal_work_from()`). It did flag two parameter names of `dyn.hpp` from the commit before, which are fixed here.
- Not measured: the cost of the call. An erased call through a `Ref` is one indirect call where the virtual call was one indirect call after loading the vptr, and the lookup of the schedule by discipline replaces a load of the cached pointer; no speedup or slowdown is claimed, and a measurement belongs to the performance workflow.
- Not exercised: a second discipline, so `unpark_preempt()`'s new comparison and the empty-reference path of Deadline have run only in the assertions of reading.

**Deliberately not done**
- `fair.hpp` and `fair.cpp` are not touched. They did not build before (`Runtime`, `EnqueueControl`), and the list of what bringing Fair back needs is in `.agents/tasks/sched-facade/DESIGN.md` for the author.
- `Own`, the owning type, which the scheduler does not need.
- `update_thread` and `on_thread_dead` have no caller (`on_thread_dead` is a commented-out line); they stay as optional methods, since the empty hooks are scaffolding.
- No Deadline discipline. Its slot is an enumerator and an empty reference.

**How it was found**
By the design discussion of 2026-11-03: the user found the subsystem rough and did not want `virtual`; the comments of `schedule.hpp` and 260918 supplied the list of what the registry cost.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does the thread-safety analysis, which checked `TA_REQUIRES(rq.lock())` on the virtual declaration, say nothing about a call through a `Ref`, and which function now carries the check?
- What did `x_schedule > y_schedule` compare, and why did it decide nothing while only one discipline was implemented?
- Why is an empty `Ref` for Deadline safe to skip in `pick_next_thread()` and not safe to call?

## Title: Fair is renamed onto the new framework, mechanically, and no further (2026-11-03)

**Files**
- `kernel/include/ours/task/sched/fair.hpp` — includes `sched_entity.hpp`; `Context` is `SchedEntity`; the class `RunQueue` is `FairRunQueue` (the name `RunQueue` is now the per-CPU object); `runtime()` is `exec_runtime()`
- `kernel/kernel/task/sched/fair.cpp` — `Runtime`, `Context`, `EnqueueControl`, `DequeueControl`, `UnparkControl` and `scheduling_context()` renamed to `RunQueue`, `SchedEntity`, `EnqueueOptions`, `DequeueOptions`, `UnparkOptions` and `scheduling_entity()`; `m_params.*` renamed to `m_fair.*`, `m_exec_runtime` and `m_prev_exec_runtime`; `runtime.curr_thread` and `runtime.which_cpu` to `current_thread()` and `which_cpu()`; `fair_run_queue()` replaced by `runtime.queue<Discipline::Normal>()` in fourteen functions; `Fair` has no base class and no `override`; the explicit instantiation of the registrar is gone; `FAIR_SCHEDULE` and `fair_schedule()` added
- `kernel/kernel/task/sched/scheduler.cpp`, `scheduler.hpp` — `normal_schedule()` chooses the object of `Discipline::Normal` (`NORMAL_SCHEDULE` of `RoundRobin`, or `fair_schedule()` where `SCHED_NORMAL_DISCIPLINE_FAIR` is on); `fair_schedule()` is declared under that switch
- `kernel/include/ours/task/sched/run_queue.hpp` — the comment that said `FairRunQueue` was not defined yet
- `.agents/tasks/sched-facade/DESIGN.md`, `docs/design/260918/kernel-task-sched.md` — the adaptation list, now what is done and what is left
- `docs/repairs/261103/kernel-task-sched.md` — what is left, six items, each for the author

**Symptom**
None observable: Fair is still not built (`SCHED_NORMAL_DISCIPLINE_FAIR` is off by default), so the images, the boots and the tests are the ones of the entry above. With the switch on, `fair.cpp` still does not compile: 23 diagnostics in it and 3 in `scheduler.cpp`.

**Cause**
**Fair was written against names the framework no longer has**, and the previous entry left it alone on purpose. The user then asked for the adaptation, and chose the narrowest scope there was: rename what the compiler can name, and change no behaviour (2026-11-03: "Fair adaptation", then "mechanical renames, no semantics"). `kernel-design` lists `fair.*` as hands-off, "including mechanical renames"; this change was made on that explicit request, and the rule is left as written.

Two results of the renaming are worth knowing before reading the diff.

1. **`deadline()` is the one rename that would have changed what Fair does, so it was not made.** `fair.hpp` orders its tree by `x.deadline() < y.deadline()` and `Fair::update_thread()` tests `ctx.deadline() <= ctx.exec_runtime()`. The framework's `SchedEntity::deadline()` returned a field that nothing writes (so every thread was ordered by zero, and the test was always true), and it was replaced by `virtual_deadline()`, which returns `vdeadline`. The obvious rename compiles, and then orders the tree by the virtual deadline (a real order, where there was none) and compares a virtual-time value with real execution time (`SchedTime` and `SchedDuration` are one type, so the compiler says nothing). That is EEVDF's semantics being decided by a find-and-replace. It is the first item of `docs/repairs/261103/kernel-task-sched.md`, and the cause of nearly all of the diagnostics above: `Lesser` is in the definition of `FairRunQueue`, so the class is ill-formed and every use of it fails with it.
2. **The diagnostic counts hide a missing member.** With `deadline()` renamed in a scratch copy, five diagnostics remain: `FairRunQueue::for_each` (called by `RunQueue::for_each_thread()`, and absent), three helpers that still call `Scheduler::Current::get().fair_run_queue()` (`is_object_eligible()`, `update_vlag()`, `set_next_object()`, which have no `RunQueue` parameter to take the queue from), and the `arm_preempt_timer` block of `set_next_thread()`. The 23 and the 3 do not include `for_each`, because the compiler does not report a missing member of a class it has already rejected.

**Change**
The renames, as a table:

| Fair wrote | Now |
|---|---|
| `Context`, `scheduling_context()` | `SchedEntity`, `scheduling_entity()` |
| `Runtime &`, `runtime.curr_thread`, `runtime.which_cpu` | `RunQueue &`, `runtime.current_thread()`, `runtime.which_cpu()` |
| `Scheduler::Current::get().fair_run_queue()` | `runtime.queue<Discipline::Normal>()` |
| `EnqueueControl`, `DequeueControl`, `UnparkControl` | `EnqueueOptions`, `DequeueOptions`, `UnparkOptions` |
| `m_params.runtime`, `.prev_runtime` | `m_exec_runtime`, `m_prev_exec_runtime` |
| `m_params.vruntime`, `.vdeadline`, `.vlag` | `m_fair.vruntime`, `.vdeadline`, `.vlag` |
| `class Fair: public Schedule<Fair, Normal>` with `override`s, and `template class Schedule<Fair, Normal>;` | `class Fair final`, plain members, `constinit Fair FAIR_SCHEDULE;` |

The reference to the object is made in `fair.cpp`, not in `scheduler.cpp`, because `Fair` is defined in the `.cpp`: `constinit Fair` needs the complete class, and `scheduler.cpp` only sees the declaration `fair_schedule()`.

```cpp
// scheduler.cpp, with SCHED_NORMAL_DISCIPLINE_FAIR on
auto normal_schedule() -> ustl::Ref<Schedule> {
    return fair_schedule();
}

// fair.cpp
auto fair_schedule() -> ustl::Ref<Schedule> {
    return ustl::Ref<Schedule>{FAIR_SCHEDULE};
}
```

Kept as they were: `x.deadline() < y.deadline()`, `ctx.deadline() <= ctx.exec_runtime()`, the three helpers' calls of `fair_run_queue()`, `for_each` absent, the `arm_preempt_timer` block. None of them has a rename that is only a rename.

Found by three adversarial reviewers, and not changed (all are in `docs/repairs/261103/kernel-task-sched.md`, or `docs/repairs/260917/kernel-task-sched.md` for the second):
- `on_thread_tick` has no caller, so Fair's tick-driven charging never runs; `UpdateThread`, `OverrideThreadBaseProfile` and `MigrateThread` have no call site either, and the `OnThreadDead` call is commented out
- `unpark_preempt` takes its two `Thread *` in the reverse of the interface's order
- `on_thread_tick` calls `add_pending()` with its default, which names the executing CPU, not the run queue's
- `FairRunQueue::m_num_queued` drifts by one on every park of a running thread
- an enqueue into an empty tree divides by a zero weight sum, and `Fixed::operator/` saturates, so the first thread is given maximal `vruntime` and `vdeadline`

The last two were read off the code, not run.

Rejected: renaming `deadline()` to `virtual_deadline()` "because it is obviously what was meant" (item 1 above: it may be, and it is the author's to say), and deleting the `arm_preempt_timer` block, which computes a zero consumed slice and so looks dead, for the same reason.

**Reference**
None: this project's own code. For the unchanged semantics, the entry above and `docs/design/260918/kernel-task-sched.md`.

**Verification**
- Built and tested in a clean worktree (`.worktrees/fair-adapt`, HEAD `bcc31b8e` plus this diff). The switch is off in that build, so what it shows is that nothing else moved.
- `python3 .agents/skills/verify/scripts/boot_check.py`: dbg PASS, 46 passed, 5 skipped, 0 failed; rel PASS, 44 passed, 8 skipped, 0 failed, 7/7 required; host-tests PASS, 164 passed. The counts are those of the entry above.
- `preempt.test`, `threadjoin.test` and `hotplug.test` on the dbg image with four CPUs: all ok.
- Fair itself was checked only by compiling `fair.cpp` and `scheduler.cpp` as syntax with the kernel's flags from `compile_commands.json` and `SCHED_NORMAL_DISCIPLINE_FAIR=true`: 23 and 3 diagnostics, and 5 with `deadline()` renamed in a scratch copy. Fair was not linked, booted or run.
- `scripts/format_cpp.py --check` is clean on the five C++ files. `clang-tidy` was not run on `fair.cpp`: it is not in the compile database, since the switch is off.

**Deliberately not done**
- The switch is not turned on, and Fair is not built, linked or booted.
- `deadline()`, `for_each`, the three helpers, the `arm_preempt_timer` block and the tick: the author's, in the repair file.
- `kernel-design`'s "Hands off" rule is not changed. If the exception should become the rule, that is the user's to say.

**How it was found**
By the user's request after the scheduler seam was replaced: the previous entry's list of what bringing Fair back needs, applied up to the point where a rename stops being only a rename.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does `deadline()` renamed to `virtual_deadline()` compile, and what does Fair then do that it did not before?
- Why are there 23 diagnostics, then 5, and why is `for_each` in neither count?
- Why is `fair_schedule()` defined in `fair.cpp` and only declared in `scheduler.hpp`?

## Title: Fair's run-queue order and deadline test read the virtual clock (2026-11-03)

**Files**
- `kernel/include/ours/task/sched/fair.hpp` — `Lesser` orders `FairRunQueue` by `virtual_deadline()`
- `kernel/kernel/task/sched/fair.cpp` — `Fair::update_thread()` tests `m_fair.vruntime >= virtual_deadline()`
- `docs/repairs/261103/kernel-task-sched.md` — the `deadline()` item is deleted, and what the check of it found is added: twelve items in all
- `.agents/tasks/sched-facade/DESIGN.md` — the Fair bullet

**Symptom**
None observable: Fair is still not built. With `SCHED_NORMAL_DISCIPLINE_FAIR` on, the syntax check of `fair.cpp` goes from 23 diagnostics to 5 and that of `scheduler.cpp` from 3 to 1, and none of the five distinct ones that remain mentions `deadline` (they are `FairRunQueue::for_each`, three calls of `fair_run_queue()` and `arm_preempt_timer`, items 4, 5 and 12 of the repair file; `for_each` is the one in `scheduler.cpp` too).

**Cause**
**The two lines that called `deadline()` could not be renamed, because they sat on the border between two clocks and the name only said which accessor.** A thread has three numbers in Fair: `exec_runtime` (real time on a CPU, from 0), `vruntime` (virtual time: each charge adds `calc_weighted_time(elapsed, weight)`, and its starting value comes from the queue's average) and `vdeadline` (a reading on the `vruntime` clock: some `vruntime` plus one request, or more after a yield). The framework's `deadline()` returned a field nothing wrote, and was replaced by `virtual_deadline()`, which returns `vdeadline` (2ed56f1b). The preemption test was `deadline() <= exec_runtime()`, so the obvious rename, `virtual_deadline() <= exec_runtime()`, compares a reading on one clock with a reading on the other. Its outcome is then decided by the offset between the clocks, not by whether the request is spent. Read off the code, with the constants recomputed:

| Situation | `virtual_deadline() <= exec_runtime()` |
|---|---|
| The offset `vruntime - exec_runtime` is zero | fires after one (weighted) request, by accident |
| A peer that has run 10 ms is queued when T, of `MID_WEIGHT`, is placed | T's `vdeadline` is about 274 ms, so it fires after about 274 ms of execution, not 64 ms (at the default priority 15: about 262 ms, against a weighted request of 52 ms) |
| T is alone in an empty tree | `vruntime` and `vdeadline` saturate at the maximum (item 6 of the repair file), so it never fires |
| The offset is at most minus one request | fires on every charge |

The author chose, out of three readings, the one in which both operands are on the virtual clock. `vdeadline` is built as `vruntime` plus a request (a yield adds one more), so whatever `vruntime` started from cancels in `vruntime >= vdeadline`, and the test holds when about one request of real time has been charged since `vdeadline` was last set (about, because the weight scaling rounds). The exception is a start that saturates (item 6 of the repair file): `vruntime` and `vdeadline` are then both the maximum and the test holds on every charge. The weight cancels too, while a thread's weight stays constant, because the same factor scales the charge and the request.

**Change**
```cpp
// fair.hpp, FairRunQueue
return x.virtual_deadline() < y.virtual_deadline();   // was x.deadline() < y.deadline()

// fair.cpp, Fair::update_thread()
if (ctx.m_fair.vruntime >= ctx.virtual_deadline()) {  // was ctx.deadline() <= ctx.exec_runtime()
```

The ordering needed no choice: `virtual_deadline()` is what WaitQueue already orders by, and the comment on `evaluate_next_thread()` says "the earliest virtual deadline". The choice was the test. Nothing delivers a tick (item 8 of the repair file), so at the end of a slice the test is reached in `put_prev_thread()`, after the pick: `mark_preempt_pending()` is redundant there, and the replenish is what it does. Were a tick delivered, `on_thread_tick()` would reach it first, and `mark_preempt_pending()` would be what asks for the reschedule.

Rejected: `remaining_slice() <= 0`, which does not depend on the clock but is not what the algorithm states, and after a replenish does not restart the slice (read, not checked); and the real clock, which means rewriting the three `vdeadline = vruntime + ...` writers onto `exec_runtime`, after which `vdeadline` is not virtual and a weight only scales the request, so shares are no longer proportional.

**Reference**
Linux EEVDF, `update_deadline()` in `kernel/sched/fair.c`: it returns while `vruntime` is before the deadline, and otherwise sets `deadline = vruntime + calc_delta_fair(slice)` (and asks for a reschedule when more than one task is runnable: from memory, not checked). A reviewer read it in the first 100 kB of upstream master on 2026-11-03. That the tree is ordered by the deadline is from memory, not checked.

**Verification**
- Four independent read-only checks, each asked to refute one claim, all with file and line: the mixed clocks (partly: the lone thread never fires, which I had not said), the consistency of the new test (partly), the direction of the weight (holds) and what else uses the real clock (partly). Corrections taken in: the test is not exact to the nanosecond; `yield_thread()` writes `vdeadline +=`, not `vruntime + request`; and a pick restarts the request (item 9).
- `python3 .agents/skills/verify/scripts/boot_check.py --out out/fair-lane`: build ok, dbg PASS (46 passed, 5 skipped, 0 failed), rel PASS (44 passed, 8 skipped, 0 failed, 7/7 required), host-tests PASS (164 passed). Fair is not in these images, so this shows only that nothing else moved.
- `-fsyntax-only` on `fair.cpp` and `scheduler.cpp` with the kernel's flags and the Fair switch on: 5 and 1 diagnostics, listed above. Fair was not linked, booted or run, and the numbers in the table and in the repair file are arithmetic on the constants, not measurements.
- `scripts/format_cpp.py --check` is clean on both files. `clang-tidy` was not run: `fair.cpp` is not in the compile database.

**Deliberately not done**
- The four sites that read `exec_runtime()` where `vruntime` belongs, the direction of the weight and the scale of the average: items 1 to 3 of the repair file. This decision makes them mandatory before Fair is enabled; it does not make them part of this change.
- `for_each`, the three helpers, the `arm_preempt_timer` block, the tick, and the rest of the repair file.

**How it was found**
By the first item of the previous entry. The author asked for it first, and chose the reading from three options set out with the check's findings.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does `vruntime >= vdeadline` not depend on where `vruntime` started, and `vdeadline <= exec_runtime` does?
- Why is this test unchanged by the wrong direction of the weight (item 1) while the order of the tree is not?
- After `set_next_object()` rewrites `vdeadline` at every pick, which events can make the test fire, and which cannot?

## Title: Fair's weights run the right way and its weighted average is in the right units (2026-11-03)

**Files**
- `kernel/include/ours/task/sched/common.hpp` — `to_virtual_time()` and `weighted_offset()`, and the `static_assert`s that pin them
- `kernel/kernel/task/sched/fair.cpp` — the anonymous `calc_weighted_time()` is gone; its five call sites use `to_virtual_time()`; `FairRunQueue::term_of()` uses `weighted_offset()`
- `kernel/include/ours/task/sched/fair.hpp` — `FairRunQueue::calc_weighted_time()` is `term_of()`
- `docs/repairs/261103/kernel-task-sched.md` — items 1 and 2 are deleted (the numbers are kept, the change records cite them), items 13 to 15 are added
- `docs/repairs/260903/scheduler.md` — the bullet about `calc_weighted_time()`'s integer division, a defect `ffl::Fixed` removed on 2026-09-17 (`docs/changes/260917/ffl.md`), is deleted: it named a function that is gone
- `.agents/tasks/sched-facade/DESIGN.md` — the Fair bullet

**Symptom**
None observable: Fair is still not built. By hand, from the constants: a thread of priority 31 (weight 1.0) and a default thread (priority 15, weight 0.0389) that both always want to run were given the CPU in the ratio 1 : 25.7, the heavy one getting 3.7 %. And two `MID_WEIGHT` threads whose virtual times stand 100 ms and 0 ms from the minimum had an average of about 1050 ms instead of 50 ms, which made both eligible.

**Cause**
**One function did two jobs that run in opposite directions.** The old `calc_weighted_time(duration, weight)` returned `duration * (weight / MID_WEIGHT)`.

As a clock it is backwards. The discipline hands out equal virtual time, so a thread is run until its virtual clock catches up with the others'. Equal clocks mean service in proportion to weight only if a heavier thread's clock runs *slower*: virtual time is real time divided by weight. Multiplied, the heaviest thread's clock ran 21 times faster than a `MID_WEIGHT` thread's, so it received about a twenty-sixth of a default thread's CPU, not twenty-six times as much. `vruntime`, the request that builds `vdeadline`, and the order of the tree all took the wrong direction. The deadline test chosen earlier today was not hurt, because both of its sides carry the same factor while a weight stays constant.

As a term of the average it has the wrong scale. The mean that decides eligibility is `sum(weight * offset) / sum(weight)`, where `offset` is how far a thread's virtual time stands from the minimum; the author's comment above `is_object_eligible()` derives it (`IdealServiceVirtualTime = WeightedOwnedVirtualTimeSum / TotalWeight`, with `Lag(i) = Weight(i) * (IdealServiceVirtualTime - OwnedServiceVirtualTime(i))`). The numerator's unit is weight times time; dividing it by the sum of the weights gives a time. The old helper made the term `offset * weight / MID_WEIGHT`, and the extra `/ MID_WEIGHT` made the numerator 21 times too large, so the mean was too, and `offset * weight_sum <= total` held for nearly every thread. A discipline in which everyone is eligible picks by earliest deadline alone: the part of EEVDF that makes a thread that has had more than its share wait was gone.

The two uses were different quantities that happened to share a name, so each had to be right for the other to be wrong.

**Change**
The helper is split, in `common.hpp` beside `priority_to_weight()`, where a `static_assert` can reach it in every build:

```cpp
// before, fair.cpp
auto calc_weighted_time(SchedDuration duration, Weight weight) -> SchedDuration {
    if (weight == MID_WEIGHT) { return duration; }
    auto const rate = weight / MID_WEIGHT;
    return duration * rate;
}

// after, common.hpp
constexpr auto to_virtual_time(SchedDuration const real, Weight const weight) -> SchedDuration {
    DEBUG_ASSERT(weight > Weight{0});
    if (weight == MID_WEIGHT) { return real; }
    return real / (weight / MID_WEIGHT);
}
constexpr auto weighted_offset(SchedDuration const offset, Weight const weight) -> SchedDuration {
    return offset * weight;
}
```

| Site | Is | Now |
|---|---|---|
| `Fair::update_thread()`: the charge, and the replenish | real to virtual | `to_virtual_time()` |
| `Fair::adjust_vlag()`, `Fair::set_next_object()`, `Fair::yield_thread()`: the request | real to virtual | `to_virtual_time()` |
| `FairRunQueue::enqueue_object()`, `dequeue_object()`, `avg_vruntime()` | a term of the sum | `term_of()`, which calls `weighted_offset()` |

The names: the first version called the second function `weighted_key`, and a reviewer pointed out that "key" already means the ordering key in this module (`SchedEntity::virtual_deadline()`, `WaitQueue::QueueKey`), and that a member of the same name would hide the free function. The member is `term_of()` and the function says what it multiplies, an offset from the minimum.

The anchor stays `MID_WEIGHT`, the weight whose clock reads real time, which is what the code already assumed. Preparing the decision showed that it is not the default: `DEFAULT_PRIORITY` is `HIGHEST_PRIORITY >> 1` = 15, `MID_WEIGHT` is `PRIORITY_TO_WEIGHT_MAP[NUM_PRIORITIES / 2]` = priority 16 (Zircon's `DEFAULT_PRIORITY` is `NUM_PRIORITIES / 2`, which is 16). A default thread's weight is `MID_WEIGHT / 1.225`, so its clock runs 1.225 times as fast as the wall clock, and the fast path `weight == MID_WEIGHT` is taken only by a priority-16 thread, while the spawn sites pass `DEFAULT_PRIORITY` and the idle threads `IDLE_PRIORITY` (7): every call pays the division. The author kept `MID_WEIGHT`. The anchor changes no share, no order and no eligibility, because every clock is scaled by the same constant; it changes the magnitude of `vruntime`, the range, the rounding and how readable a `vruntime` is. Rejected: a new `DEFAULT_WEIGHT` anchor (priority 15), which makes a default thread's `vruntime` read wall time, and `MAX_WEIGHT`, which makes a priority-0 clock run 540 times fast and saturate an `i64` of nanoseconds after about 200 days of CPU. Under the chosen anchor a priority-0 clock runs about 26 times as fast as the wall clock.

The form of the division was chosen between three. `real * MID_WEIGHT / weight` rounds `real * MID_WEIGHT` to a nanosecond and then divides by a weight as small as 0.00185, so the half nanosecond comes back as about 270 ns. A single exact expression in `__int128` needs raw access and a hand-written overflow rule that `ffl::Fixed` already has. `real / (weight / MID_WEIGHT)` is two library operators; its errors are the rate kept in sixteen fractional bits, at worst 1/2539 of itself (a relative 0.04 % at priority 0, 0.039 % computed below, against 0.34 % that the table's own rounding already puts in the raw value 121, whose exact value is 121.4), and the truncation of the quotient, under a nanosecond and always downward. It is the simplest and was taken; what it costs per call is item 14.

`weighted_offset()` stays a function of its own, though it is one multiplication, because it carries the explanation that it multiplies where its neighbour divides, and the `static_assert` of its units.

The argument of `FairRunQueue::term_of(object)`, `exec_runtime() - min_vruntime_`, is unchanged; a comment there says so. It is one of the four sites of item 3 and is changed with it.

The `static_assert`s at the end of `common.hpp`:
- `MID_WEIGHT`'s clock reads real time.
- At every priority, and for 1 ms, 1000 s and 11.6 days of real time, virtual time times weight equals `real * MID_WEIGHT` within 1/2048 and a nanosecond (the worst deviation is 1/2507, at priority 0). Multiplying instead of dividing fails this by the square of `weight / MID_WEIGHT`; a conversion that narrows its intermediate to 64 bits fails the long ones.
- The heaviest thread's clock runs about 21 times slower than `MID_WEIGHT`'s (asserted between 20 and 22.2), the lightest's about 26 times faster (between 25 and 26), and a default thread's about 1.225 times as fast (between 1.2 and 1.25).
- A term is rounded to the nearest nanosecond: half of 3 is 2.
- The two-thread example: offsets 100 ms and 0 average to 50 ms within 16 ns, the one behind the average is eligible and the other is not.

**Reference**
Linux EEVDF: `calc_delta_fair()` (virtual time is real time times `NICE_0_LOAD / weight`), and `sum_w_vruntime_add()`, `avg_vruntime()` and `vruntime_eligible()` (`sum_w_vruntime` is the sum of `key * weight`, a thread is eligible when `avg >= key * load`). A reviewer confirmed the function names in a local Linux checkout; the bodies are from memory, not checked. Zircon has none of this: `scheduler.cc`, `scheduler.h` and `scheduler_state.h` contain neither `vruntime` nor `virtual_time`, and its fair threads carry a start time and a finish time on a timeline that is rescaled as a whole (`fair_affine_transform_`); that is from a search and an outline reading, not a full reading. Only the numerics (`ffl::Fixed`, the weight table) are shared with it.

**Verification**
- `python3 .agents/skills/verify/scripts/boot_check.py --out out/fair-lane`, run on the final code: build ok (30.7 s, a full rebuild, because `common.hpp` is included almost everywhere and so every translation unit evaluated the `static_assert`s), dbg PASS (46 passed, 5 skipped, 0 failed), rel PASS (44 passed, 8 skipped, 0 failed, 7/7 required), host-tests PASS (164 passed). Fair is not in these images, so the boots show that nothing else moved; the `static_assert`s are the check of the change.
- Five independent read-only reviewers: 63 mutants of the `static_assert`s (the old multiply, a wrong anchor, the old term and so on; every behavioural one was killed, and four gaps were found and closed: a tolerance 2.5 times too loose, no check of long durations, of the default priority or of the rounding), a Python model of `ffl::Fixed` checked against the compiler on 458 values (0 mismatches), the substitution at each call site (all nine were right), style and lint, and a fact-check of these documents. They found a comment that said the opposite of its assert, the name clash above, and a dozen wrong or loose statements in the first draft of these documents (a count of 128-bit divisions, "wraps" for a saturating type, "measured" for computed figures); all were corrected.
- The error of the conversion, recomputed in Python from the table generator's recurrence for 1 ms at each priority: 0.039 % at priority 0 (25,811,736 ns against an exact 25,801,653), under 0.008 % from priority 5 up, under 0.0025 % from priority 10 up, 0 at `MID_WEIGHT`.
- `-fsyntax-only` on `fair.cpp` and `scheduler.cpp` with the Fair switch on: still 5 and 1 diagnostics, the same ones (items 4, 5 and 12 of the repair file).
- `scripts/format_cpp.py` and `scripts/lint_docs.py` are clean. `clang-tidy -p out/fair-lane` on `rr.cpp`, which includes `common.hpp`, with `common.hpp` and `fair.hpp` in the header filter: no diagnostic in either. `fair.cpp` is not in the compile database and was not run through it.
- Fair was not linked, booted or run. The shares in the Symptom are arithmetic on the constants, not measurements; no test shows them (item 13).

**Deliberately not done**
- The four sites that read `exec_runtime()` where `vruntime` belongs: item 3, and mandatory before Fair is enabled.
- A `DEFAULT_WEIGHT` anchor or a change of `DEFAULT_PRIORITY` (above): the author kept `MID_WEIGHT`, and moving either is a change of numbers, not of behaviour.
- A faster conversion (item 14), a sum with fractional bits (item 15), and the test that shares follow the weights (item 13).

**How it was found**
By the review of 2026-11-03 that produced the repair file, and by reading the two uses of the helper side by side: the same function appeared under `vruntime +=` and under `total_vruntime_ +=`, and only one direction can be right for each. The anchor mismatch was found while preparing the decision: a search for the uses of `DEFAULT_PRIORITY` and `MID_WEIGHT`, and the table recomputed in Python. The author asked for this fix and chose the anchor; the "Hands off" rule for `fair.*` in `kernel-design` is unchanged.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why must a heavier thread's virtual clock run slower, and what does an EEVDF pick do with two clocks that read the same?
- What unit does `total_vruntime_` have, and why does dividing the term by `MID_WEIGHT` make the mean 21 times too large whatever the weights are?
- A default thread has priority 15 and the anchor is priority 16. What does that do to its `vruntime`, and why does it leave its share against a priority-17 thread unchanged?
