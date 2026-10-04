# kernel-task changes

## Thread::Current::tid() reads the thread through get() instead of a CPU_LOCAL handle it made up (2026-10-04)

**Files**
- `kernel/include/ours/task/thread.hpp` — `Thread::Current::tid()` returns `Thread::Current::get()->tid()`

**Symptom**
Every CPU took a kernel page fault in its first profiler sample once the sampler ran from a timer: `page fault at 0xffffffffbff1c088 from a context that may not sleep`, at `rip=+0x14b65`, which is `prof::arch::capture_sample_context()`. The fault address differs per CPU (`...bff1c088`, `...bff33088`, `...bff05088`, `...bfeee088`) and always ends in `088`.

**Cause**
`CpuLocal::read(t)` takes a `CPU_LOCAL` variable and reads this CPU's copy of it. On x86 that is one instruction, `mov %gs:&t`: %gs holds this CPU's offset from the linker template, and the displacement is the variable's own address in the template, so the two add up to this CPU's copy (`cpu_local.hpp`, the comment above `read()`).

The old body was `mm::CpuLocal::read(CURRENT_INSTANCE->m_tid)`. The argument is not a CPU_LOCAL variable. To form it, the compiler first loads `CURRENT_INSTANCE` the ordinary way, from the template copy, which nothing ever writes and is a null pointer. `->m_tid` then names address `0 + offsetof(Thread, m_tid)`, which is 0x88, and `read()` treats that as a variable's template address: `mov %gs:0x88`. What it reads is this CPU's offset plus 0x88, an address that holds nothing, hence a fault whose address is "a per-CPU offset, plus 0x88" on every CPU.

The mistake is applying a CPU_LOCAL accessor to a field of the object a CPU_LOCAL pointer points at. Only the pointer is per-CPU; the thread is ordinary memory, reached by reading the pointer first. Every other accessor in `Thread::Current` already does that (`get()->memory_allocation_state()` and the rest).

Nothing noticed because nothing called it outside the profiler, and the profiler only reached it from a sampling interrupt, which until now meant the PMU, and QEMU TCG has no PMU.

**Change**
Before:

```cpp
static auto tid() -> KoId {
    return mm::CpuLocal::read(CURRENT_INSTANCE->m_tid);
}
```

After:

```cpp
static auto tid() -> KoId {
    return Thread::Current::get()->tid();
}
```

**Reference**
Zircon `Thread::Current::Get()` returns the current thread and every per-thread query goes through it (`zircon/kernel/include/kernel/thread.h`); Linux `current->pid` likewise reads the per-CPU `current_task` pointer and then the field. Linux's `this_cpu_read()` has the same rule as `CpuLocal::read()`: its argument must be a per-CPU variable, never an expression through one.

**Verification**
`boot_check.py --out out/sampler`: dbg and rel PASS. `SMP=1 kshell_run.py --image dbg "prof.start timer 100000" "prof.stop" ...`: samples taken on every start, no fault. Before the change the same command faulted on all four CPUs.

**Deliberately not done**
None.

**How it was found**
The fault's `rip` went through `llvm-objdump -d` on `out/sampler/kernel_x86.dbg.1/obj/kernel/products/bin/vmours` around `0x14b65`: `movq %gs:0x88, %rdi`, inside `capture_sample_context()`, right after it read the interrupt frame. A `%gs` displacement as small as 0x88 cannot be a variable's address in the template, which sits at the top of the kernel's address space, so the handle had been computed from a null pointer; `0x88` is `m_tid`'s offset.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does `CpuLocal::read()` on x86 not need to subtract anything from the variable's address, and what does %gs hold?
- Why does the faulting address end in the same three digits on every CPU while its upper digits differ?
- Which part of `Thread::Current` is per-CPU, and which part is ordinary memory?

## Title: A joinable thread's exit signals TaskState::exited() after its last switch, and detach() refuses a claimed thread (2026-10-04)

**Files**
- `kernel/kernel/task/thread.cpp` — `exit()` of a joinable thread queues a Dpc that signals `TaskState::exited()` instead of panicking; `detach()` refuses a thread a join has claimed and reaps a Dead one outside its PI lock; `TaskState::join()` and `try_wake_joiners()` are gone; `join()` stays a stub with its outline.
- `kernel/include/ours/task/thread.hpp` — `TaskState` holds a `sync::Event` `m_exited` in place of `m_result_wait_queue`, and `invoke()` no longer wakes anyone; `Flags::Joined`; the contracts of `join()` and `detach()`.
- `kernel/include/ours/sync/event.hpp` — `wait(deadline, Interruptible)`, so a join can wait out a deadline uninterruptibly.
- `kernel/kernel/task/dpc.cpp`, `kernel/top/main.cpp`, `kernel/lib/kshell/kshell/shell.cpp` — the comment on why a just-spawned thread's `detach()` cannot fail names the new refusal too.
- `.agents/tasks/thread-join/DESIGN.md` — the detach decision.

**Symptom**
Before: a thread that exited without being detached stopped the kernel with "exited joinable, and join() is not supported". `TaskState::invoke()` woke the joiners' queue the moment the entry point returned, before the thread was even Dead. `detach()` of a Dead thread called `join()` with its PI lock still held.

**Cause**
A join has two facts to wait for, and they come at different times. The thread is Dead as soon as `exit()` sets the state, but it keeps running on its own stack until its final reschedule completes, and only after that may anyone free the stack. Zircon makes the joiner wait for the second by having `exit()` wake the joiners while still holding the thread's lock into the final reschedule: the joiner cannot finish until it gets that lock, and the next thread releases it only after the switch. The cost is that the wake takes each joiner's lock inside the dying thread's: two thread locks nested, which Zircon's ChainLock backs off from when they would deadlock. This kernel has no ChainLock yet and nothing that orders two PI locks, so copying that wake would add an unordered nesting.

Option B of the design moves the wake to a place where nothing is held: a Dpc that `exit()` queues on its own CPU. The Dpc's worker is pinned to that CPU, and the dying thread keeps preemption off until its final switch, so the Dpc runs only once the thread is off its stack. It takes and drops the thread's PI lock, which the next thread released in `complete_context_switch()`, and asserts the thread is Dead. Then it signals a manual-reset `sync::Event` in `TaskState`. That event, not the Dead state, is what tells a reaper it may free the thread. `Event::wait_etc()` already makes "is it signalled?" and the enqueue one step under the event's queue lock, so a join that arrives after the signal returns at once, and one that arrives before is woken by it.

`detach()` and `join()` decide who frees the thread. `Flags::Joined` is the claim of the one joiner, and `Detached` means the thread frees itself; the two are never set together. Zircon's `Detach()` wakes a waiting joiner with `ZX_ERR_BAD_STATE`. Here the joiner's event belongs to a thread that, once detached, frees itself, and a joiner whose wait times out has to hand its claim back under the thread's PI lock, which would race the thread freeing itself. So `detach()` of a claimed thread answers `BadState`.

**Change**

```cpp
// before (exit, joinable branch)
panic("thread |{}| exited joinable, and join() is not supported: detach() it", ...);

// after
joinable = true;   // stays on SPAWNED_THREADS; no free Dpc
...
auto func = Dpc::Func{[](Dpc *dpc) -> void { dpc->arg<Thread>()->destroy(); }};
if (joinable) {
    func = [](Dpc *dpc) -> void {
        auto *const thread = dpc->arg<Thread>();
        {
            Guard<SpinLock, lockdep::IrqSave> guard{thread->get_pi_lock()};
            DEBUG_ASSERT(thread->state() == State::Dead);
        }
        thread->m_task_state.exited().signal(Status::Ok);   // last touch
    };
}
```

```cpp
// detach(), after
{
    Guard<SpinLock, lockdep::IrqSave> guard{get_pi_lock()};
    if (!!(m_flags & Flags::Joined)) { return Status::BadState; }
    if (state() != State::Dead) { m_flags |= Flags::Detached; return Status::Ok; }
}
return join(nullptr, time::Deadline::infinite());   // not under the PI lock any more
```

`join()` itself is left as a stub that answers `NotSupported`: it is the milestone's hand-written function, and the next change brings its failing test. Signalling the event is the Dpc's last touch of the thread, and `SpinLock::release()`'s last access to the lock is its releasing store, so a reaper may free the thread, and the Dpc's storage on its stack, as soon as it is woken.

**Reference**
Zircon `Thread::Join()`, `Thread::Detach()` and the joinable branch of `Thread::Current::Exit()` (zircon/kernel/kernel/thread.cc:774, :858, :1050-1104), whose TODO prefers dropping the thread's lock to wake joiners unconditionally, with a cleanup lock guarding the stack; the exit Dpc plays that role here. Zircon's free Dpc for a detached thread (`thread.cc:1007-1043`) relies on the same pinning argument.

**Verification**
- Isolated copy of HEAD with this change: `boot_check.py --out out/task-wake`, dbg PASS (20 passed), rel PASS (18 passed), 0 error lines; `waitqueue.test` and `sleep.test` still ok.
- `threadjoin.test` (next change) against this change with the stub `join()`: every check that joins fails with `NotSupported`, as it should; five joinable threads exit through the new Dpc without a panic or an assertion.
- The same test with a reference `join()` written only in that scratch copy, never committed: `ok -- 11 checks`, and `threadjoin.test stress 300`: 300 rounds x 4 threads in 52912 ms, 0 bad. This checks the exit side and the test, not the user's join.
- Not with lockdep: a `lockdep_enable = true` build does not link (`lockdep::current_lock_context()` and `report_violation()` undefined), which is `docs/repairs/260917/kernel-task-sched.md` item 6, a feature of its own.
- clang-tidy: no finding on the changed lines.

**Deliberately not done**
- `join()`'s body: the user's hand-written function (`docs/authored.md`).
- The lockdep hooks: repair item above.
- A count of joins (`THREAD_JOIN_COUNT` exists and is unused); it belongs with `join()`.

**How it was found**
None: a task (`.agents/tasks/thread-join/`), handed over by another session on 2026-10-03.

**The user's prediction**
The user chose option B and to refuse a detach while a join holds the claim (2026-10-04). The written argument (invariant, who races, worst interleaving) is still to come, as the comment above the hand-written `join()`.

**Three questions you should be able to answer**
- Why is "the thread is Dead" not enough for a joiner to free it, and what does the exit Dpc know that the Dead state does not say?
- Why can the exit Dpc not run before the dying thread's final switch, and what would break if its worker were not pinned to that CPU?
- Why must `detach()` refuse a thread a join has claimed here, when Zircon's wakes the joiner instead?

## Title: Thread::join() claims the thread, waits for TaskState::exited(), and reaps it (2026-10-04)

**Files**
- `kernel/kernel/task/thread.cpp` — `join()`'s body, in place of the stub that answered `NotSupported` since `d5a7477e`.
- `kernel/include/ours/task/thread.hpp`, `kernel/commands/thread_join_test.cpp` — the comments that called `join()` unwritten.
- `docs/authored.md` — the exercise's row is withdrawn: the user asked for the function to be written for them.
- `.agents/tasks/thread-join/DESIGN.md` — who wrote it; the design is accepted.

**Symptom**
`join()` answered `NotSupported`, so no thread could be joined, and `detach()` of a thread that had already exited (which reaps through `join()`) failed the same way. `threadjoin.test` failed 8 of its 9 checks.

**Cause**
Not a defect: the function was left out on purpose as the milestone's hand-written exercise, and is written here at the user's request. What it has to get right is who may touch the joined thread, and when. The thread frees nothing of itself while joinable, so the joiner owns its memory, but only from the moment the thread is off its stack, which the exit Dpc announces through `TaskState::exited()`. Two joiners, or a joiner racing a detach, would both believe they owned it; the `Joined` claim makes exactly one of them the owner.

**Change**

```cpp
{
    Guard<SpinLock, lockdep::IrqSave> guard{get_pi_lock()};
    if (!!(m_flags & (Flags::Detached | Flags::Joined))) {
        return Status::BadState;
    }
    m_flags |= Flags::Joined;                      // the claim
}
auto const status = m_task_state.exited().wait(deadline, Interruptible::No);
if (Status::Ok != status) {
    Guard<SpinLock, lockdep::IrqSave> guard{get_pi_lock()};
    m_flags &= ~Flags::Joined;                     // give it back
    return status;
}
if (retcode != nullptr) {
    *retcode = m_task_state.retcode();
}
erase_from_global_list();
destroy();
```

No step holds two threads' PI locks: the claim and its give-back take this thread's, the wait takes only the caller's (inside `Event::wait_etc()`), and `destroy()` takes this thread's again once nobody else can. Touching the thread after the wait is safe on both branches because of the claim: nobody else frees a claimed thread. After a timeout the thread may have exited in the meantime; it then stays Dead and joinable for the next `join()` or `detach()`. `THREAD_JOIN_COUNT`, defined and never counted, now counts joins.

**Reference**
Zircon `Thread::Join()` (zircon/kernel/kernel/thread.cc:774): the same three outcomes (`BAD_STATE` for a detached thread, the wait's error, reap and free), with a chain-lock transaction over the list lock, the thread's lock and the caller's where this has the claim and the exit event; `EraseFromListsLocked()` and `free_thread_resources()` are `erase_from_global_list()` and `destroy()` here.

**Verification**
- Isolated copy of HEAD with this change: `boot_check.py --out out/task-wake`, dbg PASS (20 passed), rel PASS (18 passed), 0 error lines.
- `threadjoin.test` on dbg: `ok -- 11 checks`. `threadjoin.test stress 300` at the default 4 CPUs: 300 rounds x 4 threads in 51076 ms, 0 bad; at `SMP=8`, `stress 200`: 200 rounds x 8 threads in 44811 ms, 0 bad. `waitqueue.test` and `sleep.test` still ok.
- Not with lockdep, which cannot be linked in yet (`docs/repairs/260917/kernel-task-sched.md` item 6). Not on rel, which has no kernel shell.
- clang-tidy: no finding on the changed lines.

**Deliberately not done**
- A user-visible join: user threads stay detached; `ThreadDispatcher` and process teardown own them.
- Waking a joiner from `detach()`, as Zircon does: refused instead (the decision in the design).

**How it was found**
None: the task's last step.

**The user's prediction**
None. The user chose option B and the detach rule, and asked for the function to be written for them rather than write it, and the lock-order argument, by hand.

**Three questions you should be able to answer**
- After a join's wait times out, why may it still touch the thread to give its claim back?
- Why does a join that comes after the thread has fully exited return at once, whatever its deadline?
- Which two threads' PI locks would Zircon's `Join()` hold at once, and what does this one hold instead at each step?
