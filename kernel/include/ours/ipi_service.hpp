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
    /// word that outlives the call. Zircon's mp_sync_task_t is the same
    /// signature for the same reason.
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
    /// interrupting itself with it.
    ///
    /// Requirements, both of which are asserted:
    ///
    ///   - Interrupts must be enabled. This spins until the targets answer,
    ///     and they answer from their interrupt handlers; a caller that spun
    ///     with interrupts masked could not service the sync_exec a target is
    ///     itself waiting on, and the two would spin at each other for ever.
    ///   - |task| runs in interrupt context, so it must not block, take a lock
    ///     that is held outside interrupt context, or call sync_exec() again.
    ///
    /// Linux's smp_call_function_many() and Zircon's mp_sync_exec() are the
    /// same operation; this follows Zircon's shape, where the request lives on
    /// the caller's stack and the targets clear their bit in it.
    static auto sync_exec(CpuMask targets, SyncTask task, void *context) -> void;

    /// Runs whatever sync_exec() has queued for this CPU. This is the body of
    /// the arch generic-IPI handler and has no other caller.
    static auto handle_generic() -> void;

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
