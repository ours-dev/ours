/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IPI_SERVICE_HPP
#define OURS_IPI_SERVICE_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/ipi_event.hpp>

namespace ours {
class IpiService {
    using Self = IpiService;

  public:
    using Event = IpiEvent;

    /// A function to run on another CPU, with the argument it is given.
    ///
    /// A plain function pointer and a `void *`, not anything that captures:
    /// the call runs in interrupt context on a CPU that has never seen the
    /// caller's frame, so whatever it needs has to be reachable through one
    /// word that outlives the call.
    using SyncTask = auto (*)(void *context) -> void;

    static auto init() -> void;

    static auto send_resched(CpuNum dest) -> void;

    static auto send_resched(CpuMask const &dest) -> void;

    /// Runs |task| on every online CPU in |targets| and returns only once all
    /// of them have finished running it.
    ///
    /// This is the piece that makes a "generic" IPI generic. An IPI carries a
    /// vector and nothing else, so a second client of IrqVec::IpiGeneric could
    /// not previously say what it wanted done -- the profiler had to reach past
    /// this class into apic_send_ipi_multicast() and get its own handler
    /// hardcoded into the x86 fault dispatcher. The payload lives in the
    /// target's mailbox now, and the vector only says "look at it".
    ///
    /// The calling CPU, if it is in |targets|, runs |task| itself rather than
    /// interrupting itself with it, with interrupts off as the others run it:
    /// they stay off from finding out which CPU this is until that share is
    /// done, so it runs on the CPU that was left out of the IPI, and the caller
    /// need not disable preemption for that.
    ///
    /// Requirements, both of which are asserted:
    ///
    ///   - Interrupts must be enabled. This spins until the targets answer,
    ///     and they answer from their interrupt handlers; a caller that spun
    ///     with interrupts masked could not service the sync_exec a target is
    ///     itself waiting on, and the two would spin at each other for ever.
    ///   - |task| runs in interrupt context, so it must not block or call
    ///     sync_exec() again. It may take a spin lock only if every holder
    ///     keeps interrupts off while holding it and never waits for another
    ///     CPU meanwhile; otherwise the task can spin for ever on a lock held
    ///     by the code it interrupted, or by a CPU that is waiting for this one.
    ///
    /// The request lives on the caller's stack and each target clears its own
    /// bit in it once it has run |task|, so a call allocates nothing.
    static auto sync_exec(CpuMask targets, SyncTask task, void *context) -> void;

    /// Runs whatever sync_exec() has queued for this CPU. This is the body of
    /// the arch generic-IPI handler and has no other caller.
    static auto handle_generic() -> void;

    /// This CPU is going offline: runs what sync_exec() has queued for it,
    /// as handle_generic() would, and closes its mailbox, so that a later
    /// sync_exec() naming it counts it as done instead of queueing a request
    /// nothing would run. On the CPU going offline, with interrupts off,
    /// after it has left the online mask: the Ipi level's CPU hotplug hook
    /// (ipi_service.cpp, ours/cpu_hotplug_hook.hpp).
    ///
    /// Without it a sync_exec() that read the online mask just before the
    /// CPU left would either wait for ever or, if it gave up on the CPU,
    /// leave its request -- which lives on the caller's stack -- linked in a
    /// mailbox the CPU would read when it came back.
    static auto close_mailbox() -> void;

    /// The CPU is back: its mailbox takes requests again. On the CPU coming
    /// back, interrupts off, before it rejoins the online mask: the same
    /// hook's other half.
    static auto open_mailbox() -> void;

    /// Stops every other online CPU, and waits (briefly, and without
    /// insisting) for them to say they have stopped.
    ///
    /// There is no resuming from this: a suspended CPU halts with interrupts
    /// masked for good. It exists for the moment when the machine has to stop
    /// being a moving target -- panic, above all, where three other CPUs
    /// carrying on buries the message and the log dump underneath everything
    /// they go on to print.
    ///
    /// Best effort by construction. It is called from contexts where things
    /// are already wrong, so it neither asserts nor reports: a CPU that is
    /// wedged, or that has interrupts masked for longer than the wait, simply
    /// does not answer, and the caller carries on without it.
    static auto suspend_others() -> void;

    static auto send_event(CpuNum dest, Event event) -> void;

    static auto send_event(CpuMask const &dests, Event event) -> void;
};
} // namespace ours

#endif // #ifndef OURS_IPI_SERVICE_HPP
