# kernel-object outstanding repairs

Performance items, per "Working with the user" in CLAUDE.md: to be done after the features settle, and discussed with the user one at a time, with numbers.

## 1. Everything a thread start does runs inside the process lock; measure how long that is held

**Severity**
Low, nothing is known to wait on it today.

**Symptom**
Latent, cannot be triggered today. `ProcessDispatcher::attach_thread()` holds the process lock (a sleeping mutex) across the state check, `ThreadDispatcher::create_kernel_thread()` (which allocates and maps a kernel stack, registers the thread with the scheduler) and `activate()`. What the lock covers -- state, thread list, return code, suspend count -- is one lifecycle invariant and small; how long it is held is the part that grew when the kernel thread moved from thread creation to start (K23a). It would matter if many threads of one process start at once (an application spawning worker threads) or if `kill()`/`exit()` of a process with a thread mid-start had to wait for a stack allocation.

**Cause**
The user chose the simplest version first, "one lock over everything", and the measurement second. The design was argued on 2026-09-28: a lock-free state word with CAS for the edges that touch only the state (Initial -> Dead) and a lock for the edges that touch the thread list (Starting -> Running, Running -> Dying, Dying -> Dead) works if Initial is the only state left by CAS; it also allows the heavy work (creating the kernel thread) to happen outside the lock, between CAS(Initial -> Starting) and a short critical section.

**Change**
Measure first, on a machine that gives trustworthy timings (TCG on an arm64 host does not: no cache model, no PMU, `rdtsc` includes host scheduling; counts such as contention are usable, times are not):
- how long `create_kernel_thread()` takes against the rest of the critical section;
- with N threads of one process started concurrently, the wait time on the process lock, in a log2 histogram;
- whether `kill()` of a process with a thread mid-start waits, and for how long.
Then decide, with the numbers: keep it; or add a Starting state and CAS(Initial -> Starting) so the kernel thread is made outside the lock, which needs a rule for `kill()` seeing Starting (a "kill requested" flag that start checks when it finishes, or `kill()` waiting) and a way to dispose of a kernel thread that was made and not used (Zircon's `Thread::Forget()`, which this kernel does not have). Each candidate's invariant and lock order must be written down before it is built; measurement cannot show that a race is absent.

**Reference**
Zircon `ProcessDispatcher::AddInitializedThread()` and `ThreadDispatcher::MakeRunnable()` (one process lock over the state check, the list and making the thread runnable); Zircon `Thread::Forget()`.

**Acceptance**
A benchmark under `benches/` with the numbers and the machine and accelerator it ran on, and a decision recorded in `docs/changes/`. If the split is built: a stress test starting and killing threads concurrently that passes under lockdep.
