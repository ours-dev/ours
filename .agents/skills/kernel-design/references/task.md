# Tasks, scheduling, locking, time

## Scheduler

- Fair (`kernel/kernel/task/sched/fair.*`) is the user's own design and off by
  default (`sched_normal_discipline` is `"rr"`; a lane built with `"fair"`
  compiles, links and boots). Bring-up runs on a simple round-robin
  discipline. Do not edit
  Fair at all; a refactor whose value depends on changing fair.cpp must be put
  to the user, not carried out. In the generic scheduler machinery
  (`scheduler.cpp`, `context.hpp`, `scheduler_inline.hpp`), ask before any fix
  beyond a plain seeding or ordering defect.
- The framework around the algorithm may be restructured boldly; what the user
  protects is the scheduling algorithm, not the size of a change.
- A discipline is a plain class with no base class and no virtual function,
  reached through a `ustl::Ref<Schedule>` (`ustl/dyn.hpp`; the user's call of
  2026-11-03, design in `.agents/tasks/sched-facade/DESIGN.md`).
  `Scheduler::schedule_of(Discipline)` is a `switch` over the closed enum, and
  a discipline that is not implemented (Deadline today) is an empty reference
  that traps. Five methods are required (`enqueue_thread`, `dequeue_thread`,
  `evaluate_next_thread`, `on_thread_tick`, `num_queued`); the others are
  optional and their defaults sit beside them in `schedule.hpp`. The thread-
  safety analysis cannot follow an erased call, so `Scheduler::call_schedule<>`
  carries `TA_REQUIRES(get_lock())` and a discipline declares
  `TA_REQUIRES(rq.lock())` on its own functions. One discipline outranks
  another if its enumerator is later (Deadline over Normal), not by address.
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

## Interrupt lines: owned domains and explicit controller contracts

The line owns masking and deferred holding: the deliberate genirq-derived
exception chosen on 2026-10-05. The accepted 2026-11-03 rewrite is specified in
`docs/design/261103/irq-redesign.md`. The user selected object model B,
suspended learning exercises and retained the HIrqNum/VIrqNum spelling. The
reference kernels identify useful roles; they do not prescribe our object
shape or synchronization protocol.

- Public Registration owns immutable callbacks/context/RegistrationOptions;
  the caller retains its stable address through detach and drains its own
  completion calls before reuse or destruction. Only Line invokes callbacks.
  Shared, Permanent and Oneshot combinations and unknown bits are validated
  before occupancy or hardware changes. Permanent never shares or detaches.
- Line, Domain and DomainRegistry are private. Line owns mutable occupancy,
  Held/Dying and cached trigger under L1. Domain owns transactional maps and
  boxed Lines. The directory owns domains and coordinates publication,
  revocation, IRQ RCU grace and reclamation. No public storage pointer escapes.
- DomainEndpoint is the controller-owned entry. dispatch(source) holds its own
  IRQ RCU reader scope over lookup and flow. Some means the source was found
  and completed, even if unclaimed; None leaves unmatched EOI to the root.
  Controllers stop delivery before removal and retain ops/context through it.
- HIrqNum and VIrqNum are separate scoped u32 types, with no implicit numeric
  or cross-conversion. The user retained these names; do not reintroduce
  primitive aliases. irq_for_source(domain, source) always names its domain;
  source_for_irq(id) recovers the local source. Syscall wire values remain
  u32 and convert at the ABI boundary. Vectors are a third distinct type.
- Each domain assigns consecutive kernel ids from an aligned block above its
  local hardware range and the global allocation floor. Ids are never reused.
  Root is permanent; other domains can be revoked and removed. The directory
  keeps ownership through Removing and the reader grace period, then moves
  it out under its Guard and destroys outside the writer lock.
- Ordinary controls share with_line, a structural C++ operation that owns
  lookup, status propagation and the complete reader scope. There is no
  second public storage/control facade or virtual resolver hierarchy.
- Every dispatch uses the same real L1 Guard, Permanent included, with
  interrupts and preemption disabled. The 2026-11-03 guard requirement
  supersedes the earlier lockless Permanent path. Mutable line facts are
  TA_GUARDED and helpers TA_REQUIRES; no analysis escape is permitted.
  Statistics read outside L1 are atomic.
- L1 stays held across callbacks; detach taking it is the callback barrier.
  Orders are L1 -> LI for controller registers or L1 -> object L2 -> port/wait
  internals. Owners release L2 before line control. No callback controls any
  line: two line locks have no order. Named runtime lockdep classes still
  depend on unfinished library infrastructure; do not bypass its Guard.
- Held records deferred line work, while owner debt stays under its own L2.
  Completion takes L1, validates attachment and calls bounded readiness under
  L2 before clearing Held/unmasking. Physical owner readiness is Idle/Waiting,
  not Triggered/NeedAck/Destroyed. This prevents an old ack reopening newer
  work without tokens or duplicate debt. Ack/wait drop L2 before completion.
- A level Oneshot line masks before notification and remains masked on
  WakeThread; unmask leaves Held intact. Edge delivery coalesces at the object
  and does not hold the line. Detach/revocation clears Held and masks; it does
  not wait for a user thread that may never ack. User lines stay exclusive.
- ControllerOps declares Eoi or AckEoi independently of Edge/Level. Unknown,
  missing or unused handshake hooks fail binding. EOI occurs once on the
  receiving CPU after line-state updates, including unclaimed/held sources.
  Synthetic dispatch issues neither ACK nor EOI. AckEoi is for a known source
  retaining in-service state until EOI; claim-token/mask-before-ACK protocols
  need their own declaration and tests, not a silently reused trigger label.
- Deferred registration additionally requires Masking::Synchronous and
  mask/unmask hooks. Pointer presence cannot promise hardware completion.
  IO-APIC declares Eoi and synchronous masking. Per-instance context supports
  overlapping hardware numbers without global controller state.
- The interrupt object remains the deferred handler: Oneshot, WakeThread,
  existing NeedAck/coalescing/port/timestamp behavior, and the four object
  hooks. The line is the only mask-state writer; its IO-APIC entry remains the
  only mask-bit record. No in-kernel IRQ threads, disable depth or replay
  machine are implied by these interfaces.
- Revocation marks every line with rollback on refusal, unlinks callbacks
  under L1 with preemption off, then publishes the detached binding last
  with release ordering. Object destroy always detaches, including after a
  line-side revocation, so it drains the callback before freeing storage.
- entry.hpp/entry.cpp own the root-handler publication and regular trap
  bookkeeping. enter_trap saves the prior CPU frame on the trap stack and
  raises both scheduler disable counts. leave_trap restores frame/counts
  before architecture preempts or delivers signals. Nested exit restores
  the outer frame. NMI, double fault and machine check remain separate.
- Actual startup is the platform InitLevel::Irq hook. Empty generic/arch IRQ
  init calls and the unbound invalid LAPIC controller table are retired.
  Local APIC fixed vectors retain their architecture-owned handling.

Controller adapters, contexts, endpoints and registration storage have explicit
borrowed lifetimes; no new ownership manager hides a missing lifetime rule.
Future hardware must state its real handshake/masking contract and demonstrate
it with adapter tests. Performance changes require the separate measurement
workflow; no speedup is claimed for this rewrite.

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
- After a pass that ran a callback the device is held off for the pass's own
  length, at most 100 ms (`TimerQueue::m_hold_off_until`): a timer's deadline
  is armed no sooner than that, the preemption deadline is not. Without it a
  periodic timer whose callback is slower than its period is due again within
  one period of the pass, and the thread it interrupts keeps only the gap
  between two callbacks. The whole device is held, not the slow timer (the
  user's choice, 2026-10-21), so the other timers on the CPU are late by up to
  the pass before them. Linux holds off only when a timer is already late;
  that trigger does not fire for a timer that restarts ahead of now.
- No tick drives the scheduler: Zircon's tickless model, the user's choice
  of 2026-10-13 (decision ii in `.agents/tasks/idle-power/DESIGN.md`). A
  slice ends at the CPU's preemption deadline, the slot in its `TimerQueue`.
  `Scheduler::set_next_thread()` sets it at every pick, the running thread
  picked again included, from `SchedEntity::remaining_slice()`, and empties
  it for the idle thread, so an idle CPU is woken by its timers alone. A
  passed deadline only asks for a reschedule
  (`Scheduler::Current::on_preemption_deadline()`, Zircon
  `Scheduler::TimerTick()`); the reschedule charges and rotates. The
  scheduler sets it under its run queue lock, so that lock comes before the
  timer queue's. A periodic tick finds a passed deadline at the next tick.
- A CPU enters one-shot when its own timer device is enrolled, on that CPU,
  and never leaves; it is not a switch of a running tick. The boot option
  `kernel.time.oneshot` decides, so one kernel serves both settings: the dev
  images turn it on by default (the build argument `time_oneshot`), and the
  kernel does nothing one-shot unless the option is there.
- Jiffies advance only from a periodic tick, on their one CPU: the PIT's at
  boot, which the local APIC's calibration counts, and every tick of a
  periodic build. With a one-shot device they stand still, read by nothing
  (the user's choice of 2026-10-13, A in `.agents/tasks/idle-power/PLAN.md`).
  They stay: removing them, with the calibration timed by the HPET and the
  PIT's channel 2 polled where there is none, was put to the user and
  declined (2026-10-23). Do not remove them as dead code.
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
  files (`ours/cpu_hotplug_hook.hpp`, a level each: AddressSpace, Tick,
  Timers, Ipi), walked up one way and down the other. AddressSpace, the
  lowest, is the arch's: a CPU coming back moves off the trampoline's page
  tables onto the kernel's before any other hook runs, and only after
  `come_back()`'s claim, before which nothing may take a lock
  (2026-10-30). Only the CPU's own steps
  (deactivate, the online mask) are named in the idle thread. A subsystem
  whose control path touches every online CPU takes `cpu_hotplug_lock()`
  before its own lock (the sampler does, and so does `irq_rcu_synchronize()`,
  whose wait lasts as long as the longest span with interrupts off on any
  online CPU: nothing that can run under the lock may wait for it), so task
  never depends on it (the
  user's call, 2026-10-13, after the first version called each subsystem by
  name).
- The AP start-up path (`CPU_INIT_CODE`, the trampoline buffer) stays after
  boot only with the build argument `cpu_hotplug`; without it it is freed
  as init code and `take_cpu_offline()` answers NotSupported (the user's
  call, 2026-10-13).
- Every CPU a thread is moved to is sent a reschedule, as a wakeup's target
  is: an idle CPU sleeps until something wakes it, and with no tick that
  could be a second away (`Scheduler::Current::deactivate()`, 2026-10-14).

## Idle

The user's call, 2026-10-13 (deep idle and tickless both,
`.agents/tasks/idle-power/DESIGN.md`).

- An idle CPU waits in MWAIT, C1 for now, on a per-CPU monitor word alone
  on its cache line when CPUID reports MONITOR, and in HLT otherwise
  (`ArchIdleThread::idle()`), one wait per call: the idle loop looks again
  at its run queue and transition whatever ended it.
- A reschedule to a CPU waiting on its word writes the word instead of
  sending an IPI (`ArchIdleThread::wake()` in `arch_mp_reschedule()`), after
  Zircon's `arch_mp_reschedule()` and `ArchIdlePowerThread::EnterIdleState()`
  (`zircon/kernel/arch/x86/mp.cc`). The word says Waiting only inside
  `idle()`, so a waker that finds it so knows the target is the idle thread,
  and that its loop will see what it was woken for.
- Which state each wait enters is the governor's
  (`idle_states::IdleStates::pick()`, `kernel/arch/x86/lib/idle_states`):
  Zircon's rule, the deepest enabled state whose exit latency is under three
  times the expected idle time, with the expected time the last idle
  stretch bounded by the time until the CPU's next deadline (2026-10-14).
  The states are a table per processor model, cut to those CPUID leaf 5
  reports, found once on the boot CPU before any other starts. Only C1 is
  enabled, as in Zircon. A CPU without ARAT (CPUID leaf 6) keeps C1 alone,
  since its local APIC timer stops in anything deeper; handing the deadline
  to a broadcast device instead is not built (2026-10-23).
- QEMU's TCG runs MWAIT as a PAUSE with more than one CPU, and its leaf 5
  reports no C-states: the path is tested there with `CPU=Nehalem,+monitor`
  or `Skylake-Client,+monitor`, but what it saves is not.

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
