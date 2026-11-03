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
