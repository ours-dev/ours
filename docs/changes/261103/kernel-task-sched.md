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

## Title: Fair's average, lag and eligibility read the virtual clock, not real time (2026-11-03)

**Files**
- `kernel/include/ours/task/sched/sched_entity.hpp` — new `SchedEntity::vruntime()`, beside `virtual_deadline()`; the comments of `exec_runtime()` and of `FairState` are brought up to date
- `kernel/include/ours/task/sched/fair.hpp` — `FairRunQueue::lag_of()` reads `vruntime()`
- `kernel/kernel/task/sched/fair.cpp` — `FairRunQueue::term_of()`, `FairRunQueue::update_min_vruntime()` and `Fair::is_object_eligible()` read `vruntime()`; the comment over `term_of()` that pointed at this item is gone
- `docs/repairs/261103/kernel-task-sched.md` — item 3 is deleted (the number is kept), the references to it are cut, items 16 and 17 are added and item 6 gets one more consequence
- `.agents/tasks/sched-facade/DESIGN.md` — the Fair bullet

**Symptom**
Latent, cannot be triggered today: Fair is not built (items 4, 5 and 12 of the repair file). Once it is, and the queue's weighted mean covers every runnable thread, the running one included (item 6), the algebra is this. A priority-31 thread A and a default thread B (weights 1.0 and 0.0389) both always want to run. A is eligible when its offset is at most the weighted mean, and `wA*oA + wB*oB >= oA*(wA+wB)` reduces to `oB >= oA`; B's test reduces to `oA >= oB`. The weights cancel. On the real clock that keeps the pair within about one request of equal *real* time, 1 : 1, whatever the priorities; on the virtual clock, within about one request of equal *virtual* time, which is 25.7 : 1 in real time. With more than two threads the weights do not cancel in each test, but the stationary point is the same: nobody ahead, all clocks equal.

In the code as it is the mean at a pick covers the waiting threads only: `current_` is never written, and `pick_next_thread()` runs before `put_prev_thread()` (`Scheduler::evaluate_next_thread()`), so the running thread is off the tree. A pair then alternates 1 : 1 on either clock, and this change alone does not make shares follow weights. It is necessary for that and not sufficient (items 6 and 16).

**Cause**
**The run queue measured fairness on a clock that is not the one the weights act on.** A thread carries two numbers. `exec_runtime` is real time on a CPU, from zero, not scaled. `vruntime` is virtual time: real time divided by the weight in units of `MID_WEIGHT` (`to_virtual_time()`), starting from the queue's average less the lag the thread carried (`Fair::adjust_vlag()`). Four places that mean the virtual quantity read the first: `lag_of()` (`avg_vruntime() - exec_runtime()`), `term_of()` (a thread's term of the weighted sum), `update_min_vruntime()` (`exec_runtime() < min_vruntime_`) and `is_object_eligible()`. The comment on `exec_runtime()` had pointed at the pattern, with two of the expressions; the review of 2026-11-03 found all four.

Eligibility gates `Fair::evaluate_next_thread()`: only an eligible thread can be chosen there, whatever its deadline. Before this change the weight reached the deadline and the request only through `to_virtual_time()` (the rate `vruntime` advances at and the size of the request), so it decided who won among the eligible threads. It entered the mean too, but a mean of real times has equal real time as its stationary point whatever the weights, so it could not change the long-run split. On the virtual clock it now acts on eligibility as well.

It also placed threads on the wrong origin. `update_vlag()` stored `avg_vruntime() - exec_runtime()`, and `avg_vruntime()` was a mean of `exec_runtime` values, so `adjust_vlag()` computed `vruntime = avg_vruntime() - vlag` entirely on the real clock and stored the result in `vruntime`, the variable `update_thread()` advances in virtual time. On an unchanged average a re-placed thread got `vruntime = exec_runtime`, while a priority-31 thread's `vruntime` had been accruing 21 times slower than real time (a default thread's, 1.225 times faster), so one dequeue and enqueue took the first one's virtual clock to about 21 times its value.

**Change**
Four reads, one accessor (abbreviated):

```cpp
// sched_entity.hpp
[[nodiscard]] auto vruntime() const -> SchedDuration { return m_fair.vruntime; }

// fair.hpp, lag_of()
return avg_vruntime() - object.exec_runtime();                 // was
return avg_vruntime() - object.vruntime();                     // now

// fair.cpp, FairRunQueue::term_of()
weighted_offset(object.exec_runtime() - min_vruntime_, ...)    // was
weighted_offset(object.vruntime() - min_vruntime_, ...)        // now

// fair.cpp, FairRunQueue::update_min_vruntime()
if (current.exec_runtime() < min_vruntime_) { min_vruntime = current.exec_runtime(); }   // was
if (current.vruntime() < min_vruntime_) { min_vruntime = current.vruntime(); }           // now

// fair.cpp, Fair::is_object_eligible()
(object.exec_runtime() - rq.min_vruntime()) * weight_sum       // was
(object.vruntime() - rq.min_vruntime()) * weight_sum           // now
```

The real-time uses stay: the charge in `update_thread()`, `m_prev_exec_runtime` in `set_next_object()`, `remaining_slice()` and the NOHZ block (item 12) use `m_exec_runtime` directly. After this nothing calls `exec_runtime()`; it is kept, with its comment brought up to date, not deleted.

`FairRunQueue` is not a friend of `SchedEntity`, so three of the four sites needed a way to read the field; `is_object_eligible()` is a member of `Fair`, which is a friend, and uses the accessor so that all four read alike. Chosen: a read-only accessor, the way `virtual_deadline()` is one. Rejected: `friend class FairRunQueue`, which would open every private member of `SchedEntity` (the profile, the slice, the queue hooks, `m_exec_runtime`) to a class that needs one number. The choice was the implementer's; it was not put to the author.

The queue keeps `total_vruntime_` and `weight_sum_` equal to the sums of `term_of()` and `weight()` over the threads in the tree, at the current `min_vruntime_`. So a thread's `vruntime` and weight must not change while it is in the tree, and `min_vruntime_` may move only through `update_min_vruntime()`, which re-bases the sum (item 15 on the rounding). On every path called today that holds: `update_thread()` charges the thread it is given, which is the running thread, off the tree since `set_next_object()`, and `adjust_vlag()` runs on a thread that is not in the tree. Exceptions, read and not run: `override_thread_base_profile()` charges a queued thread before taking it off the tree (item 11, no caller); `adjust_vlag()` is the one writer `exec_runtime` did not have, and a delayed dequeue (`SCHED_DELAY_DEQUEUE`, off) leaves the thread in the tree, where a later wake would run it; tree membership is taken from `is_active()` throughout (item 17).

**Reference**
Linux EEVDF keeps one clock, `se->vruntime`, read through `entity_key()` by `avg_vruntime()`, `sum_w_vruntime_add()`, `vruntime_eligible()` and `update_zero_vruntime()` (`kernel/sched/fair.c` in the local checkout `~/Workspaces/projects/linux`, v7.2-rc5; the names were confirmed there by a reviewer). Zircon has no per-thread virtual runtime: a search of `scheduler.cc`, `scheduler.h` and `scheduler_state.h` and of `zircon/kernel` finds neither `vruntime` nor `virtual_time`.

**Verification**
- `python3 .agents/skills/verify/scripts/boot_check.py --out out/fair-lane`, run on the final code: build ok (51.3 s, a full rebuild, because `sched_entity.hpp` is included almost everywhere), dbg PASS (46 passed, 5 skipped, 0 failed), rel PASS (44 passed, 8 skipped, 0 failed, 7/7 required), host-tests PASS (166 passed; two more than the previous entry, from commits other sessions merged into the branch in between). Fair is not in these images, so the boots show only that nothing else moved.
- `-fsyntax-only` on `fair.cpp` and `scheduler.cpp` with the Fair switch on: still 5 and 1 diagnostics, the same ones (items 4, 5 and 12 of the repair file); a search finds no call of `exec_runtime()` in `kernel/` or `system/`.
- `scripts/format_cpp.py` and `scripts/lint_docs.py` are clean. `clang-tidy -p out/fair-lane` on `rr.cpp` reports nothing on the changed lines of `sched_entity.hpp`, nor in `fair.hpp` or `common.hpp` (older diagnostics in `sched_entity.hpp` remain).
- Three independent read-only reviewers (the eligibility algebra and the tree invariant, with a rational-arithmetic model of the queue; the C++ diff; a fact-check of these documents). They showed that the first draft described an idealised queue as if it were the code, and found items 16 and 17, a wrong placement account, a wrong invariant paragraph and a dozen smaller errors; all were corrected.
- Fair was not linked, booted or run. The algebra in the Symptom is on paper and a model, and no test shows it (item 13).

**Deliberately not done**
- Fair still cannot work: `min_vruntime_` never advances, `current_` is never written and the first thread of an empty queue is placed on a saturated average (item 6), and the eligibility test is not ready for a running thread in the average (item 16). A consequence of this change until item 6 is fixed: the first thread's term is now about `weight * MAX` where it was 0, so the average is about the maximum while that thread is in the tree and a thread enqueued meanwhile is placed near it. It is recorded in item 6.
- `exec_runtime()` is not deleted, and the real-time uses are not moved onto it.

**How it was found**
By the review of 2026-11-03 that produced the repair file, and from the comment on `exec_runtime()`. The decision of the same day to compare `vruntime` with `vdeadline` made the item mandatory. The author asked for it; the "Hands off" rule for `fair.*` in `kernel-design` is unchanged. That the weights cancel when the offsets are real times is the algebra above, worked out for this entry and checked by a reviewer's model.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why do the weights cancel in the eligibility test when the offsets are real times, and what does the test keep equal on the virtual clock?
- Why did placing a thread on `avg_vruntime() - vlag` put a priority-31 thread about 21 times too far along when both were on the real clock?
- Why must a thread's `vruntime` not change while it is in the tree, and which call breaks that today?

## Title: Fair's queue has a zero at the weighted mean, a running thread, and one eligibility test (2026-11-04)

**Files**
- `kernel/include/ours/task/sched/types.hpp` — new `WeightedTime`, `ffl::Fixed<i64, 16>`, the unit of the weighted sum (the same type as `Weight`)
- `kernel/include/ours/task/sched/common.hpp` — `weighted_offset()` returns it, exactly; new `weighted_mean_offset()` (rounded down) and `offset_is_eligible()`; the `static_assert`s follow
- `kernel/include/ours/task/sched/fair.hpp` — `FairRunQueue`: `min_vruntime_` is `zero_vruntime_`; `current_` starts null and has `set_current()` and `clear_current()`; `avg_vruntime()` is no longer const and is defined in `fair.cpp`; new `recenter()`, `is_eligible()` and the private `sums()`; `total_weight()` and `total_vruntime()` forward to `sums()` and so include the running thread, and `total_vruntime()` returns `WeightedTime`
- `kernel/kernel/task/sched/fair.cpp` — the queue's methods; `update_min_vruntime()` became `update_zero_vruntime()` and its TODO is gone; the author's derivation above it now reads `Weight(i) * (... - ZeroVirtualTime)` and `ZeroVirtualTime * TotalWeight`; `Fair::is_object_eligible()` is gone, its derivation comment moved to `is_eligible()`; `update_vlag()` and `set_next_object()` take the `RunQueue` they are handed; `update_thread()` recenters when a request is replenished, and loses its sentence about a saturating start; `put_prev_thread()` charges, then clears `current_`, then enqueues; the `Fair` class comment about the three helpers is gone
- `docs/repairs/261103/kernel-task-sched.md` — items 5, 6, 15 and 16 are deleted (the numbers are kept); items 8, 13 and 17 are extended and item 18 is new
- `.agents/tasks/sched-facade/DESIGN.md` — the Fair bullet

**Symptom**
None observable: Fair is still not built. With the Fair switch on, the syntax check of `fair.cpp` goes from 5 diagnostics to 2 (`for_each` and `arm_preempt_timer`, items 4 and 12), the three `fair_run_queue()` ones being gone. By hand: the first thread of an empty queue was placed on `min_vruntime_ + total / 0`, and `Fixed::operator/` answers its maximum on a zero divisor, so its `vruntime` and `vdeadline` were saturated; and a pair of always-runnable threads alternated 1 : 1 whatever their weights, because the running thread was in neither the mean nor the eligibility test. A model of the finished queue that two reviewers ran gives the shares the weights ask for: 25.69 : 1 for priorities 31 and 15 at the code's 64 ms slice (25.71 expected), and the right split for three threads.

**Cause**
Five defects of one queue, which only make sense together.

1. **The empty queue.** `avg_vruntime()` divided by the weight sum without looking at it. With nothing in the tree, and `current_` unwritten, that is zero, and the answer saturates.
2. **`current_` had no writer.** `total_weight()` and `avg_vruntime()` add the running thread when `current_` is set and active, and it never was, so the mean at a pick covered the waiting threads only. `Scheduler::evaluate_next_thread()` picks before `put_prev_thread()`, so the running thread is off the tree at the pick. A lone waiter is trivially eligible and a pair alternates 1 : 1, on either clock (the previous entry's Symptom).
3. **The eligibility test used two different sums.** `avg_vruntime()` added the running thread's weight and its term; `is_object_eligible()` took `total_vruntime()`, the tree's sum alone, against `total_weight()`, which includes the running thread. With `current_` set the numerator would cover the tree and the denominator the tree and the running thread: for a running priority-31 thread A and a waiting default thread B the test is `wB*oB >= oB*(wB+wA)`, true only for `oB <= 0`. A rational model of the queue that a reviewer ran gave A no further slice after its first one. `term_of()` is private to `FairRunQueue`, so `Fair` could not add the term itself; the pair has to come from one place.
4. **`min_vruntime_` could not advance.** `update_min_vruntime()` took the smaller of the running thread's clock and `min_vruntime_`, so its `delta > 0` test was never true, and it had no caller. Its own TODO says why it was left: the smallest virtual time of the tree has to be searched for. The tree is ordered by virtual deadline, so its leftmost is not the smallest: with A at `v=50, d=60` and B at `v=10, d=90`, the leftmost is A and the smallest `v` is B's.
5. **Three helpers reached for the calling CPU's queue.** `is_object_eligible()`, `update_vlag()` and `set_next_object()` called `Scheduler::Current::get().fair_run_queue()`, which no longer exists, and would have used the wrong queue on a wakeup from another CPU. Writing `current_` and moving the zero both mutate the queue, so they needed the right one.

**Change**
The author chose the zero (a decision card, 2026-11-03): the weighted mean, not the smallest virtual time. Every offset is a distance from the zero, so the sums are valid for any value of it; the mean is already in the two sums, so moving the zero there costs `total -= delta * weight_sum; zero += delta`, with no search. Options set out and not taken: a scan of the tree for the minimum (a new O(n) walk at each placement) and an augmented tree (heavier, a change to `ustl`).

Before (abbreviated):

```cpp
// fair.hpp
auto avg_vruntime() const -> SchedDuration {
    auto total_vruntime = total_vruntime_; auto weight_sum = weight_sum_;
    if (current_ && current_->runqueue_state().is_active()) { weight_sum += current_->weight(); total_vruntime += term_of(*current_); }
    return min_vruntime_ + total_vruntime / weight_sum;                // no guard for weight_sum == 0
}
// fair.cpp
update_min_vruntime(current):  min = (current.vruntime() < min_vruntime_) ? current.vruntime() : min_vruntime_;
                               if (min - min_vruntime_ > 0) { min_vruntime_ = min; total_vruntime_ -= delta * weight_sum_; }   // never true
Fair::is_object_eligible(object):  rq = Scheduler::Current::get().fair_run_queue();
    return rq.total_vruntime() >= (object.vruntime() - rq.min_vruntime()) * rq.total_weight();   // tree sum, full weight
```

After:

```cpp
auto FairRunQueue::sums() const -> Sums {            // the one pair
    auto terms = Sums{total_vruntime_, weight_sum_}; // the tree
    if (current_ && current_->runqueue_state().is_active()) {
        terms.total += term_of(*current_);           // and the running thread
        terms.weight += current_->weight();
    }
    return terms;
}
auto FairRunQueue::update_zero_vruntime(SchedDuration const delta) -> void {
    total_vruntime_ -= weighted_offset(delta, weight_sum_);   // the tree's sum, the tree's weight
    zero_vruntime_ += delta;
}
auto FairRunQueue::recenter() -> void {
    auto const terms = sums();
    if (terms.weight == Weight{0}) { return; }                // an empty queue has no mean
    update_zero_vruntime(weighted_mean_offset(terms.total, terms.weight));
}
auto FairRunQueue::avg_vruntime() -> SchedDuration { recenter(); return zero_vruntime_; }   // an empty queue's mean is its zero
auto FairRunQueue::is_eligible(SchedEntity const &object) const -> bool {
    auto const terms = sums();
    return offset_is_eligible(terms.total, terms.weight, object.vruntime() - zero_vruntime_);
}
```

`avg_vruntime()` and `is_eligible()` both take `sums()`, so the running thread is in both or in neither and the mismatch cannot return. `set_next_object()` calls `rq.set_current(ctx)` after taking the thread off the tree; `put_prev_thread()` charges the thread, then clears `current_`, then enqueues it. The empty queue answers its zero, so the first thread is placed at `zero - 0`, finite, a request ahead of its `vruntime`; a lone running thread is the mean (its own term over its own weight), and its lag is exactly zero.

**The zero has to follow the threads.** A reviewer found that the first version moved the zero only inside `avg_vruntime()`, which only a placement or a lag reaches. A thread that is only ever re-picked reaches neither, and its offset from the zero grows with its clock, 25.8 times real time at priority 0, until `WeightedTime` saturates, about 1.5 h of CPU at priority 0 and 32 h at the default priority; past that the sums stop adding up and the shares of unequal weights collapse in the model. `Fair::update_thread()` now calls `recenter()` where a request is replenished, at O(1): a thread's offset then grows by at most one request between two moves. The limit is 39 h divided by the sum of the weights, since `update_zero_vruntime()` and the eligibility test multiply an offset by it.

**The mean is rounded down.** `weighted_mean_offset()` floors to the nanosecond, so a thread placed at `avg_vruntime()` with no lag is eligible: the test compares against the exact mean, and a mean rounded up could sit half a nanosecond ahead of it. Two threads at 0 and 1 ns have a mean of 0.5; rounding up put the zero at 1 and the newcomer failed its own test.

**The sum had to become exact.** Moving the zero at every placement is a rounding hazard the old `min_vruntime_` never ran into, because that one never moved. Each term was a `SchedDuration`, whole nanoseconds, rounded to nearest, and a move subtracted one rounded product from a sum of others, so the sum drifted from the sum of its terms by up to half a nanosecond per term per move; the division by a small weight sum then multiplied it (repair item 15, now deleted). A term is now a `WeightedTime`, a nanosecond count times the weight's raw value, which is a whole number of its units: no rounding, so add, subtract and move are exact. The cost is the range: a time converts into it up to about 39 hours (2^47 ns) and saturates beyond. `WeightedTime` is the same type as `Weight`; the sum and the weight it divides by can be swapped without a diagnostic.

Three helpers took the `RunQueue` they were handed: `update_vlag(runtime, ctx)`, `set_next_object(runtime, ctx)`, and the eligibility test, which is now `runtime.queue<Discipline::Normal>().is_eligible(...)` at its two callers. That closes repair item 5 without a separate change; it was not separable from writing `current_` and moving the zero on the right queue.

`update_min_vruntime()` was rewritten and renamed, not deleted. The author's derivation of the sum above it is kept with "Minimal" read as "Zero", and corrected: its first line was unweighted and its sum line said `NumTasks` where the sum is over `TotalWeight`. `min_vruntime()` is `zero_vruntime()`. The accessors `total_weight()`, `total_vruntime()` and `zero_vruntime()` have no caller; they are kept.

The `static_assert`s added in `common.hpp`: a term is exact (half of 3 is 1.5); a lone thread is its own mean at every priority, for positive and negative offsets, and is eligible, the test being `>=`; the mean is rounded down (0 and 1 give 0, -1 and 0 give -1, and a thread placed there is eligible); the two-`MID_WEIGHT` example is exact (the mean is 50 ms, no tolerance); a running priority-31 thread at 10 ms and a waiting default thread at 5 ms, with the sum over both: the waiting one is eligible and the running one is not, and with the sum over the waiting one only, the waiting one is not; and the conversion over eleven days, which a 64-bit intermediate gets wrong. The proportionality test over every priority now runs for 1 ms and 1000 s: the real time of eleven days itself (1e15 ns) exceeds the 39 hours that `WeightedTime` holds, at every weight.

**Reference**
Linux EEVDF (`kernel/sched/fair.c`, local checkout `~/Workspaces/projects/linux`, v7.2-rc5 plus 31 commits; the bodies were read there). `update_zero_vruntime()` (line 759): `sum_w_vruntime -= sum_weight * delta; zero_vruntime += delta`. `avg_vruntime()` (780) moves the zero to the average on every call, returns the zero when there is neither weight nor `curr`, and takes `curr`'s own `vruntime` as the average when it is alone; its comment says it is called from `place_entity()`, `update_entity_lag()` and `update_deadline()`, the last at slice expiry, which `recenter()` in `update_thread()` follows; and it biases to the left so that `avg_vruntime() + 0` is eligible, which the floor follows. `vruntime_eligible()` (894) adds `curr` to both the sum and the load. The lag bound is `entity_lag()`. Its sum is an exact integer sum (a `PARANOID_AVG` feature path aside); the exactness here is `WeightedTime`.

**Verification**
- `python3 .agents/skills/verify/scripts/boot_check.py --out out/fair-lane`, run on the final code: build ok, dbg PASS (46 passed, 5 skipped, 0 failed), rel PASS (44 passed, 8 skipped, 0 failed, 7/7 required), host-tests PASS (172 passed; more than the previous entry's 166, from commits other sessions merged into the branch in between). Fair is not in these images, so the boots show only that nothing else moved.
- `-fsyntax-only` on `fair.cpp` and `scheduler.cpp` with the Fair switch on: 2 and 1 diagnostics (`run_queue.hpp:252` `for_each`, item 4, and `fair.cpp` `arm_preempt_timer`, item 12). The `static_assert`s in `common.hpp`, which run in every translation unit, pass.
- Mutation test of the new `static_assert`s by hand: 8 mutants (eligibility with `>`, with a nanosecond-rounded product, ignoring the weight sum; the mean rounded or truncated instead of floored; a term rounded to a nanosecond; a sum without the weight; the old `MID_WEIGHT` division in the term) were all killed once the lone-thread assert also tested eligibility at the mean; the first run let the two eligibility mutants survive.
- Four independent read-only reviewers, run twice because the first run hit the usage limit; the third of them, numerics, hit it again and returned nothing, and its mutation tests were done by hand (above) and its differential test is the queue-logic reviewer's. Queue logic: an exact-integer Python translation of the queue checked against the compiled header on 9788 random inputs (0 mismatches) and fuzzed over 277,336 operations (the rebase invariant held exactly, an empty tree had total 0 and weight 0, the eligibility answers were unchanged by rebases), the `current_` lifecycle traced through every path of `fair.cpp` and `scheduler.cpp`, and the shares simulated: 25.69 : 1 and 25.71 : 1 for priorities 31 and 15 at 64 and 8 ms slices. Code: the diff, the removed names, style and clang-tidy (nothing on the changed lines of `common.hpp` or `types.hpp`). Docs: every sentence of this entry against the code and the Linux checkout. Together they found the zero that did not follow a re-picked thread, the mean that rounded up, a self-contradictory derivation comment, a wrong clause in a `static_assert` comment, repair items for the pick-before-charge order, the `Pi::common()` weight change, the double count through `current_` and the unscaled lag (items 8, 17, 18), and about twenty smaller errors in the first draft of this entry; all were corrected.
- `scripts/format_cpp.py` and `scripts/lint_docs.py` (also on the whole file) are clean.
- Fair was not linked, booted or run. The queue's behaviour is read and modelled, not run, and no test runs it (item 13).

**Deliberately not done**
- The tests of item 13, now also for the finite first deadline, the lone thread's lag through `lag_of()` and a wakeup from another CPU; a faster conversion (item 14); tree membership by `is_active()`, a running thread overridden or reweighted (item 17); the stub that should scale the carried lag (item 18).
- Fair still does not compile: `for_each` (item 4) and the `arm_preempt_timer` block (item 12). The pick still reads the running thread uncharged (item 8).
- Linux's lag bound (`entity_lag()`).

**How it was found**
By the review of 2026-11-03 that produced the repair file, and by the reviewers of the previous entry: one showed that the mean at a pick leaves the running thread out, another that the test's two sums disagree once it is in. The four reviewers of this entry found that the zero must also follow a thread that is only re-picked, that the mean must round down, and that the first draft overstated what the exact sum covers. The author asked for items 6 and 16 and chose the zero; folding item 15 in, closing item 5, and the recentering at the replenish were forced by that choice and are the implementer's.

**The user's prediction**
None (predictions are paused)

**Three questions you should be able to answer**
- Why is the leftmost thread of a deadline-ordered tree not the one with the smallest virtual time, and why does moving the zero to the weighted mean need no search?
- Why do `avg_vruntime()` and `is_eligible()` take their sums from one function, and what did the test answer when they did not?
- Why did moving the zero at every placement make the rounding of a term matter, when the old `min_vruntime_`, which never moved, could do without exactness, and why must the zero also move when a request is replenished?

## Title: FairRunQueue visits its threads for the scheduler (2026-11-04)

**Files**
- `kernel/include/ours/task/sched/fair.hpp` — new `FairRunQueue::for_each()`
- `docs/repairs/261103/kernel-task-sched.md` — item 4 is deleted (the number is kept); the header paragraph, item 13 (the walk joins its test) and item 17 (the offline walk meets delayed sleepers) are edited
- `.agents/tasks/sched-facade/DESIGN.md` — the Fair bullet

**Symptom**
With the Fair switch on, `RunQueue::for_each_thread()` did not compile: it calls `m_normal_queue.for_each(...)` and `FairRunQueue` had no such member. The error is reported at `run_queue.hpp:252`, so in every translation unit that includes the header with the switch on; it was the only diagnostic of `scheduler.cpp`, which instantiates the body in `Scheduler::Current::deactivate()`, and one of the two of `fair.cpp`. Fair is not built, so nothing was observable.

**Cause**
The framework asks every discipline's queue type for one walk, `for_each(visit)` with `visit(SchedEntity &) -> bool`, which a CPU going offline uses to find a queued thread that may run elsewhere (`Scheduler::Current::deactivate()`). `RrRunQueue` gained it on 2026-10-13 with CPU hotplug (`54203477`, `7a5e86db`), three weeks after the run queue's fields were sealed (`abe6f1d0`). Fair's queue was out of the build, so nothing compiled it against the new use.

The contract in `run_queue.hpp` (`for_each_thread()`): every thread queued on the CPU, until `visit` answers false; no order is promised; the running thread is not queued and is not visited; under the run queue lock; and `visit` must not change the queues, because a thread it picks is moved once the walk is over. The last matters for an intrusive tree: erasing the node being visited, or any node, invalidates the iterator the loop holds. The one caller sets a pointer and returns false, and moves the thread after the walk, under the same guard. For Fair that means walking `queue_`, which holds the waiting threads, and not `current_`, which `sums()` counts in the mean while it is active but which is not in the tree. Under `SCHED_DELAY_DEQUEUE` (off) the tree also holds the sleepers whose dequeue was delayed, and the walk visits them (item 17).

**Change**
Before: none.

```cpp
// fair.hpp, FairRunQueue
template <typename Visit>
auto for_each(Visit &&visit) -> void {
    for (auto &object: queue_) {
        if (!visit(object)) {
            return;
        }
    }
}
```
The body has the shape of `RrRunQueue::for_each`. The order is the tree's, earliest virtual deadline first. The one caller does not need it: every round starts from the head again and takes the first thread that may go, so which threads move and when the loop ends do not depend on it. Which thread goes first does decide which lag each carries to its new CPU, which item 18 leaves unscaled anyway. No other form was considered; the walk is the member the framework's template asks for.

**Reference**
The walk is the framework's own: `RrRunQueue::for_each()` in `rr_run_queue.hpp`, which this one copies. For the job it serves, Zircon moves a CPU's threads away in `Scheduler::MigrateUnpinnedThreads()`, which builds a move list over its fair run queue with `CreateMoveList`; the run queue's lock and the visitor-then-move split here are this project's own design.

**Verification**
- `-fsyntax-only` on `fair.cpp` and `scheduler.cpp` with the Fair switch on: 1 and 0 diagnostics. The one left is `arm_preempt_timer` (item 12 of the repair file).
- `python3 .agents/skills/verify/scripts/boot_check.py --out out/fair-lane`: build ok, dbg PASS (46 passed, 5 skipped, 0 failed), rel PASS (44 passed, 8 skipped, 0 failed, 7/7 required), host-tests PASS (172 passed). `fair.hpp` is not in these images, so the boots show only that nothing else moved; the one edit after the run was a comment in `fair.hpp`.
- Two independent read-only reviewers. Code: the real `FairRunQueue::for_each` was run on the host over the real `ustl` `MultiSet` of real `SchedEntity` values, with the real deadline comparator, and visited seven out-of-order keys in order, stopped at the first `false`, visited three equal keys once each, visited nothing on an empty queue and all 100 of a hundred, and did not visit a thread set with `set_current()`; three mutants of the walk failed the same checks (clang-tidy reports nothing on the added lines). Docs: every sentence of this entry against the code and the history. They found two false sentences (when `RrRunQueue` gained the walk, and that the default build never runs it), a contract quoted with an order it does not promise, a Reference that was too short, and the delayed sleepers that the walk would visit; all were corrected.
- `scripts/format_cpp.py` and `scripts/lint_docs.py` are clean.
- Fair was not linked, booted or run. The default build's boot runs `deactivate()` and `RunQueue::for_each_thread()` with `RrRunQueue::for_each` (through `hotplug.test`), which shows the caller's logic and not Fair's walk.

**Deliberately not done**
- A test of the walk in the build: it joins the host-style test of `FairRunQueue` (item 13), which needs Fair to compile first (item 12).
- The `arm_preempt_timer` block (item 12), and a filter for delayed sleepers, which waits for a predicate on that state (item 17).
- The "Hands off" rule for `fair.*` in `kernel-design` is unchanged: this edit is on the author's request and adds no EEVDF semantics.

**How it was found**
By the first syntax check of `scheduler.cpp` with the Fair switch on (`python3 scratchpad/fair_check.py kernel/kernel/task/sched/scheduler.cpp`, the check of the rename entry of 2026-11-03), which reported `for_each` as its one diagnostic. The author asked for it.

**The user's prediction**
None (predictions are paused)

**Three questions you should be able to answer**
- Why is the running thread not visited, and where is it counted instead?
- Why must the visitor not change the queue during the walk, and how does the one caller avoid the problem?
- What does the walk's order decide for the caller, and what does it leave alone?

## Title: Fair arms no preemption timer of its own, and now compiles (2026-11-04)

**Files**
- `kernel/kernel/task/sched/fair.cpp` — `Fair::set_next_thread()` loses its `if constexpr (SCHED_NOHZ)` block and gains a comment saying where the deadline is set
- `kernel/include/ours/task/sched/rr.hpp`, `.agents/skills/kernel-design/references/task.md`, `.agents/tasks/sched-facade/DESIGN.md` — a comment and two notes that called Fair "out of the build" now say it is off by default
- `docs/repairs/261103/kernel-task-sched.md` — item 12 is deleted (the number is kept); the header, item 8 (what the Fair lane showed, and its Acceptance), items 13 and 14 are edited; items 19 and 20 are new

**Symptom**
`runtime.arm_preempt_timer(remaining_timeslice)` did not compile: `RunQueue::arm_preempt_timer` was renamed `set_preemption_deadline` on 2026-10-26 (`f7825b7c`) and this call was left, because `fair.cpp` was not built. It was the last diagnostic of `fair.cpp` with the Fair switch on, and it did not depend on `SCHED_NOHZ`, whose default is false and which no commit ever set true: the discarded branch of an `if constexpr` in a member of a non-template class is still checked (the same error with `SCHED_NOHZ` false and true, in HEAD's `fair.cpp`; none after). With it gone Fair compiles, links and boots (below).

**Cause**
The block came in with the checkpoint `feec7b72` as `driver.reset_preempt()`, for a scheduler whose picks re-armed the clock event. `c6764d82` (2026-10-13) made `Scheduler::set_next_thread()` (`scheduler_inline.hpp`) set the preemption deadline at every pick of a thread other than the idle thread, from `ctx.remaining_slice()`, after the discipline's own `set_next_thread()` and under no `SCHED_NOHZ` condition; from then on the block was redundant. It could not add to that: its `consumed_timeslice` is `m_exec_runtime - m_prev_exec_runtime`, evaluated right after `set_next_object()` has made the two equal, so it is zero, and `remaining_timeslice` is the whole slice. Both write the single deadline slot of the CPU's `TimerQueue` (`SchedClock::set_preemption_deadline()`), and the scheduler's write comes last, so the block's would have been overwritten with the same value. The comment above it, that the next thread "has been woken just now" and the slice has to be recalculated, describes a case that cannot arise: `set_next_object()` restarts the slice at every pick, so no thread is picked with part of it consumed.

**Change**
```cpp
// before
auto Fair::set_next_thread(RunQueue &runtime, Thread *next) -> void {
    runtime.queue<Discipline::Normal>().verify();
    auto &ctx = next->scheduling_entity();
    set_next_object(runtime, ctx);
    if constexpr (SCHED_NOHZ) {
        // (a comment: the next thread may have been woken just now, so recalculate)
        auto const consumed_timeslice = ctx.m_exec_runtime - ctx.m_prev_exec_runtime;
        auto const remaining_timeslice = ctx.m_timeslice.length() - consumed_timeslice;
        if (remaining_timeslice < SchedDuration{0}) { return; }
        runtime.arm_preempt_timer(remaining_timeslice);
    }
}

// after
/// The preemption deadline that ends the slice is set by the scheduler after
/// this, from remaining_slice() (Scheduler::set_next_thread()), so Fair arms no
/// timer of its own.
auto Fair::set_next_thread(RunQueue &runtime, Thread *next) -> void {
    runtime.queue<Discipline::Normal>().verify();
    auto &ctx = next->scheduling_entity();
    set_next_object(runtime, ctx);
}
```
Options: delete the block, or say what it adds. It adds nothing, so it is deleted. Renaming the call to `set_preemption_deadline(...)` was rejected: it would arm, a moment before the scheduler does, the value the scheduler arms again. `SCHED_NOHZ` itself is left, with no reader (item 20).

**Reference**
The framework side is this project's own design (`kernel-design`, references/task.md, Time; `RoundRobin::set_next_thread()` relies on the same call). Zircon sets the preemption deadline in `Scheduler::RescheduleCommon()` through `PreemptReset()` (`scheduler.cc`), as the scheduler does here. Linux arms a high-resolution timer from the discipline, `hrtick_start_fair()` (`kernel/sched/fair.c`), which is the design this block copied and this project did not follow.

**Verification**
- `-fsyntax-only` on `fair.cpp` and `scheduler.cpp` with the Fair switch on: 0 and 0 diagnostics.
- `python3 .agents/skills/verify/scripts/boot_check.py --out out/fair-lane` (the default lane, `rr`), run on the final code: build ok, dbg PASS (46 passed, 5 skipped, 0 failed), rel PASS (44 passed, 8 skipped, 0 failed, 7/7 required), host-tests PASS (172 passed). `fair.cpp` is not in these images; the Fair lane's own boots are in the next bullet.
- **Fair, built and booted for the first time.** `python3 tools/set.py bringup.x86 --out out/fair-on --args 'sched_normal_discipline="fair"'`, then `boot_check.py --out out/fair-on --no-tests`: build ok (26.8 s cold), dbg PASS (46 passed, 5 skipped, 0 failed), rel PASS (44 passed, 8 skipped, 0 failed, 7/7 required). Fair is linked there (19 `Fair::` and `FairRunQueue` symbols in `vmours` and no `RoundRobin` ones; the default lane has none of the first and eight of the second). A reviewer reproduced all of it in a directory of its own.
- **The kernel tests on that lane** (`kshell_run.py`): eight commands were run, of the 46 `*.test` and `*.stress` commands there are: `sleep.test`, `waitqueue.test`, `semaphore.test`, `threadjoin.test`, `dpc.test` and `timer.test` pass (in the author's boots and in the reviewer's), `preempt.test` and `hotplug.test` fail. The other 38 were not run. `preempt.test` fails three of its eight checks, two when it counts four turns or more: counted turns 2 to 4 in six boots (one run of 15 split by host stalls), none a whole slice, the longest 197 to 263 ms against a 64 ms slice; the round-robin lane passes it 8 of 8 in three runs (about 13 whole turns, the longest 66 ms). An earlier run of the author's on the round-robin lane that failed one check ("most turns are a whole slice", 10 of 21) was a noisy one and did not reproduce. `hotplug.test` fails 2 of its 47 checks ("and every spinner goes on elsewhere") on the Fair lane in all three boots and passes 47 of 47 on round-robin; the cause is not established (item 19).
- **The cause of `preempt.test`'s bursts,** by an exact-integer model of the queue that a reviewer wrote: the pick reads the running thread uncharged (item 8), the waiter is then ineligible, the running thread is picked again and gets a whole slice. A turn is 2 slices at an exact tie and 3 to 4 once timer latency breaks the ties against the waiter, which matches the observed 197 ms and 260 to 263 ms. The model does not explain the run of 15 turns.
- **An experiment of the author's, reverted and not committed, and not reproduced by the reviewer:** charging the running thread at the top of `Fair::evaluate_next_thread()` (`if (curr is active) update_thread(runtime, curr);`) gave 12 turns of which 11 whole, longest 131 ms, and 10 of which 7 whole, longest 132 ms, in two runs, with one failed check ("no turn runs past its slice"). The reviewer's model gives the same shape and attributes the two-slice turns that remain to the first half of item 9 (a re-pick gets a whole new slice) and not to its `vdeadline` half.
- Two independent read-only reviewers, each building in a directory of its own. Code: the redundancy of the block read against `scheduler_inline.hpp`, `SchedClock` and `TimerQueue` and against the git history, the Fair lane rebuilt and rebooted (same numbers), the kernel tests rerun, and `preempt.test` run on both lanes; an exact-integer model of the queue's first slices. Docs: every sentence of this entry against the code, the history and the logs. They found two results of mine that were wrong (the round-robin lane's `preempt.test` failure, which did not reproduce, and the number of failed checks), a second failing test I had not run (`hotplug.test`, item 19), an acceptance that assumed round-robin's alternation, the stale "out of the build" notes, a missing Reference, and the unread `SCHED_NOHZ` (item 20); all were corrected.
- `scripts/format_cpp.py` and `scripts/lint_docs.py` are clean.

**Deliberately not done**
- Fair is not selected in the product: `sched_normal_discipline` stays `"rr"` and the default images do not contain it.
- The fix of the bursts (item 8, and the re-pick of item 9), and the cause of `hotplug.test` (item 19): how Fair ends a slice and breaks a tie is the author's to decide, and the experiment above is evidence for that, not a change. `preempt.test`'s three turn checks assume strict alternation, which is the round-robin's property; item 8 says so.
- `SCHED_NOHZ` and `sched_nohz` (item 20).
- The "Hands off" rule for `fair.*` in `kernel-design` is unchanged: this edit is on the author's request and adds no EEVDF semantics.

**How it was found**
By the first syntax check of `fair.cpp` with the Fair switch on, in the rename entry of 2026-11-03. The author asked for the item ("处理第 12 项", 2026-11-04). Building and booting a Fair lane was not part of the request; it is the check that "Fair compiles" is more than a syntax check, and it found the two failing tests.

**The user's prediction**
None (predictions are paused)

**Three questions you should be able to answer**
- Why would the deleted block have armed the full slice whatever the thread had consumed, and who arms it instead?
- Why did a discarded `if constexpr` branch still stop the file from compiling with `SCHED_NOHZ` off?
- Why does a pick that reads the running thread uncharged re-pick it, and why does the re-pick get a whole new slice?

## Title: Fair charges the running thread before it picks, and preempt.test allows two slices in a row (2026-11-04)

**Files**
- `kernel/kernel/task/sched/fair.cpp` — `Fair::evaluate_next_thread()` calls `update_thread(runtime, curr)` first when `curr` is of the Normal discipline and active
- `kernel/commands/preempt_test.cpp` — `MAX_SLICES_IN_A_ROW` (2 when Fair is the Normal discipline, 1 otherwise): a turn is whole when it is that many slices or fewer, a turn longer than a slice that is not whole counts as stray and fails "no turn runs past its slices", and the log line and two verdict names say "a whole number of slices"
- `docs/repairs/261103/kernel-task-sched.md` — item 8 is cut down to the tickless-charging question and its severity lowered; item 9 gets the two-slice turns; item 19's Cause is updated
- `.agents/tasks/sched-facade/DESIGN.md` — the Fair bullet

**Symptom**
In the Fair lane (`sched_normal_discipline="fair"`), `preempt.test` failed in every boot: two spinners on one CPU with a 64 ms slice, which round-robin runs a slice at a time, ran 2 to 4 counted turns of 196 to 197 ms (three slices) in four boots of an experiment and up to 263 ms (four) in earlier ones, and failed three of its eight checks, or two when it counted four turns or more.

**Cause**
`Scheduler::evaluate_next_thread()` picks first and then calls `put_prev_thread()`, which charges the running thread. At the pick the waiter was charged in full when it last stopped, and the running thread is up to a slice behind in the mean. A worked example, two equal threads A and B at 0, one 64 ms request being 78,387,949 ns of virtual time: A runs, and at its slice end the pick sees A at 0 (uncharged) and B at 0: B is eligible and runs. At the next slice end the pick sees B at 0 (uncharged) and A at 78,387,949 (charged at its hand-over): the mean is 39,193,974, A is ahead of it and ineligible, `Fair::evaluate_next_thread()` answers null, the scheduler falls back to the running thread, and `set_next_object()` plus `Scheduler::set_next_thread()` give B a whole new slice. B ran two. Then A is eligible again at a tie and the same thing happens to whichever stays. An exact-integer model of the queue that a reviewer wrote reproduces the turns of the base lane: 2 slices at an exact tie, and 3 to 4 once timer latency breaks the ties against the waiter, which matches the observed 197 ms and 260 to 263 ms.

**Change**
```cpp
// Fair::evaluate_next_thread(), before the walk of the tree
auto &curr_entity = curr->scheduling_entity();
if (curr_entity.discipline() == Discipline::Normal && curr_entity.runqueue_state().is_active()) {
    update_thread(runtime, curr);
}
```
Linux does the same: `pick_task_fair()` calls `update_curr()` before it picks. `put_prev_thread()` charges again at the hand-over and adds nothing: the scheduler clock is read once at the top of `reschedule_common()` and not again between the pick and the hand-over, and the first replenish moved the deadline, so the test does not fire twice (a reviewer read this and confirmed that the pending bit the pick-time replenish sets is consumed by `take_pending_local()` right after the pick, as the old mark was). The guard on the discipline keeps the call from charging a thread of another discipline once there is one.

Three variants were measured in an experiment before this change, four boots each (the patch applied and reverted by a script):

| Variant | `preempt.test`: turns, whole slices (63 to 68 ms), longest turn | Failed checks |
|---|---|---|
| base | turns 2, 2, 4, 2; whole 0, 0, 0, 0; longest 197, 196, 197, 196 ms | 3, 3, 2, 3 of 8 |
| A, charge first (this change) | turns 11, 9, 13, 8; whole 11, 8, 13, 5; longest 66, 131, 66, 132 ms | 0, 1, 0, 1 |
| B, A and the running thread competes (returned when active, eligible, and either no waiter is eligible or its deadline is earlier than the first eligible waiter's) | turns 14, 13, 12, 20; whole 13, 13, 2, 8; longest 67, 67, 131, 132 ms | 0, 0, 2, 2 |

B did no better than A and costs more code, and with one waiter it answers differently from A only on an exact tie of `vruntime`, so it was not taken (it is the form `pick_eevdf()` has in Linux). Its two boots with 2 of 12 and 8 of 20 turns whole are not explained by the tie argument; they look like host stalls splitting turns, as in the round-robin lane's one noisy boot below, and four boots run one variant after the other cannot separate that from a real difference.

**The remaining two-slice turns, and the test.** After A, `preempt.test` still shows turns of two slices (130 to 132 ms) in more than half of the boots (12 of the author's 18: 2 of 4 in the experiment, 4 of 6 with the first form of the test, 6 of 8 with the final code; 13 of 20 in a reviewer's). The model's reading, not instrumented: a waiter ahead of the mean by a sliver (timer latency gave it a longer slice last turn) is ineligible, the running thread is picked again, and it gets a whole new slice, though it is owed the sliver (item 9). They are within EEVDF's bound, because the lag stays within a slice, and `preempt.test`'s turn checks were written for round-robin, which hands the CPU over at every slice end. A reviewer found that allowing two slices everywhere would blind the test on the round-robin lane, where a second slice in a row is always a bug, and that a single late-fired preemption (a turn between 68 and 127 ms) would pass the longest-turn bound. The test therefore became:

```cpp
// before
if (turn >= slice - EARLY && turn <= slice + late) { num_whole += 1; }
verdict(tally, longest <= slice + late, "no turn runs past its slice");
// after (abbreviated: the real code casts the counts to i64)
constexpr usize MAX_SLICES_IN_A_ROW = 1 + static_cast<usize>(SCHED_NORMAL_DISCIPLINE_FAIR);
for (usize slices = 1; slices <= MAX_SLICES_IN_A_ROW; ++slices) {
    is_whole |= turn >= slice * slices - EARLY && turn <= (slice + late) * slices;
}
if (is_whole) { num_whole += 1; } else if (turn > slice + late) { num_stray += 1; }
verdict(tally, num_stray == 0, "no turn runs past its slices");
```
Round-robin keeps exactly the old windows, and so fails any turn over 68 ms as before. Fair accepts a turn of one or two slices, and a turn longer than a slice that is neither (the dead zone of more than 68 and less than 127 ms, or past 136 ms; with a periodic tick, whose lateness bound is 10 ms larger per slice, more than 78 and less than 127 ms, or past 156 ms) fails. Turns shorter than a slice, which host stalls produce, stay allowed. A stall of more than 1 ms in the middle of a two-slice turn splits it in two, though, and a piece of more than 68 and less than 127 ms counts as stray, so on the Fair lane a host stall can now fail "no turn runs past its slices" where the first form's single 136 ms bound did not: 3 of the 40 Fair boots on record had a split turn and none landed in the dead zone. The round-robin lane is as strict as before. If that flake shows, tolerate one stray turn, which costs the single late-preemption case. The checks "the spinners take turns" (at least four turns in all) and "most turns are a whole number of slices" keep their bounds. The old Fair behaviour still fails: its 196 to 263 ms turns are past the 136 ms limit. Not done: requiring each spinner to have at least two counted turns, which would make "take turns" independent of how long a turn is.

**Reference**
Linux EEVDF: `pick_task_fair()` calls `update_curr()` before `pick_eevdf()`, and `pick_eevdf()` considers `curr` (`kernel/sched/fair.c`, local checkout, v7.2-rc5 plus 31 commits). Linux re-runs the pick at a tick once the running entity is past its protected slice (`update_curr()` asks for a reschedule when `protect_slice()` is false; a tick is 1 / `CONFIG_HZ`, 4 ms by default, `kernel/Kconfig.hz`), and with HRTICK by a high-resolution timer, so a thread owed a sliver is stopped within a tick; here a slice ends only at its deadline.

**Verification**
- `-fsyntax-only` on `fair.cpp` and `scheduler.cpp` with the Fair switch on: 0 and 0 diagnostics.
- `boot_check.py --out out/fair-lane` (default lane): build ok, dbg PASS (46 passed, 5 skipped, 0 failed), rel PASS (44 passed, 8 skipped, 0 failed, 7/7 required), host-tests PASS (172 passed). `boot_check.py --out out/fair-on --no-tests` (Fair lane): dbg PASS (46/5/0), rel PASS (44/8/0, 7/7). Run on the final code.
- **`preempt.test` on the Fair lane, final code, eight boots:** all eight pass, with 9 to 13 turns, 9 to 12 of them whole (a host stall split a turn in two boots: 13 turns of which 11 whole, and 12 of which 10), the longest 66, 132, 132, 130, 131, 131, 66, 131 ms. Earlier boots of the author with the first form of the test (six) and of a reviewer (twenty, 6 to 13 turns, the longest 66 to 132 ms) all passed too. **On the round-robin lane, final code, six boots:** all six pass (12 or 13 turns, all whole, the longest 66 ms). Before the test was hardened the same lane had seven passes in eight; the failure was one noisy boot, straight after `boot_check.py` had built that lane and booted its images: 32 turns of which 3 whole, the longest 70 ms, and the probe timer late. The final test fails any round-robin turn over 68 ms, as the pre-change test did, so that boot would now fail three checks. A host stall splits turns (a gap over 1 ms starts a new turn) and would also make the probe timer late, but no run was instrumented. Before the change to Fair the same lane had shown one such run in four (10 of 21 turns whole, in the previous entry's investigation), so it is not new. Five of those boots are only in the author's transcript.
- **Other tests on the Fair lane:** `sleep.test`, `waitqueue.test`, `semaphore.test`, `threadjoin.test`, `dpc.test` and `timer.test` pass (run on the code before the last edit of `fair.cpp`, which cannot change them while every thread is of the Normal discipline). `hotplug.test` fails the same 2 of its checks, "and every spinner goes on elsewhere", in all the boots (item 19), also after this change.
- Two rounds of independent read-only reviewers, each rebuilding and rebooting the lanes in directories of its own. First round: the charge-first call read against `reschedule_common()`, the idle and boot threads, delayed-dequeue sleepers and the pending bit (no defect), the test's arithmetic, a model of the spinners (a two-slice run in about 75 % of boots and never a third), and twenty Fair boots and six round-robin boots of `preempt.test`, all passing. It found that allowing two slices everywhere would blind the test on the round-robin lane and that a turn between 68 and 127 ms would pass, which became `MAX_SLICES_IN_A_ROW` per discipline and the stray-turn check. Second round: the delta, the windows for eleven turn lengths, the macro in a constant expression in all 104 translation units of `kernel/commands`, and the entry's numbers against the raw logs; it found two wrong counts, the host-stall flake the stray check adds on the Fair lane, and the age of the other tests' runs; all were corrected.
- `scripts/format_cpp.py` and `scripts/lint_docs.py` are clean.

**Deliberately not done**
- The re-pick's whole new slice (item 9) and the instrumentation that would show it is the cause of the two-slice turns; variant B; a tolerance on eligibility.
- `hotplug.test` (item 19): the charge at the pick did not change it.
- Fair is still not selected in the product.
- The queue count that drifts above the scheduler's (item 10), which makes the fast path in `pick_next_thread()` rarely taken.

**How it was found**
By the first Fair boot, in the previous entry, and by the investigation of `preempt.test` of 2026-11-04: `/private/tmp/.../scratchpad/variants.py` (a script that applied each variant, built the Fair lane, ran the tests and restored `fair.cpp` from git) and a reviewer's exact-integer model. The author chose "charge first and relax the test" from the options measured there. The edit of `fair.cpp` is on the author's request; it changes when Fair charges, which is EEVDF semantics, and the "Hands off" rule for `fair.*` in `kernel-design` is otherwise unchanged.

**The user's prediction**
None (predictions are paused)

**Three questions you should be able to answer**
- Why does a pick that reads the running thread uncharged re-pick it, and why does charging first remove the three-slice turns?
- Why does a two-slice turn remain, and why is it still within EEVDF's lag bound?
- What do `preempt.test`'s turn checks assume about round-robin that Fair need not satisfy, and why does round-robin keep the old windows?

## Title: preempt.test bounds a turn by two slices again, and no longer fails the stretch between one and two (2026-11-05)

**Files**
- `kernel/commands/preempt_test.cpp` — the "stray turn" count of the previous entry is gone; "no turn runs past its slices" is `longest <= (slice + late) * MAX_SLICES_IN_A_ROW` again

**Symptom**
In the Fair lane, one boot of nine with the previous form of the test failed "no turn runs past its slices" with 13 turns of which 11 whole and the longest 96.7 ms. No spinner ran 97 ms in a row on a 64 ms slice: the turn was a longer one cut by the host.

**Cause**
The previous entry made a turn that is longer than a slice and not within a window of one or two slices (a "dead zone" of more than 68 and less than 127 ms) fail, to catch a single late-fired preemption. A spinner sees a turn as the stretch between two jumps of the clock larger than `GAP` (1 ms), so a host stall of more than 1 ms in the middle of a two-slice turn of about 130 ms splits it into two pieces, and unless the stall falls near either end or at about 62 to 68 ms one piece lies in the dead zone. That entry's review had predicted it ("3 of the 40 Fair boots on record had a split turn and none landed in the dead zone") and offered the way out; this is the boot in which it landed. Round-robin cannot be hit: its pieces are all under a slice and are ignored.

**Change**
```cpp
// before (previous entry)
if (is_whole) { num_whole += 1; } else if (turn > slice + late) { num_stray += 1; }
verdict(tally, num_stray == 0, "no turn runs past its slices");
// after
if (is_whole) { num_whole += 1; }
verdict(tally, longest <= (slice + late) * static_cast<i64>(MAX_SLICES_IN_A_ROW), "no turn runs past its slices");
```
With `MAX_SLICES_IN_A_ROW` at 1 (round-robin) this is the pre-change check, `longest <= slice + late`. On the Fair lane a turn of up to two slices plus lateness passes, whatever its length inside that bound. The cost: a single late-fired preemption (a turn between 68 and 127 ms) goes unseen there; "most turns are a whole number of slices" still fails a lane in which they are common. Tolerating one stray turn instead was considered and not taken: two stalls in one boot would still fail it, and it keeps the extra count.

**Reference**
None, this project's own test.

**Verification**
- `boot_check.py --out out/fair-lane` (default lane): build ok, dbg PASS (46 passed, 5 skipped, 0 failed), rel PASS (44 passed, 8 skipped, 0 failed, 7/7 required), host-tests PASS (173 passed). `boot_check.py --out out/fair-on --no-tests` (Fair lane): dbg PASS (46/5/0), rel PASS (44/8/0, 7/7).
- `preempt.test`, Fair lane, ten boots: all pass, with 10 to 13 turns, 10 to 13 of them whole, the longest 66, 129, 131, 132, 130, 66, 66, 130, 130, 131, 66 ms (a mixture of one and two slices as before). Round-robin lane, four boots: all pass, 13 turns, all whole, the longest 66 to 67 ms.
- `scripts/format_cpp.py` is clean.

**Deliberately not done**
- Telling a stall-split turn from a late preemption; requiring each spinner to have two counted turns.

**How it was found**
By a boot in the run for the next entry: `kshell_run.py --out out/fair-on preempt.test` printed "13 turns, 11 of them a whole number of slices, the longest 96687 us" and failed "no turn runs past its slices".

**The user's prediction**
None (predictions are paused)

**Three questions you should be able to answer**
- How does a host stall turn one 130 ms turn into a 97 ms one in the spinner's record?
- Why can the round-robin lane not be hit by the same thing?
- What does the longest-turn bound give up compared with the window check?

## Title: hotplug.test waits until every spinner has moved, instead of 100 ms (2026-11-05)

**Files**
- `kernel/commands/hotplug_test.cpp` — the check "and every spinner goes on elsewhere" polls (`poll_until`, up to `LOST_AFTER`, 5 s) for every spinner's round counter to pass its value from before the 100 ms sleep; the comment above says why, and the comment of `OFFLINE_FOR` says "first"
- `docs/repairs/261103/kernel-task-sched.md` — item 19 (this failure) is deleted (the number is kept), item 21 is new, the header is edited
- `.agents/tasks/sched-facade/DESIGN.md` — the Fair bullet

**Symptom**
In the Fair lane `hotplug.test` failed "and every spinner goes on elsewhere" twice, once per round, in every Fair boot on record (25 boots in the investigation's records, 16 of the unmodified test and 9 of instrumented copies; none passed); the round-robin lane passed 47 of 47.

**Cause**
The test starts two spinners per online CPU (eight on four), takes CPU 3 offline, and checks that each spinner's round counter advanced between the sleeper's join and the end of a 100 ms sleep, to catch one stranded on the offline CPU. The two spinners of CPU 3 go to the survivors, which then hold 3, 3 and 2: as even as eight over three CPUs can be, so somebody shares a CPU with two others whatever the balancing. Instrumenting the test (the script applied a patch to the test, built a Fair lane and a round-robin lane, restored the source and ran both; none of it is committed) showed:

1. **A spinner stays on the CPU it was placed on.** Every spinner that ran in the window ran on exactly one CPU (a one-bit mask of the CPUs it was seen on in 117 of 117 spinner-windows; the 11 that got nothing have an empty mask). No CPU steals work: `Scheduler::steal_work_from()` returns null (item 21, a different problem: it does not cause this one).
2. **A spinner that shares with two others waits longer than a slice cycle suggests.** Per-turn records: in round-robin a spinner ran 65 ms and waited 130 to 132 ms (three turns of about 65 ms, 196 ms); in Fair it ran 64 to 66 ms and waited 130 to 133 ms (9 of 12 turns), 156 or 157 ms (2) or 175 ms (1). The longer Fair waits are not whole slices; their cause was not established (the near-ties of item 9 are one candidate).
3. **In the Fair lane at most one spinner of a three-spinner CPU got nothing in the window, never two:** 45 of 68 CPU-windows had one and 23 had none. Of the 18 test windows, 10 had one such spinner in all and 8 had two (one per CPU). Over five successive 100 ms windows every spinner advanced (one spinner's counts per window: 0, 470719, 0, 444482, 0), so none was stranded: it was slow, not stuck.
4. **The window is longer than 100 ms, and longer in round-robin.** The check runs from the test thread's wake-up after the sleep; the log timestamps between "the thread asleep there wakes on time" and "no spinner runs on it" put the window at 166 to 171 ms in 13 of 14 round-robin rounds (one of 232 ms) and at 167 to 169 ms in four, or 101 to 103 ms in six, of the old test's rounds in the Fair lane. That the test thread lands on CPU 2, the CPU with two spinners, and waits for a slice is a reading, not verified. A round-robin window of 166 ms or more cannot lie inside a wait of 131 ms, so it cannot show a zero, by construction; a Fair window of 101 to 103 ms can, and a Fair wait of 175 ms is longer than a 167 ms window.

The test's assumption, that every spinner gets the CPU within its window with two others on the CPU, holds for round-robin because the window happens to be longer than its wait, and for Fair it does not.

**Change**
```cpp
// before
auto _ = Thread::Current::sleep_for(OFFLINE_FOR, task::Interruptible::No);
auto ran_there = false;
auto all_went_on = true;
for (usize i = 0; i < state.num_spinners; ++i) {
    ran_there |= state.spinners[i].ran_there_offline.load(std::memory_order_relaxed);
    all_went_on &= state.spinners[i].rounds.load(std::memory_order_relaxed) > rounds_before[i];
}
// after
auto _ = Thread::Current::sleep_for(OFFLINE_FOR, task::Interruptible::No);
auto const all_went_on = poll_until([&state, &rounds_before] {
    for (usize i = 0; i < state.num_spinners; ++i) {
        if (state.spinners[i].rounds.load(std::memory_order_relaxed) <= rounds_before[i]) { return false; }
    }
    return true;
});
auto ran_there = false;
for (usize i = 0; i < state.num_spinners; ++i) { ran_there |= state.spinners[i].ran_there_offline.load(std::memory_order_relaxed); }
```
The 100 ms sleep stays, so the spinners still run with the CPU offline for as long as before before "no spinner runs on it while it is offline" is read (it is read after the poll, so over a window at least as long; `state.offline` is cleared only after the check, so a longer window cannot count a spinner that ran on the CPU once it was back). The progress check waits as long as it takes, up to `LOST_AFTER`. A stranded spinner never advances, so the check fails after 5 s per round, where it failed after about 100 to 170 ms; a healthy run finishes as soon as the slowest has moved. The test takes 3.3 s (Fair) to 3.8 s (round-robin) by the log timestamps; a failing run takes about 14 s by the code, not measured. Rejected: a longer fixed window (any number is a guess about the slices and the sharing), and fixing the discipline (the failure is the test's assumption).

**Reference**
None, this project's own test.

**Verification**
- `boot_check.py --out out/fair-lane` (default lane): build ok, dbg PASS (46 passed, 5 skipped, 0 failed), rel PASS (44 passed, 8 skipped, 0 failed, 7/7 required), host-tests PASS (173 passed). `boot_check.py --out out/fair-on --no-tests` (Fair lane): dbg PASS (46/5/0), rel PASS (44/8/0, 7/7).
- `hotplug.test`: Fair lane, eight boots of the author and seven of a reviewer, all pass (47 checks); round-robin lane, four boots of the author and seven of a reviewer, all pass.
- `preempt.test` still passes on both lanes (previous entry). A reviewer found the clang-tidy output for the file clean; `scripts/format_cpp.py` and `scripts/lint_docs.py` are clean.
- Not run: the failing path, a spinner whose counter never advances. It needs an edit to the test (or, for a genuinely stranded one, to `Scheduler::Current::deactivate()`); that the check fails, after at most 5 s per round, is by reading `poll_until`.
- A reviewer read the entry against the instrumentation logs and the code, and found that the first draft measured the wrong window (100 ms for 167), called the round-robin pass luck, mislabelled item 21 as an imbalance and miscounted the failing boots; all were corrected.

**Deliberately not done**
- Work stealing (item 21).
- A reviewer's boot on 2026-11-04 counted 53 checks in `hotplug.test`, not 47: `check_start_withdrawn()` adds six verdicts when the withdrawal comes before the CPU arrives, and the usual path logs "that case is not made" and gives 47. That is a coverage gap of that check, unrelated to this change and not addressed.

**How it was found**
By the first Fair lane (2026-11-04) and its failing `hotplug.test`. The candidates were in the repair file; the instrumented runs refuted the first, the near-ties of `preempt.test`, as the cause, and showed the window. The author asked for the investigation ("查 hotplug.test"); the change is to a test, not to `fair.*`.

**The user's prediction**
None (predictions are paused)

**Three questions you should be able to answer**
- Why can a spinner that shares its CPU with two others make no progress for 100 ms without being stranded?
- Why can round-robin not show a zero in this test, and what makes Fair able to?
- What does the poll fail on that the fixed window failed on, and how long does each take?

## Title: An idle CPU takes a queued thread from another CPU, its own NUMA node first (2026-11-05)

**Files**
- `kernel/kernel/task/sched/scheduler.cpp` — new `Scheduler::Current::steal()`, `num_steal_looks()` and `num_steals()`; the counters `SCHEDULE_STEAL_LOOKS` and `SCHEDULE_STEALS`; the thresholds `STEAL_MIN_RUNNABLE_SAME_NODE` (2) and `STEAL_MIN_RUNNABLE_OTHER_NODE` (3), in the unnamed namespace; includes `ours/mm/cpu_node_map.hpp`
- `kernel/include/ours/task/sched/scheduler.hpp` — the declarations (`steal()` is `[[nodiscard]]`), and a note on the stub `steal_work_from()` saying what pulls today
- `kernel/kernel/task/idle_thread.cpp` — the idle loop calls `Scheduler::Current::steal()` when `num_runnable()` is zero, before it halts
- `kernel/commands/steal_test.cpp` — new `steal.test`; `kernel/commands/BUILD.gn` lists it
- `kernel/mm/include/ours/mm/cpu_local.hpp`, `kernel/include/ours/task/thread.hpp`, `kernel/commands/hotplug_test.cpp` — three comments that said no CPU takes a queued thread, now false (the first is a warning about exactly this change; its text now says what the audit of the callers found)
- `docs/repairs/261103/kernel-task-sched.md` — item 21 is deleted (the number is kept); items 22 to 26 and 28 to 33 are new (27 was the audit, done); the header is edited
- `.agents/tasks/sched-facade/DESIGN.md` — the Fair bullet

**Symptom**
No CPU took a queued thread from another. A thread was placed when it became runnable (a start or a wakeup) or when its CPU went offline, by `select_target_cpu()`, and then stayed on that CPU's queue however idle the others were. Observed on 2026-11-05 with `hotplug.test` instrumented: eight always-running spinners on four CPUs, CPU 3 taken offline, its two spinners go to the survivors, which hold 3, 3 and 2; when CPU 3 returns it gets nothing (by inference from the identical spinner-to-CPU map of the second round, not observed). Round-robin and Fair alike.

**Cause**
`Scheduler::steal_work_from()` was a stub returning null, called at the end of `pick_next_thread()`. Two things make the obvious fix, filling in that stub, the wrong place here:
1. **Who looks, and when.** An idle CPU runs `IdleThread::run()`, which halts when `num_runnable()` is zero and takes no tick. A CPU that comes back online enters that loop through `IdleThread::come_back()` and does not call `pick_next_thread()` first; `Current::reactivate()` only sets the state to Active under the run queue lock. So a pull that lives in the pick does not run at come-back time (a later preemption of the CPU would reach it, but nothing guarantees one). The loop is where "this CPU has nothing to run" is decided on every path, so that is where the CPU looks. What it does not cover is a CPU that has already halted: it looks again only when an interrupt wakes it (repair item 23).
2. **Locks.** A pull needs the victim's run queue lock; the pick holds the thief's. No order of run queue locks is defined, and two CPUs pulling from each other under their own locks would take them in opposite orders and deadlock. The idle loop holds none, so the pull can follow `Scheduler::Current::deactivate()`'s shape, one lock at a time: take the victim's lock, detach the thread (`deactivate_thread()`, which makes it Migrating and takes it off the queue), drop it, take the own lock, `activate_thread()`. Between the two the thread is in neither queue and its `m_curr_cpu` still names the victim: the window `deactivate()` has between its own two locks. Two things reach a thread there and are not kept out: `set_cpu_affinity()` installs a mask with no run queue lock, so a mask that excludes the thief, installed after `steal()` checked it, leaves the thread queued on the thief (repair item 25; `deactivate()` re-selects the destination, so its window is shorter); and `Scheduler::Pi::common()` takes the victim's lock by `m_curr_cpu` and would dequeue a thread that is not there (repair item 26; nothing calls it today).

The choice of victim is made on a racy hint, `num_runnable()` (the count of queued threads, the running one included), read without the victim's lock as `select_target_cpu()` reads it, and confirmed under the lock, where the depth is checked again (two thieves that read the same count must not both take from a queue that had enough for one) and the victim's queue is walked (`for_each_thread`), skipping the thread the victim has nominated to run next (a directed yield), any thread that is not Ready and is only still in a Fair queue (`SCHED_DELAY_DEQUEUE`, off by default, leaves asleep, parked, suspended and dead threads in the tree until their lag runs out), and any thread whose affinity mask, intersected with the active CPUs, does not contain this CPU. A hint that turned stale just finds nothing and the CPU halts.

**Why the node comes first.** A thread that has run on a node has its memory there. Taking one from a CPU on the same node costs a cache refill; from another node, every access to what it has allocated crosses the interconnect. So `steal()` picks two candidates, the deepest queue on this node and the deepest elsewhere, and tries the same node first (so the order is two buckets, not the "deepest overall"). The two have different thresholds: on the same node one running and one waiting is enough (2); from another node a second waiting thread is asked for (3), so a single waiting thread is not dragged across just to shave one slice of latency. The numbers are guesses, recorded as item 24.

**Fair.** `Fair::dequeue_thread()` records the thread's lag against the victim's weighted mean on the way out and `enqueue_thread()` goes through `adjust_vlag()`, which places it against the thief's, so the move needs no edit in `fair.*`. Four things about it are worth knowing: the lag is carried unscaled between the two means (item 18, which a move between CPUs exercises and a wakeup on one CPU does not); the walk visits a Fair queue in deadline order, so the first thread that may run here is the one the victim would pick next only for round-robin (the comment in `steal()` says what each queue's order is); under `SCHED_DELAY_DEQUEUE`, which no lane in this record builds, the Fair tree also holds delayed sleepers, parked, suspended and dead threads, which `steal()` skips by taking only Ready ones; and `Fair::dequeue_thread()` calls `update_thread()` on the victim's running thread from the thief's CPU, which charges it against the victim's clock. The first version of `steal()` passed `SkipClock` to the victim's dequeue, so that charge used a clock last read when the victim last scheduled, up to a slice old, and the lag recorded for the stolen thread was understated by what the victim's thread had run since. It does not pass it now, and the dequeue reads the victim's clock under the victim's lock, as `do_unpark_thread()` does for a remote enqueue. (`Scheduler::Migrator::move_queued_thread()` also dequeues remotely with `SkipClock` and has no callers, so `steal()` is the first remote Fair dequeue.) What remains is that `update_thread()` can mark a preemption pending on the victim's thread from the thief's CPU with a non-atomic write (item 33).

**Change**
```cpp
// idle_thread.cpp, before
if (!scheduler.num_runnable()) {
    // halt ...
    continue;
}
// after
if (!scheduler.num_runnable() && !Scheduler::Current::steal()) {
    // halt ...
    continue;
}
```
```cpp
// scheduler.cpp, the shape of steal(); the full text is in the file
auto Scheduler::Current::steal() -> bool {
    SCHEDULE_STEAL_LOOKS.add(1);
    // deepest same-node and deepest other-node candidates, from num_runnable()
    peek_active_mask().for_each([&](CpuNum const cpu) { ... });
    auto *thread = detach_from(same_node_cpu);   // victim's lock only
    if (thread == nullptr) { thread = detach_from(other_node_cpu); }
    if (thread == nullptr) { return false; }
    { Guard<SpinLock, IrqSave> guard{self.get_lock()}; self.activate_thread(thread, EnqueueOptions::None); }
    SCHEDULE_STEALS.add(1);
    return true;                                  // the idle thread reschedules to it
}
```
The walk takes only a Ready thread, checks the victim's depth again under its lock, and lets the dequeue read the victim's clock. Two per-CPU counters, `schedule.steal_looks` and `schedule.steals`, with accessors `num_steal_looks(cpu)` and `num_steals(cpu)` (the shape of `IdleThread::num_halts()`), so a test can tell a CPU that looked and declined from one that was never woken.

Rejected: filling in `steal_work_from()` in the pick (Zircon's place: `Scheduler::DequeueThread()` drops its queue lock around `StealWork()` with `CallUnlocked`), because it does not run at come-back time and needs the own lock dropped in the middle of the pick, a change to the pick that is item 22's to decide; one lock held across both queues, ruled out by the missing lock order; one threshold for every victim, for lack of a reason to send a thread across nodes for one slice.

**Reference**
Zircon `Scheduler::StealWork()` and its caller `Scheduler::DequeueThread()` (`scheduler.cc`, local Fuchsia checkout, read on 2026-11-05): a pull when the CPU would otherwise idle, victims in the order of a search set by cache distance, one victim's lock at a time, and a cross-cluster gate `kInterClusterThreshold` of two milliseconds of queued time. Differences here: the trigger is the idle loop, not the pick; the gate is a count, not queued time; the order is two buckets (this node, other nodes), not a distance. Linux `sched_balance_newidle()` and `sched_balance_rq()` (`kernel/sched/fair.c`; names confirmed, bodies not read). Local: `Scheduler::Current::deactivate()` for the two-phase move; `mm::CpuNodeMap::node_of()` for the node.

**Verification**
- `steal.test` (new) takes a pair of CPUs, a victim that holds the queue and a thief that is idle and halted, once for the highest two CPUs on one node and once for the highest pair on two nodes, whichever the machine has. Per pair, three rounds, each with fresh spinners started pinned to the victim: (1) three that are then allowed on the thief, the thief is woken by a reschedule request, and one must be seen on it and counted by `num_steals()`; (2) two such, taken only if the pair is on one node (from another node, nothing seen on the thief and `num_steals()` unchanged for 300 ms), and the thief's `num_steal_looks()` must have risen, which says it looked; (3) three that stay pinned to the victim, which the thief must look at and take none of. The spinners never sleep or wake, so a spinner on the thief got there by a take, not a placement: `Thread::resume()` honours the pin, and `set_cpu_affinity()` does not move a thread whose current CPU is still allowed (`migrate_thread()` is a stub).
- Default machine (all CPUs on one node, `out/fair-lane`): 2 runs, 15 checks each, ok. Fair lane (`out/fair-on`, `sched_normal_discipline="fair"`): 1 run, 15 checks, ok. Four-node machine (`mm_numa=true`, `NUMA=1`, `out/numa.x86`; CPUs 6 and 5 on one node, 7 and 6 on two): 3 runs, 30 checks each, ok, so one run covers both thresholds. After the tightening described under Change (Ready-only, depth rechecked, victim clock read) the same: 3 runs of 30 checks on the four-node machine, 1 run of 15 on the Fair lane.
- Negative controls on the four-node machine (run on the first version and again on the final code, with the same results), each restored and compared with `cmp`: `steal()` disabled after the look is counted: 5 of 30 fail (the take, the take count and, on one node, the take of the single waiting thread); the affinity test skipped: 3 of 30 fail (the pinned round on one node, and, on two, round 1, where CPU 1, which the spinners are not allowed on, took one: the filter is what keeps it out); the cross-node threshold lowered from 3 to 2: 1 of 30 fails ("and left it, from another node").
- The console tests around the scheduler in one `kshell_run.py` invocation each, with `tickreplace.test` last (see item 34): `preempt idle sleep threadjoin timer timershare mutex.stress hotplug irq interrupt steal tickreplace`, 12 commands, ok on the default lane, the Fair lane and the four-node lane, once each; `ktest all` (`ran=24 passed=23 failed=0 skipped=1`, which includes `WaitQueue.*`, `Semaphore.*` and `Dpc.*`, the tests that replaced the console commands `waitqueue.test`, `semaphore.test` and `dpc.test`) on the same three lanes. `pmnode.test` on the four-node machine passed 3 of 3 after the tightening, and `vmosnapshot.test` and `usercopy.test` on the default lane. **Correction (2026-11-05, the same day):** the first version of this bullet claimed a fourteen-name batch passing nine times on three lanes. It had been run from zsh as `kshell_run.py --out out/<dir> $T`, and zsh does not split `$T`, so the shell was typed `idle.test` with nine arguments and only `idle.test` ran; the log line `run 'idle.test' with 9 argument(s)` showed it. The batch above was rerun with the names split (`${=T}`). Those that were run alone before (`steal.test`, `hotplug.test`, `preempt.test`, `pmnode.test`) were unaffected.
- `boot_check.py`: default lane dbg PASS (46 passed, 5 skipped, 0 failed), rel PASS (44/8/0, 7/7 required), host-tests PASS (173 passed); Fair lane and four-node machine (`NUMA=1`), `--no-tests`: dbg PASS (46/5/0), rel PASS (44/8/0, 7/7).
- `scripts/format_cpp.py --check` and `scripts/format_gn.py --check` are clean; clang-tidy (`-p out/fair-lane`, header filter on `scheduler.hpp`) on `scheduler.cpp`, `idle_thread.cpp` and `steal_test.cpp` reports nothing on a line of this change; what it reports is old (`idle_thread.cpp:156` and `:161`, the lambdas' names, and `scheduler.cpp:457`, the stub's `return 0;`).
- Not measured: any benefit. QEMU TCG has no remote-memory latency, so no run can show the node preference paying off, and the thresholds are untested against data (item 24). Not observed: the 3, 3, 2, 0 split of `hotplug.test` becoming 2, 3, 2, 1 after the CPU returns; `hotplug.test` passes but does not print the counts. Not run: `SCHED_DELAY_DEQUEUE`, the narrowed-affinity window and the priority-inheritance window (items 25 and 26).
- `waitqueue.test` does not hang: the command is gone. `waitqueue.test`, `dpc.test`, `semaphore.test` and `tickbcast.test` were migrated to ktests (`f44d9634`) before this work, and `kshell_run.py` waits for a verdict line a missing command cannot print (repair item 3 of `docs/repairs/261105/kernel-test-framework.md`). The earlier claim in this entry that it hangs alone and passes in a batch was wrong, both halves: the batch ran only its first name (above).
- Two rounds of independent read-only review. Round 1 (three lenses: concurrency, test power and claims, conventions) lost its concurrency lens to a usage limit; the other two found: the first draft's pinned-thread check could pass without the filter (it depended on which thread was at the head: replaced by round 3), the "not taken" round had no proof the thief looked (the counters), the Migrating-window text overstated (rewritten, items 25 and 26), "Fair needs no edit" and "waiting the longest" overstated (above, and the comment), the header comment for `steal()` and the change title wrong about the busiest CPU (fixed), three stale comments elsewhere (fixed), a global array shared by rounds, two ternaries, a `[[nodiscard]]`, the constants' names, a Linux function named wrongly and the "3, 3, 2, 1" count (all fixed). Round 2 re-ran the concurrency lens and audited every caller of `CpuLocal::access()`. The concurrency lens argued these safe: the context-switch hand-off (the thief spins on the thread's PI lock until the victim has saved its stack), the victim going offline mid-steal, wakeups, kills and joins in the Migrating window, and the idle loop's halt after a take. What it found was fixed here (non-Ready threads in a Fair tree; the stale clock; the depth not rechecked under the lock) or recorded: the affinity window (item 25, certain), the priority-inheritance window (item 26, certain, latent), the non-atomic preemption write (item 33, speculative), one deep queue of pinned threads hiding a shallower queue that has a thread to give (item 23). The audit found every one-argument `access()` caller non-preemptible, and four unsafe derived uses, none of them changed here: `Timer::cancel()` (item 28, a stack frame freed under a running handler), the page-fault assertion (item 29, a release-image panic), `DpcRunner::enqueue()` and `ChainLockTransaction::current()` (items 30 and 31, latent), and `pmnode.test` (item 32).

**Deliberately not done**
- A balance among busy CPUs and a pull at the pick (item 22), a kick for a halted idle CPU (item 23): a split like 2, 3, 2, 1 stays.
- The thresholds' data (item 24), and a queued-time gate in place of the count.
- The affinity and priority-inheritance windows (items 25 and 26), and the four uses of a stale CPU number that the audit found (items 28 to 32): `Timer::cancel()` and the page-fault assertion are in other subsystems and are one-line guards each, and they are reachable now that a preempted thread can move (item 28 is memory-unsafe in a window of a few instructions); they are for their own commits and sessions, and this change does not make them wait or fix them.
- More than one thread per attempt: the idle thread reschedules to what it got, and if the victim still has two waiting the CPU is no longer idle, so it does not look again until it is.
- Deleting `steal_work_from()`: it is scaffolding for a pull at the pick, now with a comment saying so.
- Fair's carried lag (item 18) and the preemption-state write of its `update_thread()` (item 33): the user's code.
- A known rare flake in `steal.test`: the victim's count must be exactly two when the thief looks in round 2 on two nodes, and any other runnable thread on the victim at that instant (a worker pinned there) makes it three; the window is microseconds, and the final test's runs above (7 on the four-node machine, 3 on the default lane, 3 on the Fair lane, the batches included) have not shown it.

**How it was found**
The user chose work stealing (item 21) from the repairs list, then argued for the idle thread as the place ("那我们在IdleThread里面偷吧") after the first design question (idle loop or pick) exposed that `come_back()` skips the pick, and asked why the cluster was not considered ("为什么不考虑CPU簇亲和性问题"), which led to the node-first order and to `CpuNodeMap`, which my first answer had wrongly said did not exist. The two rounds of independent review found most of the defects listed under Verification.

**The user's prediction**
None (predictions are paused)

**Three questions you should be able to answer**
- Why does a CPU that comes back online not reach the stub that was in `pick_next_thread()`, and what in the idle loop does reach it?
- Why does `steal()` hold the victim's lock and its own lock one after the other and never both, and what is the thread in between, and who can still reach it there?
- Why is the cross-node threshold higher than the same-node one, and what would you have to measure to choose the numbers?
