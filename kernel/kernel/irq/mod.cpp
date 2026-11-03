#include <ours/irq/mod.hpp>
#include <ours/irq/registration.hpp>
#include <ours/task/irq_rcu.hpp>

#include <kernel/kernel/irq/domain.hpp>
#include <kernel/kernel/irq/domain_registry.hpp>
#include <kernel/kernel/irq/line_access.hpp>

namespace ours::irq {
namespace {
    // The machine's directory of interrupt domains. The root controller's
    // driver registers its domain in it (register_domain()); every control call
    // resolves its number through it, inside a read guard.
    FORCE_INLINE
    auto get_domain_registry() -> DomainRegistry * {
        static DomainRegistry REGISTRY;
        return &REGISTRY;
    }

    /// The table that owns `id`, `guard` held over it and over the caller's
    /// whole use of the answer: Domain::find_line()'s contract, one directory
    /// up. InvArgs for VIrqNum::Invalid, NoResource for a number no registered
    /// domain owns.
    [[nodiscard]]
    auto find_table(VIrqNum const id, task::IrqRcuReadGuard const &guard) -> ktl::Result<Domain *> {
        if (id == VIrqNum::Invalid) {
            return ustl::err(Status::InvArgs);
        }
        auto *const table = get_domain_registry()->domain_for(id, guard);
        if (table == nullptr) {
            return ustl::err(Status::NoResource);
        }
        return ustl::ok(table);
    }
} // namespace

auto register_domain(DomainId const id, ustl::views::Span<HIrqNum const> const sources,
                     ControllerOps const &controller, DomainEndpoint &endpoint, void *const context)
    -> Status {
    return get_domain_registry()->register_domain(id, sources, controller, endpoint, context);
}

auto unregister_domain(DomainId const id) -> Status {
    return get_domain_registry()->unregister_domain(id);
}

auto attach(VIrqNum const id, Registration &registration) -> Status {
    return with_line(*get_domain_registry(), id,
                     [&](Line &line) { return line.attach(registration); });
}

auto attach(VIrqNum const id, Registration &registration, ustl::Option<LineMode> const mode)
    -> ktl::Result<TriggerMode> {
    return with_line(*get_domain_registry(), id,
                     [&](Line &line) { return line.attach(registration, mode); });
}

/// The registration knows its line, so the caller names only the registration: on a
/// shared line a number alone cannot say which of its registrations to remove.
/// VIrqNum::Invalid, which is what an registration not on any line holds, is in no
/// domain's block and answers NotFound, as it did when the machine had one
/// table.
auto detach(Registration &registration) -> Status {
    task::IrqRcuReadGuard const guard;
    auto *const table = get_domain_registry()->domain_for(registration.line_id(), guard);
    if (table == nullptr) {
        return Status::NotFound;
    }
    return table->detach(registration);
}

auto finalize_oneshot(VIrqNum const id, Registration const &registration) -> void {
    task::IrqRcuReadGuard const guard;
    auto *const table = get_domain_registry()->domain_for(id, guard);
    if (table == nullptr) {
        return;
    }
    table->finalize_oneshot(id, registration);
}

auto mask(VIrqNum const id) -> Status {
    return with_line(*get_domain_registry(), id, [&](Line &line) { return line.mask(); });
}

auto unmask(VIrqNum const id) -> Status {
    return with_line(*get_domain_registry(), id, [&](Line &line) { return line.unmask(); });
}

auto configure(VIrqNum const id, TriggerMode const trigger, Polarity const polarity) -> Status {
    return with_line(*get_domain_registry(), id,
                     [&](Line &line) { return line.configure(trigger, polarity); });
}

auto trigger_mode(VIrqNum const id) -> ktl::Result<TriggerMode> {
    return with_line(*get_domain_registry(), id, [&](Line &line) { return line.trigger_mode(); });
}

auto is_masked_for_test(VIrqNum const id) -> ktl::Result<bool> {
    return with_line(*get_domain_registry(), id, [&](Line &line) { return line.is_masked(); });
}

auto irq_for_source(DomainId const id, HIrqNum const source) -> VIrqNum {
    // One read of the directory, and the answer is a plain number: good after
    // the guard closes, because a virq is never reused.
    task::IrqRcuReadGuard const guard;
    return get_domain_registry()->irq_for_source(id, source, guard);
}

auto source_for_irq(VIrqNum const id) -> HIrqNum {
    task::IrqRcuReadGuard const guard;
    auto *const table = get_domain_registry()->domain_for(id, guard);
    if (table == nullptr) {
        return HIrqNum::Invalid;
    }
    return table->source_for_irq(id);
}

/// A played interrupt names its line by virq, as every caller outside the
/// interrupt entry does, and a thread-context entrant into the interrupt path
/// is a read of the directory like any control call: the guard covers the
/// lookup and the flow both. The virq is turned into the hardware number
/// inside the table; a number that is no line becomes HIrqNum::Invalid,
/// which has no line.
auto handle_irq_for_test(VIrqNum const id) -> IrqReturn {
    task::IrqRcuReadGuard const guard;
    auto *const table = get_domain_registry()->domain_for(id, guard);
    if (table == nullptr) {
        return IrqReturn::None;
    }
    auto const result = table->handle_irq(table->source_for_irq(id), Eoi::Skip);
    if (result.is_none()) {
        return IrqReturn::None;
    }
    return *result;
}

auto revoke_line_for_test(VIrqNum const id) -> Status {
    task::IrqRcuReadGuard const guard;
    auto const table = find_table(id, guard);
    if (table.is_err()) {
        return table.unwrap_err();
    }
    return (*table)->revoke_line(id);
}

auto unmark_dying_for_test(VIrqNum const id) -> void {
    task::IrqRcuReadGuard const guard;
    auto *const table = get_domain_registry()->domain_for(id, guard);
    if (table == nullptr) {
        return;
    }
    table->unmark_dying(id);
}

auto spurious_count(VIrqNum const id) -> ustl::Option<u64> {
    task::IrqRcuReadGuard const guard;
    auto *const table = get_domain_registry()->domain_for(id, guard);
    if (table == nullptr) {
        return ustl::none();
    }
    return table->spurious_count(id);
}

auto is_kernel_owned(VIrqNum const id) -> bool {
    task::IrqRcuReadGuard const guard;
    auto *const table = get_domain_registry()->domain_for(id, guard);
    if (table == nullptr) {
        return false;
    }
    return table->is_kernel_owned(id);
}

auto root_source_exists_for_test(HIrqNum const source) -> bool {
    task::IrqRcuReadGuard const guard;
    return get_domain_registry()->irq_for_source(DomainId::Root, source, guard) != VIrqNum::Invalid;
}

} // namespace ours::irq
