# kernel-sync outstanding repairs

## 1. Semaphore::post() and wait() take the wait queue's lock even when no thread has to block

**Severity**
Low: a performance observation, not measured. Nothing is wrong.

**Symptom**
Latent, cannot be triggered today. Since step 3 of `.agents/tasks/microkernel-interrupts/PLAN.md`, every `port_queue`, every `object_wait_async` match and every `port_wait` goes through `sync::Semaphore`:
- a post with nobody waiting takes the wait queue's spin lock, with interrupts off, only to add one to the count;
- a wait with a unit banked takes the calling thread's PI lock (interrupts and preemption off) and then the queue lock, only to take one away.

From step 4 an interrupt object posts its port from the interrupt handler, so the queue lock is taken in IRQ context on every interrupt. A port with waiters on several CPUs and a busy interrupt contends on it.

**Cause**
`sync::Semaphore` (`kernel/kernel/sync/semaphore.cpp`) keeps its count under the wait queue's lock and never lets it go negative. That makes its invariant, "a thread waits only while the count is zero", easy to argue: a post either hands its unit to the first waiter or adds it to the count, both under one lock, as Linux's `up()` does. The price is the lock on every call, whether or not anyone blocks.

`wait_etc()` also takes the PI lock before it knows it will block. The order is PI lock outside, queue lock inside, and it must hold the queue lock from the count check through the enqueue, so it cannot take the PI lock only once it has found the count at zero without dropping and retaking the queue lock.

**Change**
Measure first, as CLAUDE.md asks of any performance work: a benchmark in `benches/` that posts and waits from one to N CPUs, with and without blocked waiters, and records the time per post and per wait, the queue lock's hold time and its contention rate. Decide with the user from the numbers. Candidates:
- Zircon's: an atomic count that `wait()` decrements below zero before it blocks. A post on a non-negative count and a wait on a positive one are a compare-and-swap each, with no lock, and the queue lock is taken only to cross zero. The cost is a subtler invariant: a negative count does not mean a thread is queued, because a timed-out or interrupted waiter leaves the count negative, and the next post has to repair it. Zircon spells out why the waiter cannot undo its own decrement (`zircon/kernel/kernel/semaphore.cc`, the comment above `Semaphore::WaitInternal()`).
- A fast path in `wait()` alone: a compare-and-swap decrement of a positive count before any lock, with `post()` unchanged. It saves the PI lock on the common "a packet is already queued" path. The count then needs to be atomic, and every write to it still has to happen under the queue lock or by compare-and-swap.

Rejected: none yet; the choice waits for the numbers.

**Reference**
- Zircon `Semaphore::Post()` and `Semaphore::WaitInternal()` in `zircon/kernel/kernel/semaphore.cc`: the atomic fast paths.
- Linux `up()` and `__down_common()` in `linux/kernel/locking/semaphore.c`: the same shape as ours, with every call under `raw_spin_lock_irqsave(&sem->lock)` and the unit handed straight to the first waiter.

**Acceptance**
The benchmark's numbers are in the change record, the user has chosen, and `semaphore.test` and `port.test` pass on the dbg image (`python3 scripts/kshell_run.py --out out/bringup.x86 semaphore.test port.test`), along with the shell's port checks on both images.
