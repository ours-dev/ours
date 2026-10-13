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

## Thread lifetime and reaping

Kernel threads keep Zircon's shape: `Thread::spawn()` returns a raw
`Thread *`, and exactly one party frees a thread -- the thread itself when
Detached, through a Dpc pinned to its CPU after its final switch, or its one
joiner (`Flags::Joined`). The design is `.agents/tasks/thread-join/DESIGN.md`.

Tried and abandoned, the user's call of 2026-10-10
(`.agents/tasks/thread-handle/DESIGN.md`): a move-only `JoinHandle` from
`spawn()`, an `UnstartedThread` typestate in front of it, and freeing by the
last of two references (the handle's and the thread's own). It worked and
passed every test, and was reverted because it added kernel code for an
interface only kernel threads use; user threads reach the kernel through
`ThreadDispatcher` and object handles. Ownership semantics -- RAII handles,
join-on-drop or detach-on-drop -- belong in a user-space thread library built
on thread handles, the way libc's `thrd_join()` and Rust's `JoinHandle` sit
on top of Zircon's. Do not propose them for kernel threads again without a
new reason; what the kernel owes such a library is a thread handle and a
waitable "terminated" signal on it.

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
counterpart). The code reached it step by step (the DESIGN's Staging, all
eight steps landed by 2026-10-07): the interrupt objects write no mask, their
action says Oneshot and a level line's flow masks it, and every write of a
line's mask, `irq::mask()` and `irq::unmask()` included, is made under the
line's lock L1.

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
- The interrupt object is the threaded handler. Its action says Oneshot and
  its handler always returns WakeThread, Destroyed included, so a level line
  stays masked until the detach. Its ack, or the wait that serves as one,
  calls the finalize after dropping L2 and only once the debt is cleared:
  an ack that sends the next packet leaves the object owed an ack again,
  and finalizing there would unmask a level line before it is acked. It
  keeps its four virtuals, with `mask_interrupt()` empty, and keeps NEEDACK,
  coalescing, port delivery and timestamps: a user-visible protocol, and
  kernel/irq knows no ports.
- L1 stays held across the handlers, as Zircon runs its handler under the
  entry lock, and detach taking L1 is the barrier: no INPROGRESS flag, no
  synchronize spin. Finalize, `mask()` and `unmask()` take L1 then LI and
  are never called with L2 held, so there is no L2 -> L1 edge, nor from any
  line's handler: on their own line they would spin on its own dispatch,
  and on another they would hold two lines' L1, which have no order.
- A Permanent line, such as the tick's, is the exception: it is dispatched
  without L1, because its action list never changes once Permanent is set
  and there is nothing for the lock to keep out. Its flow therefore reads
  nothing L1 guards, and Oneshot together with Permanent is refused.
- Inside the genirq model ours keeps rules of its own: detach clears held
  rather than waiting for the thread, because a user thread may never ack;
  `unmask()` leaves a held line masked and answers Ok, the finalize being
  the one unmask of a held line; and the pin's IO-APIC entry is the only
  record of its mask, with no software Masked bit.

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

For a lifecycle state plus configuration fields set together (the sampler is
the case): exactly one atomic, the state. The other fields stay plain, are
written only while no reader acts on them, and are published by the release
store to the running state. Never pack several fields into one atomic word.
Nor keep a second copy of the same state: the sampler once had the recorder's
own Stopped/Starting/Running atomic beside `Session::ACTIVE`, both meaning
"producers may write", and now `ACTIVE` alone gates them (2026-10-07). A
lock-guarded control-plane state beside it is not a copy: it is what the
control calls check, under their lock.

## Event and Semaphore

Each is named after its model, as in Zircon (`kernel/include/kernel/event.h`,
`semaphore.h`). An `Event` is one bit, so `signal()` sets it and `unsignal()`
clears it. A `Semaphore` is a count, so `post()` adds a unit and `wait()`
takes one. `post()` is the counting form of `signal()`, not its opposite.
Do not rename either pair to match the other (the user asked why, 2026-10-13;
the class comments in `ours/sync/event.hpp` and `semaphore.hpp` give the
answer).

## Time

`kernel/time` has no TimerScheduler layer: `TimerQueue` is the per-CPU timer
engine and does that work itself.

`kernel/time` is a hybrid, the user's call of 2026-10-09 (direction A and
refactor R1 in `.agents/tasks/high-res-timers/DESIGN.md`). The target,
decided before it is built:

- The device layers follow Linux, a deliberate exception: `ClockSource`
  with ratings, `ClockEventDevice` and `TickDevice` after clockevents and
  tick-common.
- The timer engine follows Zircon: the per-CPU `TimerQueue` is the only one
  (`zircon/kernel/kernel/timer.cc`). No hrtimer bases, timer wheel or
  softirq.
- Once a CPU's one-shot device is up, `TimerQueue` is the only writer of
  it, and nothing ticks: the device is armed for the queue's timers and the
  scheduler's preemption deadline, a slot in the queue (Zircon
  `TimerQueue::PreemptReset()`), never a second writer. An idle CPU is woken
  by its own timers alone. A periodic tick serves early boot only, and the
  builds that turn one-shot off.
- No tick drives the scheduler: Zircon's tickless model, the user's choice
  of 2026-10-13 (decision ii in `.agents/tasks/idle-power/DESIGN.md`). A
  slice ends at the CPU's preemption deadline, the slot in its `TimerQueue`.
  `Scheduler::set_next_thread()` sets it at every pick, the running thread
  picked again included, from `SchedEntity::remaining_slice()`, and empties
  it for the idle thread, so an idle CPU is woken by its timers alone. A
  passed deadline only asks for a reschedule
  (`Scheduler::Current::on_preempt_timer()`, Zircon `Scheduler::TimerTick()`);
  the reschedule charges and rotates. The scheduler sets it under its run
  queue lock, so that lock comes before the timer queue's. A periodic tick
  finds a passed deadline at the next tick.
- A CPU enters one-shot when its own timer device is enrolled, on that CPU,
  and never leaves; it is not a switch of a running tick. The boot option
  `kernel.time.oneshot` decides, so one kernel serves both settings: the dev
  images turn it on by default (the build argument `time_oneshot`), and the
  kernel does nothing one-shot unless the option is there.
- Jiffies advance only from a periodic tick, on their one CPU: the PIT's at
  boot, which the local APIC's calibration counts, and every tick of a
  periodic build. With a one-shot device they stand still, read by nothing
  (the user's choice of 2026-10-13, A in `.agents/tasks/idle-power/PLAN.md`);
  removing them, with the calibration timed by the HPET, is
  `docs/repairs/261013/kernel-time.md` item 2.
- A CPU whose tick device refuses a deadline gives it up there and then, in
  the context that found out, and takes the best device that has not failed
  (`TickDevice::on_device_refused()`); a global device serves it through a
  `TickBroadcast` group, by IPI. Never by a worker thread: a CPU without a
  timer device has nothing to preempt with, and a worker waits behind
  whatever it is running (2026-10-11).
- The TSC becomes a clock source only when CPUID reports it invariant, after
  Zircon's `pc_init_timer()`, not after Linux's clocksource watchdog.

## CPU hotplug

The user's call, 2026-10-12; the design is in
`.agents/tasks/cpu-hotplug/DESIGN.md`.

- Zircon's sequence (`mp_unplug_cpu_mask()`), one ordered function, not
  Linux's cpuhp state machine: the control thread stops the CPU's Dpc
  workers, asks its idle thread to go Offline, then takes over its timers
  and Dpcs. The idle thread does the CPU's own part, picked by the scheduler
  while it has a pending transition, even over runnable threads.
- Threads pinned to a CPU going offline follow Zircon, not Linux's
  `is_cpu_allowed()`: none may be left to wake there, and one that does
  fails selection's assertion. The Dpc workers are stopped first for that
  reason; there is no "inactive but running" window to allow them in.
- The boot CPU, which owns jiffies and takes every device interrupt, never
  goes offline.
- Subsystems take part through CPU hotplug hooks registered in their own
  files (`ours/cpu_hotplug_hook.hpp`, a level each: Tick, Timers, Ipi),
  walked up one way and down the other; only the CPU's own steps
  (deactivate, the online mask) are named in the idle thread. A subsystem
  whose control path touches every online CPU takes `cpu_hotplug_lock()`
  before its own lock (the sampler does), so task never depends on it (the
  user's call, 2026-10-13, after the first version called each subsystem by
  name).
- The AP start-up path (`CPU_INIT_CODE`, the trampoline buffer) stays after
  boot only with the build argument `cpu_hotplug`; without it it is freed
  as init code and `take_cpu_offline()` answers NotSupported (the user's
  call, 2026-10-13).

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
  seam. The core (`//kernel/lib/sampler:core`: `KernelUnwinder`,
  `walk_frame_pointers()`, `Recorder`) is host-testable and takes them as
  arguments; arch supplies only pc, sp, fp and privilege level from a frame.
  Walking a stack and storing it are separate (2026-10-07): the unwinder or
  the walk turns a stack into addresses, and the recorder only stores them.
- User code keeps a frame pointer in leaf functions too
  (`-mno-omit-leaf-frame-pointer`, `//build/config:frame_pointers_in_leaves`),
  so the frame-pointer walk of a user stack never skips a leaf's caller (the
  user's call, 2026-10-07: always, not only in a profiling build). Kernel
  code keeps the default; its stacks unwind through `.eh_frame`.
- The whole sampler is `kernel/lib/sampler`, named like its namespace
  `sampler`, in the place Zircon keeps its `ThreadSampler`
  (`kernel/lib/thread_sampler`; the user's calls, 2026-10-05 and 2026-10-07).
  One library, two targets: `:core` (the `Recorder`, its buffers, the
  frame-pointer walk, the dump format; builds on the host, with the tests
  and `prof-fold` in `cli/`) and `:sampler` (the `Session`: timers,
  buffers, unwind index, state; kernel-only). The core is not moved into
  the kernel-only target: that would lose its host tests. Not
  `kernel/kernel`: that holds what the kernel needs for itself, and a
  session is a facility user space asks for. `kernel/kernel/task` and
  `kernel/object` both call it.
- There is no separate `kernel/lib/prof`: its record buffers and sink were
  the sampler's alone and live in `:core`, and its trace sites, for a
  tracer never written, were deleted (the user's call, 2026-10-07).
