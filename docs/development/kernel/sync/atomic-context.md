# Atomic context: when kernel code may not sleep

"Sleeping" means handing this CPU to another thread for an unbounded time,
and possibly resuming on a different CPU afterwards. Code that has made a
promise which contradicts that is said to run in **atomic context**, and must
not sleep: no `sync::Mutex`, no allocation that may reclaim, no wait on an
event.

The rule is often stated as "kernel code cannot sleep". That is wrong. A
system call runs in the kernel and sleeps all the time. What forbids sleeping
is not *where* the code runs but *what it has promised*, and there are four
such promises.

## The four conditions

Each is described by the same three questions: what it promises, whom it
guards against, and why sleeping breaks it.

### 1. Holding a spin lock

- **Promise:** I am inside a critical section and will leave it quickly.
- **Guards against:** execution on other CPUs that wants the same critical
  section. Those CPUs busy-wait for it; they do not give up their CPU.
- **Why sleeping breaks it:** busy-waiting is only a sane strategy because the
  holder never sleeps. If the holder sleeps, every waiter spins for an
  unbounded time. Worse, if a waiter is spinning on the very CPU the holder
  needs in order to come back and release the lock, neither makes progress:
  a deadlock.

### 2. Running an interrupt handler

This one differs in kind from the other three. It is not a switch someone
turned on; it is *who you are*.

- **Promise:** I am borrowing the interrupted thread's identity and stack to
  do something urgent, and I will hand them back as soon as I am done.
- **Why sleeping breaks it:** an interrupt handler has no thread of its own.
  The scheduler can only suspend and resume threads, so "sleep" can only be
  recorded against the thread that was interrupted -- which is the wrong
  thread. That is not merely a bookkeeping error. The handler's frame sits on
  top of the interrupted thread's kernel stack, so that thread cannot resume
  until the handler returns. See the next section for why this deadlocks.

### 3. Interrupts disabled

- **Promise:** nothing else runs on this CPU right now -- not another thread,
  and not an interrupt handler either.
- **Guards against:** everything on this CPU, including interrupts and the
  timer-driven preemption they deliver. It does nothing about other CPUs.
- **Why sleeping breaks it:** sleeping lets another thread run on this CPU,
  which is exactly what was promised not to happen. And almost everything that
  would wake a sleeper -- a timer expiring, an IPI, a device completion -- is
  delivered by an interrupt, which is disabled. Nothing could wake it.

### 4. Preemption disabled

- **Promise:** I will not be switched off this CPU, and no other thread will
  run on it until I say so. Interrupts are still handled.
- **Guards against:** other threads on this CPU, and migration to another CPU.
  It does not guard against interrupt handlers, nor against other CPUs.
- **Why sleeping breaks it:** sleeping *is* giving up the CPU, which
  contradicts "do not switch me out". Code usually disables preemption because
  it depends on staying put -- for instance it is working on this CPU's
  per-CPU data. After a sleep it may resume on another CPU, holding a pointer
  to someone else's copy. Linux reports this as
  `BUG: scheduling while atomic`.

## Who each protection keeps out

| | Other threads on this CPU | Interrupts on this CPU | Other CPUs |
|---|---|---|---|
| Preemption disabled | yes | no | no |
| Interrupts disabled | yes | yes | no |
| Spin lock | -- | -- | yes |

Which one to use depends on who else touches the data. Per-CPU data that only
threads touch needs preemption off. If an interrupt handler also writes it,
preemption off is not enough -- the handler can still arrive in the middle --
and interrupts must be off as well. Data shared across CPUs needs a lock.
In practice the protections are combined: Linux's `spin_lock_irqsave()` is a
spin lock plus interrupts off, which keeps out all three directions at once.

## Why sleeping in an interrupt handler deadlocks

The handler (call it B) runs on the stack of the interrupted thread (A), with
B's frame on top. A cannot continue until B returns. So the moment B waits for
something A has to do, the wait is circular. Three concrete shapes:

1. **B waits for a lock A holds.** A holds mutex M when the interrupt arrives,
   and B needs M, so B sleeps. But "B sleeps" suspends A's context, and A can
   only release M after B returns. B waits for A, A waits for B. With a spin
   lock it is quicker still: B spins forever without even sleeping.
2. **B waits for something only an interrupt can deliver.** While B runs its
   interrupt line is masked, and the local APIC does not deliver interrupts of
   equal or lower priority until it sees an EOI. If B sleeps waiting for a disk
   completion or the next timer tick, and that event is carried by an
   interrupt that is being held back, nothing can wake B.
3. **A was the scheduler.** If the interrupt arrived while A held a run-queue
   lock, B's sleep calls into the scheduler, which tries to take the lock A
   holds. The CPU waits on itself.

Put another way: B is anonymous. It has no identity the scheduler can suspend,
so any attempt to suspend it suspends A instead. Misattribution is the cause;
the deadlock is the consequence.

## Threaded interrupts: giving B a name

The way to let interrupt work sleep is to give it a thread of its own. Linux's
`request_threaded_irq()` does this, and PREEMPT_RT threads almost every
interrupt. The work splits in two:

- The **hard interrupt** (top half) still runs in interrupt context and still
  cannot sleep. It does the minimum -- acknowledge, mask the line, wake the
  thread -- and returns.
- The **threaded handler** (bottom half) is an ordinary thread with its own
  stack. It may take mutexes, allocate and sleep, because a sleep now suspends
  it and nobody else.

This has costs and limits. There is an extra context switch, so latency is
higher. The line must stay masked until the thread has run, or a
level-triggered interrupt keeps firing. And some interrupts cannot be
threaded: the timer tick, IPIs, anything the scheduler itself relies on --
whatever schedules threads cannot depend on a thread being scheduled.

In this kernel, deferred work runs on per-CPU DPC worker threads (started in
`677056b`), so it may sleep. Zircon goes further: drivers live in user space
and wait on an interrupt object from their own thread.

## Preemption disabled with interrupts still on

This is the normal case, not a hazard. An interrupt arriving while preemption
is off is handled as usual: it borrows the current thread, runs, and returns
to the same thread, so the promise "no other thread on this CPU" still holds.

If the interrupt makes a higher-priority thread runnable -- the timer tick
deciding a timeslice is over, say -- the switch that would normally happen on
interrupt return is deferred instead. In this kernel:

- `PreemptionState::add_pending()` records the request;
- `irq::handle_irq_postamble()` reports that a switch is needed only when
  `is_preemption_enabled()` is true;
- `PreemptionState::enable_preemption()` calls `flush_pending_local()`, which
  takes the switch that was owed.

## How this kernel implements the four conditions

The conditions are not tracked in one place. Knowing where each lives matters
whenever code has to *ask* whether it may sleep.

- **Spin locks** are counted per CPU by `SP_SPINLOCK_NUM_HELD`
  (`kernel/kernel/sync/spin_lock.cpp`), read through
  `sync::SpinLock::num_held_cpu()`. Unlike Linux, acquiring a spin lock does
  not by itself disable preemption or interrupts; the `Guard` option decides
  that, and an option-less `Guard guard{lock}` on a spin lock is a compile
  error. The tree's rule is `Guard<SpinLock, IrqSave>` everywhere, and
  `Guard<SpinLock, IrqSaveAndPreemptDisable>` where the critical section can
  wake a thread or reach `reschedule_common()`. See the comments above
  `lockdep_option_required()` in `kernel/include/ours/sync/spin_lock.hpp`.
- **Preemption** is counted per thread in `PreemptionState`
  (`kernel/include/ours/task/preemption_state.hpp`). Linux folds spin locks and
  preemption into one `preempt_count` and asks `in_atomic()`; here they are two
  separate counts, so a check has to consult both.
- **Interrupt handlers** are implemented in terms of preemption.
  `irq::handle_irq_preamble()` (`kernel/kernel/irq/mod.cpp`) calls
  `disable_preemption()` and `disable_eager_reschedule()` once each on entry,
  as Zircon's `int_handler_start()` does. This includes the exception path:
  `arch_handle_exception()` goes through `platform_start_handling_irq()` before
  it dispatches. So **every** fault or interrupt already reads a preemption
  count of one before its handler does anything.
- **Interrupts disabled** is the CPU's IF flag. For code that was interrupted,
  the only record of whether it had interrupts on is the saved frame, because
  the interrupt gate (`type_attr = 0x8E` in `kernel/arch/x86/kernel/idt.cpp`)
  clears IF on the way in.

### Worked example: the page fault path

A page fault has to take `VmAspace::m_lock`, which is a `sync::Mutex`, so it
must know whether it may sleep. `may_sleep_resolving()` in
`kernel/arch/x86/mm/fault.cpp` asks the four questions above:

- the saved frame's IF must be set;
- `sync::SpinLock::num_held_cpu()` must be zero;
- the preemption count must not exceed `PREAMBLE_DISABLE_DEPTH` -- the one the
  exception preamble itself added. Comparing against zero was the first
  version, and it refused every fault in the system, including a plain ring-3
  one, because of that preamble;
- being in an interrupt handler is covered by the preemption count.

The answer travels to the architecture-independent resolver as
`VmfCause::MaySleep`, and `VmAspace::page_fault_internal()` refuses a fault
that arrives without it. The default is "may not sleep", so a caller that
never learned about the flag gets an error rather than a CPU wedged on a mutex.

A kernel-mode fault is not automatically atomic. Faults taken inside
`user_copy` during a system call are kernel mode, hold no lock, and sleep
normally; in the boot measured when this was written, five of eight faults
were of that kind. What is refused is only kernel code touching pageable
memory while holding a spin lock, with preemption off, or with interrupts
masked. A refused fault inside a user copy makes the copy fail, which is the
semantics of Linux's `pagefault_disable()` plus `__copy_from_user_inatomic()`:
the caller drops what it holds and retries with the sleeping variant.

## References

- Linux: `in_atomic()`, `faulthandler_disabled()`, `pagefault_disable()`,
  `do_user_addr_fault()` and `bad_area_nosemaphore()` in
  `arch/x86/mm/fault.c`, `request_threaded_irq()`, `spin_lock_irqsave()`.
- Zircon: `int_handler_start()`, `x86_pfe_handler()`, the `CriticalMutex`
  guarding `VmAspace` and `VmCowPages`.
- In this tree: `docs/changes/260926/x86-mm.md` and
  `docs/changes/260926/mm.md` record the page-fault change the example above
  describes, including the probe output that exposed the preamble baseline.
