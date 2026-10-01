#include <ours/arch/mp.hpp>
#include <ours/assert.hpp>
#include <ours/cpu.hpp>
#include <ours/init.hpp>
#include <ours/ipi_service.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/sync/spin_lock.hpp>
#include <ours/task/scheduler.hpp>

#include <atomic>
#include <new>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/spin_hint.hpp>
#include <ustl/collections/intrusive/list.hpp>

namespace ours {
namespace {
    /// One sync_exec() request, as the CPUs being asked see it.
    ///
    /// It lives on the *caller's stack*, and is linked into every target's
    /// mailbox from there. That is safe because sync_exec() does not return
    /// until |outstanding| is empty, and a target's last touch of this object
    /// is the store that clears its own bit -- so the frame cannot go away
    /// under a CPU that is still reading it. Zircon's mp_sync_context is the
    /// same trick and is what makes this allocation-free, which matters
    /// because the first caller may run long before there is a heap.
    struct SyncCall {
        IpiService::SyncTask task;
        void *context;

        /// The CPUs that have been asked and have not answered. Cleared a bit
        /// at a time by the targets; the caller spins until it is empty.
        std::atomic<CpuMask> outstanding;
    };

    /// One target CPU's place in that target's queue.
    ///
    /// Split from SyncCall because an intrusive hook may be on exactly one list
    /// at a time, and one request goes into several CPUs' mailboxes at once.
    /// Linking a single node into three lists is not a subtle mistake -- the
    /// list caught it with a `ud2` on the second push -- but it is an easy one
    /// to make, because the request really is one thing. Zircon splits it the
    /// same way: one mp_sync_context, and an mp_ipi_task per CPU pointing at
    /// it.
    struct SyncSlot {
        SyncCall *call;
        ustl::collections::intrusive::ListMemberHook<> queue_hook;
        USTL_DECLARE_HOOK_OPTION(SyncSlot, queue_hook, QueueListOption);
    };
    USTL_DECLARE_LIST(SyncSlot, SyncSlotList, SyncSlot::QueueListOption);

    /// What a CPU has been asked to run.
    ///
    /// A queue rather than a single slot: two CPUs may each start a sync_exec()
    /// naming this one, and neither may drop the other's request. The lock is
    /// only ever taken by a *sender* for a CPU that is not itself, or by this
    /// CPU's own handler -- never both from the same CPU -- so a handler
    /// interrupting a sender on the same core cannot deadlock on it.
    ///
    /// That argument does not cover preemption, so the lock is still held
    /// with interrupts off on both sides. A sender preempted while holding
    /// CPU B's mailbox lock leaves it held by its own CPU, and the next
    /// thread there to sync_exec() B spins on it for good. Zircon's
    /// mp_sync_exec() saves the interrupt state before taking ipi_task_lock
    /// for the same reason, and its mp_mbx_generic_irq() takes it from the
    /// handler.
    struct Mailbox {
        sync::SpinLock lock;
        SyncSlotList calls;
    };

    class CurrentMailbox: public mm::CpuLocal::Current<Mailbox> {};
} // namespace

/// Builds every CPU's mailbox.
///
/// A Mailbox holds a SpinLock and an intrusive list, and an intrusive list is
/// not a valid empty list when it is merely zeroed -- its sentinel has to point
/// at itself. The per-CPU template memory a CPU_LOCAL object starts life in is
/// zeroed, so it cannot be used as one: the first push_back() wrote through a
/// null next-pointer and took a page fault. Placement-new over each CPU's copy
/// is how Scheduler::init(), IdleThread::init() and DpcRunner::init() each
/// answer the same thing.
///
/// Every possible CPU, driven from the boot CPU, rather than each CPU building
/// its own on the way up: an AP can take a generic IPI from the moment it
/// enables interrupts, and the CPU that sends it fills the target's mailbox
/// before that. time::init() builds every TimerQueue here for the same reason.
INIT_CODE
auto IpiService::init() -> void {
    cpu_possible_mask().for_each([](CpuNum cpu) { new (&CurrentMailbox::get(cpu)) Mailbox(); });
}

/// Delegates rather than repeating the filter.
///
/// The two overloads disagreed: this one dropped an inactive CPU but would
/// happily interrupt the calling CPU, which the mask form refuses on purpose
/// and explains at length below. Nothing calls this one today, so the
/// disagreement had never been paid for -- and an overload whose behaviour
/// differs from its sibling's for no stated reason is a trap laid for whoever
/// calls it first.
auto IpiService::send_resched(CpuNum cpu) -> void {
    send_resched(CpuMask::of(cpu));
}

auto IpiService::send_resched(CpuMask const &mask) -> void {
    auto const local_cpu = Cpu::id();

    // The local CPU is never a target: a reschedule request for this CPU is a
    // pending-preemption bit, not an interrupt to itself, and every caller has
    // already recorded that bit before coming here. Inactive CPUs cannot take
    // a reschedule either. What is left is what gets the IPI -- |targets|,
    // not |mask|: this used to filter into |targets| and then test and send
    // |mask| anyway, so every wakeup on the boot CPU interrupted itself
    // ("IPI-Resched on CPU[0]" in the boot log) and inactive CPUs were IPI'd.
    auto targets = mask;
    targets.set(local_cpu, 0);
    targets &= task::Scheduler::peek_active_mask();

    if (!targets.count()) {
        return;
    }

    arch_mp_reschedule_mask(targets);
}

auto IpiService::suspend_others() -> void {
    auto targets = Cpu::online_mask();
    targets.set(Cpu::id(), false);
    if (targets.none()) {
        return;
    }

    arch_mp_suspend(targets);

    // Wait for the machine to go quiet, but do not insist. Each CPU clears its
    // own Online bit as the last thing it does, so "quiet" is something that
    // can be observed rather than waited out with a made-up delay -- and the
    // bound is what keeps a CPU that cannot answer from taking the caller down
    // with it. Linux's smp_send_stop() has the same shape and the same
    // resignation about it.
    constexpr auto MAX_SPINS = usize{10'000'000};
    for (auto spin = usize{0}; spin < MAX_SPINS; ++spin) {
        if (Cpu::num_online_cpus() <= 1) {
            return;
        }
        arch::spin_hint();
    }
}

auto IpiService::send_event(CpuNum dest, Event event) -> void {
    arch_mp_send_ipi(dest, event);
}

auto IpiService::send_event(CpuMask const &dests, Event event) -> void {
    arch_mp_send_ipi_mask(dests, event);
}

auto IpiService::sync_exec(CpuMask targets, SyncTask task, void *context) -> void {
    DEBUG_ASSERT(task);
    DEBUG_ASSERT(!arch::irqs_disabled(), "sync_exec() spins on CPUs that answer from interrupts");

    // Interrupts off from asking which CPU this is until this CPU's share has
    // run, for two reasons.
    //
    //  - The thread stays on this CPU. |self| is taken out of the targets and
    //    served here instead of being interrupted; a thread preempted and moved
    //    in between would serve the CPU it moved to, which also gets the IPI,
    //    and the CPU it left would never run |task| at all. For a TLB shootdown
    //    that CPU keeps the stale translation.
    //  - The share runs in the context |task| is promised: the other CPUs run
    //    theirs in their interrupt handlers, with interrupts off. Run here with
    //    them on, a task that takes a lock an interrupt handler also takes
    //    would deadlock against the first interrupt to arrive on this CPU.
    //
    // Disabling preemption alone would give the first and not the second.
    //
    // Back on before the wait, which has to be able to take the other CPUs'
    // requests: two CPUs each waiting on the other with interrupts off would
    // wait for ever. Nothing in the wait depends on which CPU this is.
    //
    // Zircon's mp_sync_exec() does the same, with arch_interrupt_save() before
    // arch_curr_cpu_num() and arch_interrupt_restore() after the local task.
    // Linux's smp_call_function_many() asks its caller to hold preemption off,
    // and runs the local share with interrupts off (local_irq_save()).
    ::arch::AutoInterruptDisabler no_interrupts;
    auto const self = Cpu::id();
    auto const run_here = targets.test(self);

    // Only CPUs that can actually take the interrupt. A CPU that is not online
    // has no mailbox worth filling and would never clear its bit, which would
    // turn the wait below into a hang.
    targets &= Cpu::online_mask();
    targets.set(self, false);

    if (targets.none()) {
        if (run_here) {
            task(context);
        }
        return;
    }

    SyncCall call{.task = task, .context = context, .outstanding = targets};

    // One slot per CPU, on this stack beside the request. MAX_CPU of them
    // rather than one per target so that a slot can be indexed by CPU number;
    // at sixteen CPUs that is 384 bytes of a 16 KiB kernel stack. Zircon sizes
    // its equivalent array the same way.
    SyncSlot slots[MAX_CPU]{};

    // One mailbox lock at a time, never two at once, so two CPUs asking each
    // other cannot build a cycle.
    targets.for_each([&call, &slots](CpuNum cpu) {
        slots[cpu].call = &call;

        auto &mailbox = CurrentMailbox::get(cpu);
        Guard<sync::SpinLock, IrqSave> guard{mailbox.lock};
        mailbox.calls.push_back(slots[cpu]);
    });

    send_event(targets, Event::Generic);

    // The caller's own share, run here rather than sent: a CPU cannot service
    // its own IPI while it is standing in this function.
    if (run_here) {
        task(context);
    }
    no_interrupts.reenable();

    while (call.outstanding.load(std::memory_order_acquire).any()) {
        arch::spin_hint();
    }
}

auto IpiService::handle_generic() -> void {
    auto &mailbox = CurrentMailbox::get();
    auto const self_mask = ~CpuMask::of(Cpu::id());

    for (;;) {
        SyncCall *call = nullptr;
        {
            // Interrupts are already off in the handler; IrqSave states the
            // lock's rule here as well, see Mailbox.
            Guard<sync::SpinLock, IrqSave> guard{mailbox.lock};
            if (mailbox.calls.empty()) {
                return;
            }
            // The slot is read out here and not held: it lives on the caller's
            // stack next to the request, and both go away the moment the last
            // bit clears below.
            call = mailbox.calls.front().value().call;
            mailbox.calls.pop_front();
        }

        call->task(call->context);

        // Release, and a CAS loop because std::atomic<CpuMask> has no
        // fetch_and: two CPUs answering the same request at once would
        // otherwise each store a mask still carrying the other's bit, and the
        // caller would wait for an answer that had already been given.
        //
        // This store is also the last time this CPU may touch |call|. The
        // caller's frame -- which is where |call| lives -- is free to go the
        // instant the final bit clears.
        auto expected = call->outstanding.load(std::memory_order_relaxed);
        for (;;) {
            auto const desired = expected & self_mask;
            if (call->outstanding.compare_exchange_weak(
                    expected, desired, std::memory_order_release, std::memory_order_relaxed)) {
                break;
            }
        }
    }
}

} // namespace ours
