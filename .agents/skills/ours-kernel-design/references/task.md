# Tasks, scheduling, locking, time

## Scheduler

- Fair (`kernel/kernel/task/sched/fair.*`) is the user's own design and out of
  the build. Bring-up runs on a simple round-robin `ISchedule`. Do not edit
  Fair at all; a refactor whose value depends on changing fair.cpp must be put
  to the user, not carried out. In the generic scheduler machinery
  (`scheduler.cpp`, `context.hpp`, `scheduler_inline.hpp`), ask before any fix
  beyond a plain seeding or ordering defect.
- The framework around the algorithm may be restructured boldly; what the user
  protects is the scheduling algorithm, not the size of a change.
- Numerics follow Zircon: weights and times are `ffl::Fixed`
  (`SchedWeight` is `ffl::Fixed<int64_t, 10>`, a fraction in (0, 1]). No Linux
  integer weights, no `wmult` reciprocal table. Read
  `zircon/kernel/include/kernel/scheduler_state.h` before designing any
  scheduler primitive.

## Thread and wait-queue locking

- Never propose a global thread lock. `Thread` (`m_pi_lock`,
  `m_resource_lock`), `WaitQueue` (`m_lock`) and each per-CPU run queue
  (`rqlock`) lock separately. What is missing is the protocol for taking an
  arbitrary priority-inheritance chain of those locks: Zircon's ChainLock
  (`zircon/system/ulib/concurrent/include/lib/concurrent/chainlock.h`, design in
  `zircon/kernel/lib/kconcurrent/docs/chainlock.md`). The
  `OwnedWaitQueue::lock_pi_chain_*` stubs are reserved for it, and the empty
  `system/lib/concurrent` exists to hold it.
- `Thread`'s two locks follow Linux's `task_struct`, not Zircon:
  `m_pi_lock` is `p->pi_lock` (PI edges, state transitions, wakeup
  serialisation; nests outside the run-queue lock), and `m_resource_lock` is
  `alloc_lock`/`task_lock()` (the resources a thread holds; never on a
  scheduler path).

## Posting a port from interrupt context

The accepted design's rule that an interrupt-context producer only queues a
Dpc (`docs/design/260923/userspace-interface.md` 4.8, hard rule 3) has one
exception, the user's call of 2026-10-04 (D6 (a1) in
`.agents/tasks/microkernel-interrupts/DESIGN.md`): an interrupt object queues
its packet on its bound port straight from the handler, as Zircon's
`InterruptDispatcher::InterruptHandler()` does. The reason: that work is
bounded and allocates nothing. The packet is embedded in the interrupt
object, queued under the port's interrupt-queue spin lock (L3), and the
waiter is woken through the port's counting `sync::Semaphore`, whose
`post()` takes only spin locks with interrupts off. A Dpc would add a thread
switch per interrupt and make destroy cancel it first, capping the latency
later. The lock order on that path is L1 (line) -> L2 (interrupt object) ->
{LI (IO-APIC), L3}; L3 is never held with the port's Mutex. Any other
interrupt-context producer still goes through a Dpc.

## Interrupt lines: kernel/irq follows genirq

`kernel/irq` is modelled on Linux's genirq, not on Zircon's interrupt
manager: a deliberate Linux exception, the user's call of 2026-10-05 (D1 (b),
D2 (a) and D6 (a) in `.agents/tasks/irq-genirq/DESIGN.md`, which names each
counterpart). The code reaches it step by step (the DESIGN's Staging); until
step 7 the interrupt objects still write the level mask themselves.

- A line is an `irq::Desc` (lock L1, its flow, its actions, the Oneshot flag
  and one held bit) in a flat table built once. A requester is an
  `irq::Action`; the controller is a stateless `irq::Chip`, seen through
  `irq::Data`. The names are genirq's roles in this scaffold's short style.
- Each line runs a flow handler, `handle_fasteoi` for a level line or
  `handle_edge`, chosen from the trigger mode when the table is built and
  re-chosen under L1 by `configure()` and attach. The flow sends the one EOI
  per vector, through the chip, after the actions; arch sends it only for a
  vector with no line.
- ONESHOT belongs to the line: `handle_fasteoi` masks a Oneshot line before
  its actions and leaves it masked while held is set; `finalize_oneshot`
  clears held and unmasks. A Oneshot action never shares, as well as never
  being Permanent: one held bit records one thread's debt, so a second
  Oneshot action's finalize would unmask the first one's line.
- The interrupt object is the threaded handler. Its handler returns
  WakeThread; its ack, or the wait that serves as one, calls the finalize
  after dropping L2 and only once the debt is cleared: an ack that sends the
  next packet leaves the object owed an ack again, and finalizing there
  would unmask a level line before it is acked. It keeps its four
  virtuals, with `mask_interrupt()` empty, and keeps NEEDACK, coalescing,
  port delivery and timestamps: a user-visible protocol, and kernel/irq
  knows no ports.
- L1 stays held across the handlers, as Zircon runs its handler under the
  entry lock, and detach taking L1 is the barrier: no INPROGRESS flag, no
  synchronize spin. Finalize takes L1 then LI and is never called with L2
  held, so there is no L2 -> L1 edge.
- A Permanent line, such as the tick's, is the exception: it is dispatched
  without L1, because its action list never changes once Permanent is set
  and there is nothing for the lock to keep out. Its flow therefore reads
  nothing L1 guards, and Oneshot together with Permanent is refused.
- Inside the genirq model ours keeps rules of its own: detach clears held
  rather than waiting for the thread, because a user thread may never ack;
  from step 8 `unmask()` takes L1 and leaves a held line masked; and the
  pin's IO-APIC entry is the only record of its mask, with no software
  Masked bit.

The reason: Zircon makes the mask a property of the object kind. That leaves
the level mask two writers, the object and the flow for a line with no action,
and the object writes the controller without L1, so kernel/irq can keep no
line state. With the mask a property of the line's flow it has one writer,
while what user space sees is unchanged: a level line stays masked until the
ack, an edge line is only coalesced. The four virtuals keep Zircon's
in-device MSI mask open.

Not taken from genirq: in-kernel interrupt threads (a kernel driver defers
with a Dpc), disable depth, lazy disable and resend, and irq_domain until MSI
(`domain.hpp` is its scaffold). User lines stay exclusive.

## Small concurrent state

For a lifecycle state plus configuration fields set together (the prof
`Sampler` is the case): exactly one atomic, the state. The other fields stay
plain, are written only while no reader acts on them, and are published by the
release store to the running state. Never pack several fields into one atomic
word.

## Time

`kernel/time` has no TimerScheduler layer: `TimerQueue` is the per-CPU timer
engine and does that work itself.

## Prof

The user's call, 2026-10-02; the design is in
`.agents/tasks/prof-sampler-object/DESIGN.md`.

- Profiling is a kernel object userspace holds, after Zircon's
  `SamplerDispatcher` and `ThreadSampler`
  (`zircon/kernel/lib/thread_sampler/`): created from the System resource
  slot SAMPLING (15), at most one session system-wide, ended when its last
  handle closes. Nothing samples and nothing is allocated while no session
  runs: no hook on the tick path, no buffers built at boot.
- A per-CPU `time::Timer`, armed only while a session runs, triggers
  samples. The PMU is not a sampler source; it returns later as a facility
  of its own, as Zircon's perfmon is.
- Both user and kernel stacks, chosen in the config. User stacks follow
  Zircon: the timer only marks the thread, which walks its own stack by
  frame pointers on its way back to ring 3. Kernel stacks are unwound in the
  timer interrupt with `.eh_frame`, a deliberate departure: Zircon does not
  sample kernel stacks.
- Kernel facts (current CPU, thread, time, output) never go through an arch
  seam. `kernel/lib/prof` is a host-testable core that takes them as
  arguments; arch supplies only pc, sp, fp and privilege level from a frame.
- The session (timers, buffers, unwind index, state) is the kernel-only
  library `kernel/lib/thread_sampler`, top-level namespace `sampler`, where
  Zircon keeps it (the user's call, 2026-10-05). Not `kernel/kernel`: that
  holds what the kernel needs for itself, and a session is a facility user
  space asks for. `kernel/kernel/task` and `kernel/object` both call it.
