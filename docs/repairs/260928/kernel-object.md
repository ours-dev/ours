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

## 2. A start that fails after the kernel thread was made leaves a thread that can neither run nor be retried

**Severity**
Low, latent: `ThreadDispatcher::activate()` answers Ok on every path today, so nothing reaches it.

**Symptom**
Latent, cannot be triggered today. `ProcessDispatcher::attach_thread()` calls `create_kernel_thread()` and then `activate()`; if `activate()` ever answered an error, `attach_thread()` would return it with the kernel thread already made and registered with the scheduler (Initial, on the global thread list, holding a kernel stack). The dispatcher would still be Initial but now has `m_kernel_thread` set, so a retried start answers BadState from `create_kernel_thread()`, and nothing disposes of the kernel thread: it has no `Thread::Forget()` to be freed with.

**Cause**
`create_kernel_thread()` is the one step of a start that has to be undone by disposing something, and it is not the last fallible step: `activate()` and the list insertion come after it. This is the price of making the kernel thread at start (K23a): the check-then-create order made a refused or out-of-memory start clean, but a failure after creation is not.

**Change**
Options, with the reasoning to be put to the user before any is built:
- Make `activate()` infallible by contract (it returns void, or asserts) and remove the error path from `attach_thread()`. The smallest change, and honest as long as it is true: resuming a fresh thread cannot fail.
- Order the steps so that creating the kernel thread is the last thing that can fail, with `activate()` and the list insertion moved before it or made unable to fail.
- Give the kernel a way to dispose of a kernel thread that never ran (Zircon `Thread::Forget()`), then undo the creation on the error path. The most general and the largest: it is also what would allow creating the kernel thread outside the process lock (item 1).

**Reference**
Zircon `Thread::Forget()` (disposes of a thread that was created and never resumed) and `ThreadDispatcher::MakeRunnable()`, which makes the thread runnable last.

**Acceptance**
A start whose `activate()` is made to fail in a test leaves no kernel thread on the global list and the dispatcher startable again; or, if the first option is taken, `activate()`'s signature says it cannot fail and no error path is left to be wrong.
