#include <ours/irq/mod.hpp>
#include <ours/irq/registration.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/task/irq_rcu.hpp>
#include <ours/task/thread.hpp>

#include <atomic>

#include <kernel/kernel/irq/domain.hpp>
#include <kernel/kernel/irq/domain_registry.hpp>
#include <kernel/kernel/irq/line_access.hpp>

namespace ours::irq {
namespace {
    // Trap frame of the interrupt currently being handled on this CPU, or
    // nullptr outside interrupt context. Instrumentation (e.g. the sampling
    // profiler's tick hook) reads it to reach the interrupted context.
    CPU_LOCAL
    arch::IrqFrame *CURRENT_FRAME;

    // The root controller's handler. Written once by set_root_handler() on the
    // boot CPU, in the controller's start-up hook, which runs before that CPU
    // enables interrupts and before any other CPU starts; read in interrupt
    // context on every CPU with no lock. That order is what keeps a CPU from
    // reading it too early. The release store and the acquire load make the
    // line table, built before the store, visible to whoever reads a handler.
    std::atomic<RootHandler> ROOT_HANDLER{nullptr};

    // The machine's directory of interrupt domains. The root controller's
    // driver registers its domain in it (register_domain()); every control call
    // resolves its number through it, inside a read guard.
    FORCE_INLINE
    auto get_domain_registry() -> DomainRegistry * {
        static DomainRegistry REGISTRY;
        return &REGISTRY;
    }

    /// The table that owns `virqnum`, `guard` held over it and over the caller's
    /// whole use of the answer: Domain::find_line()'s contract, one directory
    /// up. InvArgs for INVALID_V_IRQ_NUM, NoResource for a number no registered
    /// domain owns.
    [[nodiscard]]
    auto find_table(VIrqNum const virqnum, task::IrqRcuReadGuard const &guard)
        -> ktl::Result<Domain *> {
        if (virqnum == INVALID_V_IRQ_NUM) {
            return ustl::err(Status::InvArgs);
        }
        auto *const table = get_domain_registry()->table_of(virqnum, guard);
        if (table == nullptr) {
            return ustl::err(Status::NoResource);
        }
        return ustl::ok(table);
    }
} // namespace

auto set_root_handler(RootHandler const handler) -> void {
    ASSERT(handler != nullptr, "The root interrupt handler is null");
    auto expected = RootHandler{nullptr};
    [[maybe_unused]]
    auto const installed = ROOT_HANDLER.compare_exchange_strong(
        expected, handler, std::memory_order_release, std::memory_order_relaxed);
    ASSERT(installed, "A root interrupt handler is already installed");
}

auto handle_root_irq(::arch::IrqFrame *const frame) -> void {
    auto *const handler = ROOT_HANDLER.load(std::memory_order_acquire);
    ASSERT(handler != nullptr, "An interrupt reached the root handler before one was installed");
    handler(frame);
}

auto current_frame() -> arch::IrqFrame * {
    return mm::CpuLocal::read(CURRENT_FRAME);
}

auto register_domain(DomainId const id, ustl::views::Span<HIrqNum const> const sources,
                     ControllerOps const &controller, DomainEndpoint &endpoint, void *const context)
    -> Status {
    return get_domain_registry()->register_domain(id, sources, controller, endpoint, context);
}

auto unregister_domain(DomainId const id) -> Status {
    return get_domain_registry()->unregister_domain(id);
}

auto attach(VIrqNum const virqnum, Registration &action) -> Status {
    return with_line(*get_domain_registry(), virqnum,
                     [&](Line &line) { return line.attach(action); });
}

auto attach(VIrqNum const virqnum, Registration &action, ustl::Option<LineMode> const mode)
    -> ktl::Result<TriggerMode> {
    return with_line(*get_domain_registry(), virqnum,
                     [&](Line &line) { return line.attach(action, mode); });
}

/// The action knows its line, so the caller names only the action: on a
/// shared line a number alone cannot say which of its actions to remove.
/// INVALID_V_IRQ_NUM, which is what an action not on any line holds, is in no
/// domain's block and answers NotFound, as it did when the machine had one
/// table.
auto detach(Registration &action) -> Status {
    task::IrqRcuReadGuard const guard;
    auto *const table = get_domain_registry()->table_of(action.virqnum(), guard);
    if (table == nullptr) {
        return Status::NotFound;
    }
    return table->detach(action);
}

auto finalize_oneshot(VIrqNum const virqnum, Registration const &action) -> void {
    task::IrqRcuReadGuard const guard;
    auto *const table = get_domain_registry()->table_of(virqnum, guard);
    if (table == nullptr) {
        return;
    }
    table->finalize_oneshot(virqnum, action);
}

auto mask(VIrqNum const virqnum) -> Status {
    return with_line(*get_domain_registry(), virqnum, [&](Line &line) { return line.mask(); });
}

auto unmask(VIrqNum const virqnum) -> Status {
    return with_line(*get_domain_registry(), virqnum, [&](Line &line) { return line.unmask(); });
}

auto configure(VIrqNum const virqnum, TriggerMode const trigger, Polarity const polarity)
    -> Status {
    return with_line(*get_domain_registry(), virqnum,
                     [&](Line &line) { return line.configure(trigger, polarity); });
}

auto trigger_mode(VIrqNum const virqnum) -> ktl::Result<TriggerMode> {
    return with_line(*get_domain_registry(), virqnum,
                     [&](Line &line) { return line.trigger_mode(); });
}

auto is_masked_for_test(VIrqNum const virqnum) -> ktl::Result<bool> {
    return with_line(*get_domain_registry(), virqnum, [&](Line &line) { return line.is_masked(); });
}


auto virq_of_domain(DomainId const id, HIrqNum const hirqnum) -> VIrqNum {
    // One read of the directory, and the answer is a plain number: good after
    // the guard closes, because a virq is never reused.
    task::IrqRcuReadGuard const guard;
    return get_domain_registry()->virq_of(id, hirqnum, guard);
}

auto virq_of_hwirq(HIrqNum const hirqnum) -> VIrqNum {
    return virq_of_domain(DomainId::Root, hirqnum);
}

auto hwirq_of_virq(VIrqNum const virqnum) -> HIrqNum {
    task::IrqRcuReadGuard const guard;
    auto *const table = get_domain_registry()->table_of(virqnum, guard);
    if (table == nullptr) {
        return INVALID_H_IRQ_NUM;
    }
    return table->hwirq_of(virqnum);
}

/// A played interrupt names its line by virq, as every caller outside the
/// interrupt entry does, and a thread-context entrant into the interrupt path
/// is a read of the directory like any control call: the guard covers the
/// lookup and the flow both. The virq is turned into the hardware number
/// inside the table; a number that is no line becomes INVALID_H_IRQ_NUM,
/// which has no line.
auto handle_irq_for_test(VIrqNum const virqnum) -> IrqReturn {
    task::IrqRcuReadGuard const guard;
    auto *const table = get_domain_registry()->table_of(virqnum, guard);
    if (table == nullptr) {
        return IrqReturn::None;
    }
    auto const result = table->handle_irq(table->hwirq_of(virqnum), Eoi::Skip);
    if (result.is_none()) {
        return IrqReturn::None;
    }
    return *result;
}

auto revoke_line_for_test(VIrqNum const virqnum) -> Status {
    task::IrqRcuReadGuard const guard;
    auto const table = find_table(virqnum, guard);
    if (table.is_err()) {
        return table.unwrap_err();
    }
    return (*table)->revoke_line(virqnum);
}

auto unmark_dying_for_test(VIrqNum const virqnum) -> void {
    task::IrqRcuReadGuard const guard;
    auto *const table = get_domain_registry()->table_of(virqnum, guard);
    if (table == nullptr) {
        return;
    }
    table->unmark_dying(virqnum);
}

auto spurious_count(VIrqNum const virqnum) -> ustl::Option<u64> {
    task::IrqRcuReadGuard const guard;
    auto *const table = get_domain_registry()->table_of(virqnum, guard);
    if (table == nullptr) {
        return ustl::none();
    }
    return table->spurious_count(virqnum);
}

auto is_kernel_owned(VIrqNum const virqnum) -> bool {
    task::IrqRcuReadGuard const guard;
    auto *const table = get_domain_registry()->table_of(virqnum, guard);
    if (table == nullptr) {
        return false;
    }
    return table->is_kernel_owned(virqnum);
}

auto root_source_exists_for_test(HIrqNum const source) -> bool {
    task::IrqRcuReadGuard const guard;
    return get_domain_registry()->virq_of(DomainId::Root, source, guard) != INVALID_V_IRQ_NUM;
}

/// The bookkeeping on entry to an interrupt: record the frame, and raise the
/// counts that keep this CPU from switching threads until the exit. Runs for
/// every vector that takes the bookkeeping -- device interrupts, IPIs and
/// exceptions such as #PF alike; arch_handle_exception() keeps NMI, #DF and
/// #MC out.
///
/// Raising the two disable counts is what makes the handler non-preemptible.
/// There is deliberately no hard-IRQ count beside them, and PreemptionState's
/// Tag says why. CURRENT_FRAME is this CPU's "handling an interrupt" signal
/// instead.
auto handle_irq_preamble(arch::IrqFrame *frame) -> void {
    mm::CpuLocal::write(CURRENT_FRAME, frame);

    auto &preemption_state = task::Thread::Current::preemption_state();
    preemption_state.disable_eager_reschedule();
    preemption_state.disable_preemption();
}

/// The bookkeeping on exit from an interrupt. The interrupt is over; what is
/// decided here is whether the CPU should switch threads before returning to
/// the interrupted context, and the deciding is all this does -- the switch
/// is the caller's (arch_handle_exception), after the interrupt frame is put
/// away.
///
/// Two things this used to get wrong. It called enable_preemption(), which
/// *flushes*: a pending local preemption was performed right here, inside
/// the interrupt glue, and then reported back so the caller preempted a
/// second time. And it reported is_preemption_enabled() alone, so with
/// preemption enabled every interrupt exit asked for a reschedule whether or
/// not anything was pending -- a full pass through reschedule_common() per
/// tick. Neither showed while is_preemption_enabled() was always false.
///
/// The order matters: eager reschedules come back first, which flushes only
/// the remote requests because the preempt count is still up; then the
/// preempt count drops without a flush (enable_preemption_delay_resched),
/// and the local request, if any, is taken and handed to the caller.
auto handle_irq_postamble() -> bool {
    mm::CpuLocal::write(CURRENT_FRAME, static_cast<arch::IrqFrame *>(nullptr));

    auto &preemption_state = task::Thread::Current::preemption_state();
    preemption_state.enable_eager_reschedule();
    preemption_state.enable_preemption_delay_resched();

    return preemption_state.is_preemption_enabled() && preemption_state.take_pending_local();
}

} // namespace ours::irq