# kernel-time changes

## Title: `Timer::cancel()` reads the CPU number with interrupts off, in the same stretch as the comparison that uses it (2026-11-05)

**Files**
- `kernel/kernel/time/timer.cpp` — `Timer::cancel()` no longer reads `Cpu::id()` at its top; the "cancelling from inside our own handler" test reads it under an `arch::AutoInterruptDisabler`, right where it compares
- `docs/repairs/261103/kernel-task-sched.md` — repair item 28 deleted (number kept, the others are not renumbered)

**Symptom**
Latent, found by review and not run. A thread leaving `Thread::Current::sleep_etc()` cancels its stack `Timer` with preemption and interrupts on. If it is preempted on CPU A after `cancel()` has read the CPU number, taken by an idle CPU B (`Scheduler::Current::steal()`, commit c03894ea), and resumes in `cancel()` while A's handler for that same timer is running, then `cancel()` returns at once instead of waiting. `sleep_etc()` returns, its frame dies, and A's handler goes on using the frame and finally stores `INVALID_CPU_NUM` into it. A debug image trips the assertion in `~Timer()`; a release image corrupts the stack. The window is a few instructions wide and needs a preempt, a steal and a timer expiry inside it.

**Cause**
`cancel()` has to tell two situations apart that look alike from outside. In one the caller is the timer's own handler (it called `cancel()` from `on_expired`, and may not wait for itself). In the other the handler is running on some other CPU and the caller must spin until it is done, because the caller is about to free the timer and everything the handler touches. The test is `m_active_cpu == this CPU`: `m_active_cpu` is the CPU whose queue is running the handler, so if it names the CPU we are on, we are that handler.

That reasoning has a hidden premise: "the CPU we are on" has to be true at the instant of the comparison. `Cpu::id()` answers where the thread is at the moment it is called, and nothing keeps the thread there afterwards unless interrupts or preemption are off. The old code read it first thing in the function, then stored `m_cancelled`, then compared. Between the read and the compare a thread with interrupts on can be preempted, and before `steal()` that was harmless: a preempted thread went back on the run queue of the CPU it left and resumed there, and a handler on that CPU cannot run while the thread is running, so the number it had read was still the number of the CPU it was on. `steal()` removed the premise. An idle CPU now takes a queued thread, preempted or not, so the thread can resume on B holding "A" in a register, while A's tick runs the handler of this very timer (the sleeper's own timeout handler, which wakes the sleeper and so is the reason the sleeper is running at all). `m_active_cpu == A` is then true, but for the wrong reason: it says "the handler is on A", and the stale `cpu` says "I am on A", and the two facts are about different instants. The wrong answer is the unsafe one: "this is my own handler" means skip the wait, and the wait was the only thing keeping the stack frame alive.

The fact that makes the test sound is an exclusion, not an ordering: with interrupts off on a CPU, the only code that can be running on it while a timer's `m_active_cpu` names it is the handler (the handler runs inside the tick, with interrupts off, from `pop_expired_locked()` setting the field until `run_expired_timers()` clears it). So if interrupts are off, the CPU number is stable, and equality with `m_active_cpu` can only mean "this is the handler", never "some thread that happens to be on the same CPU".

`TimerQueue::activate_timer()` has the same test for the same reason (re-arming from the handler versus from elsewhere) and already reads the CPU under an `AutoInterruptDisabler`, with a comment naming the hazard. `cancel()` had its own `AutoInterruptDisabler` further down, around the unlink, but that scope began after the comparison had been made.

**Change**
Before:

```cpp
auto Timer::cancel() -> bool {
    m_canary.verify<Self>();

    auto const cpu = Cpu::id();

    m_cancelled.store(true, std::memory_order_release);

    if (m_active_cpu.load(std::memory_order_acquire) == cpu) {
        m_on_expired = nullptr;
        return false;
    }
    ...
```

After:

```cpp
auto Timer::cancel() -> bool {
    m_canary.verify<Self>();

    m_cancelled.store(true, std::memory_order_release);

    {
        arch::AutoInterruptDisabler const no_interrupts;
        if (m_active_cpu.load(std::memory_order_acquire) == Cpu::id()) {
            m_on_expired = nullptr;
            return false;
        }
    }
    ...
```

The `return false` inside the scope is fine: `AutoInterruptDisabler` restores the saved interrupt state, and inside a handler that state is "off", so nothing is enabled early. After the scope the thread may move, and nothing below depends on where it is: the unlink block takes its own `AutoInterruptDisabler`, and the spin on `m_active_cpu != INVALID_CPU_NUM` is CPU-independent. Not moving `m_cancelled.store` after the scope: it must stay the first thing `cancel()` does, before any lock, because a handler spinning in `trylock_or_cancel()` is waiting to see it.

Rejected: merging this scope with the one around the unlink. That would hold interrupts off across `with_queue_locked()` and the spin on the handler, and the spin lasts as long as the handler runs, keeping this CPU's own tick and IPIs off for that long. Two short scopes cost two irq save/restore pairs on a path that is not hot.

**Reference**
None, this project's own design. The same-file model is `TimerQueue::activate_timer()` in `kernel/kernel/time/timer_queue.cpp`, which reads `Cpu::id()` under `arch::AutoInterruptDisabler` before comparing it with `m_active_cpu`.

**Verification**
- Built in `out/timer-cancel` (new directory); the working tree also held other sessions' uncommitted edits to `kernel/commands/pm_node_test.cpp` and `kernel/mm/vm_aspace.cpp`, which are not part of this commit.
- `clang-tidy -p out/timer-cancel kernel/kernel/time/timer.cpp`: no diagnostic in `timer.cpp`; the ones printed are in `ustl/bitfields.hpp`, which this change did not touch.
- `scripts/kshell_run.py --out out/timer-cancel timer.test timershare.test sleep.test tickreplace.test`: `timer.test` ok (18 checks), `timershare.test` ok (3 runs, the thread never kept under 25.0 %), `sleep.test` ok (16 checks), `tickreplace.test` ok (27 checks).
- `boot_check.py --out out/timer-cancel`: dbg PASS (46 passed, 5 skipped, 0 failed), rel PASS (44 passed, 8 skipped, 0 failed, 7/7 required), host tests 173 passed.
- The race itself is not reproduced: closing it needs a preempt, a steal and an expiry in a window of a few instructions, and no test here can place them. Not measured.

**Deliberately not done**
- A test that forces the interleaving. It would need a hook between the CPU read and the comparison, and with the read now inside the comparison's own scope there is no such gap left to hook.
- The other uses of a CPU number read early that the same review found (repair items 29 to 31): they are in other subsystems and have their own items.
- `Timer::cancel()` from a thread with preemption off is unchanged: it already could not move.

**How it was found**
By a two-round independent review of commit c03894ea, reading every place that takes `Cpu::id()` early and keeps it, with the question "what if the thread is on another CPU by the time this is used". This item came from reading `cancel()` next to `sleep_etc()` (`kernel/kernel/task/thread.cpp`, where the stack timer is cancelled after the PI guard is released with preemption on). Here the diagnosis was re-checked against `pop_expired_locked()` (where `m_active_cpu` is set, under the queue lock, interrupts off) and `run_expired_timers()` (where it is cleared after the handler returns) before editing.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is "`m_active_cpu` equals the CPU I am on" only a proof that I am the handler when interrupts are off on that CPU, and what else could be running on that CPU with `m_active_cpu` set if they were on?
2. Why was the old code safe before `Scheduler::Current::steal()` existed, and which sentence of that argument stopped being true?
3. Why does the fix use a small scope for the comparison and keep a separate one around the unlink, instead of one scope over the whole function?
