#include <ours/assert.hpp>
#include <ours/lockdep.hpp>
#include <ours/task/auto_preempt_disabler.hpp>

#include <kernel/kernel/irq/line.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::irq {
Line::Line(HardwareIrq const source, IrqId const id)
    : m_canary(this), m_flags(LineFlags::None), m_source(source, id) {}

/// Refusing a second call keeps the controller what mask() and unmask(), which read
/// it without a lock, rely on: fixed once the line is published.
///
/// The flow is chosen here, once, from how the pin signals as the line is
/// bound, rather than by asking the controller on every interrupt: on the IO-APIC
/// that would be a register read under LI in every dispatch. The line table
/// is built after the platform has programmed its pins, so the mode read
/// here is the one the pin delivers with.
///
/// Nobody else can reach the line yet, so L1 is never contended here; it is
/// taken because the flow is written under it everywhere else, and the read
/// goes L1 -> LI, the order attach() takes.
auto Line::init(ControllerOps const &controller, void *const context) -> Status {
    if (m_source.controller) {
        return Status::BadState;
    }
    if (!controller.is_valid()) {
        return Status::InvArgs;
    }
    m_source.controller = &controller;
    m_source.context = context;

    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    // A controller that cannot say how its pins signal gets the edge flow, which
    // asks nothing of the controller around the registrations.
    auto trigger = TriggerMode::Edge;
    if (controller.trigger_mode) {
        trigger = controller.trigger_mode(m_source);
    }
    m_trigger = trigger;
    return Status::Ok;
}

/// The flags are the line's, and an attach only adds to them. This used to
/// assign the word outright on the first attach, `irqflags_ = Shared or
/// None`, which wiped whatever else the line had stored -- Permanent above
/// all, so the timer's request for a permanent line was dropped on the floor.
/// Now Shared alone follows the line's occupants (set by the first, cleared
/// by detach() when the last leaves), and Permanent, once set, stays.
auto Line::attach(Registration &registration) -> Status {
    if (auto const status = check_registration(registration); status != Status::Ok) {
        return status;
    }
    // Only the level flow holds a Oneshot line masked. With no mode the line
    // keeps the flow it has, chosen when it was bound or by the last configure
    // or attach with a mode, and a controller that cannot say how its pins signal
    // gave it the edge flow. So such a controller's Oneshot registration is refused here:
    // this looks at what the controller can say, not at the flow the line has now,
    // and the registration needs a mode (the other attach). The controller is fixed before
    // the line is published, so it is read without the lock.
    if (registration.is_oneshot() &&
        (m_source.controller == nullptr || !m_source.controller->trigger_mode)) {
        return Status::NotSupported;
    }

    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    // A line the kernel is taking away takes nobody new.
    if (is_dying_locked()) {
        return Status::NoResource;
    }
    // An registration hangs on one line at a time: its hook has room for one
    // list, and pushing a linked hook again would corrupt both lists.
    if (registration.is_attached()) {
        return Status::BadState;
    }

    auto const flags = m_flags;
    // A Permanent registration owns the line for its entire lifetime.
    if (!!(flags & LineFlags::Permanent)) {
        return Status::AlreadyBound;
    }
    if (!m_registrations.empty()) {
        // Joining an occupied line needs both sides to agree to share, and a
        // Permanent registration never shares: it would freeze the others on the
        // line along with itself.
        auto const both_share = !!(flags & LineFlags::Shared) && registration.is_shared();
        if (!both_share || registration.is_permanent()) {
            return Status::AlreadyBound;
        }
    }

    link_locked(registration, flags);
    return Status::Ok;
}

/// The check that the line is free, the mode and the link happen under one
/// hold of L1. That is the whole point: configure() refuses a line someone is
/// on, so once the registration is linked nobody can change the mode under it,
/// and nobody could between the check and the link either. Set by
/// configure() and then attach(), two holds of L1 with the lock free in
/// between, the mode a caller set could be overwritten by another caller's
/// configure() before its attach(), and the line taken in a mode its owner
/// never asked for.
///
/// The line is masked before the registration is linked: configure() leaves it
/// masked, and a read masks it here, so whichever way the mode is settled the
/// line raises nothing until the caller, now knowing how it signals, unmasks
/// it. Masking a line nobody is on disturbs no one.
///
/// The flow is chosen in the same hold, from the mode just set or read, so
/// the registration's first interrupt runs the flow of the mode it took the line
/// in.
auto Line::attach(Registration &registration, ustl::Option<LineMode> const mode)
    -> ktl::Result<TriggerMode> {
    if (auto const status = check_registration(registration); status != Status::Ok) {
        return ustl::err(status);
    }
    // The controller is fixed before the line is published, so it is read without
    // the lock, as mask() reads it.
    auto const *const controller = m_source.controller;
    if (mode.is_some()) {
        if (!controller || !controller->configure) {
            return ustl::err(Status::NotSupported);
        }
    } else if (!controller || !controller->trigger_mode || !controller->mask) {
        return ustl::err(Status::NotSupported);
    }

    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    // Before the controller is touched: the mode of a line being taken away is
    // not ours to set.
    if (is_dying_locked()) {
        return ustl::err(Status::NoResource);
    }
    if (registration.is_attached()) {
        return ustl::err(Status::BadState);
    }
    // Only an empty line: its mode is about to be set or read for this
    // registration, and the mode of a line others are on is theirs. A Permanent
    // line always has its registration; the flag is tested for what it says.
    auto const flags = m_flags;
    if (!!(flags & LineFlags::Permanent) || !m_registrations.empty()) {
        return ustl::err(Status::AlreadyBound);
    }

    auto trigger = TriggerMode::Edge;
    if (mode.is_some()) {
        if (auto const status = controller->configure(m_source, mode->trigger, mode->polarity);
            status != Status::Ok) {
            return ustl::err(status);
        }
        trigger = mode->trigger;
    } else {
        controller->mask(m_source);
        trigger = controller->trigger_mode(m_source);
    }
    m_trigger = trigger;

    link_locked(registration, flags);
    return ustl::ok(trigger);
}

/// Each refusal is a promise Oneshot could not keep:
/// - Permanent registrations are reserved for exclusive inline kernel work.
/// - Shared: the line keeps one held bit, which records one thread's debt.
///   With two Oneshot registrations on one line, the second one's finalize would
///   unmask the line while the first one's thread still owed its device a
///   service.
/// - A controller that cannot mask and unmask: the level flow masks a Oneshot line
///   before its registration and unmasks it after, and finalize unmasks it.
///
/// The flags and the handler are fixed at construction, and the controller before
/// the line is published, so none of this needs L1.
auto Line::check_registration(Registration const &registration) const -> Status {
    if (!registration.is_valid()) {
        return Status::InvArgs;
    }
    if (!registration.is_oneshot()) {
        return Status::Ok;
    }
    auto const *const controller = m_source.controller;
    if (!controller || !controller->mask || !controller->unmask ||
        controller->masking != Masking::Synchronous) {
        return Status::NotSupported;
    }
    return Status::Ok;
}

/// The first registration decides whether the line shares; one that joins later
/// got here only because both it and the line said Shared. Permanent, once
/// set, stays.
///
/// Oneshot is the line's for as long as its registration is on it. A Oneshot
/// registration never shares, so it only ever takes an empty line, and detach()
/// clears the flag as the line empties.
auto Line::link_locked(Registration &registration, LineFlags flags) -> void {
    if (m_registrations.empty() && registration.is_shared()) {
        flags |= LineFlags::Shared;
    }

    m_registrations.push_back(registration);
    registration.m_line.store(m_source.id, std::memory_order_release);

    if (registration.is_permanent()) {
        flags |= LineFlags::Permanent;
    }
    if (registration.is_oneshot()) {
        flags |= LineFlags::Oneshot;
    }
    m_flags = flags;
}

/// Taking the lock is the barrier. handle_irq() holds it while the handlers run,
/// so this cannot unlink the registration while its handler runs on another CPU,
/// and after the unlink no dispatch can reach the registration again. When this
/// returns, the owner may free the registration and whatever its context points
/// to.
///
/// The same hold clears held. A Oneshot line has one registration, this one, so
/// whatever the line still owes is this registration's thread's, and it goes with
/// the registration rather than waiting for a finalize: the thread may be a user
/// thread that never acks, and the line must not stay held against the next
/// owner, which would then find every interrupt it handles itself leaving
/// the line masked.
///
/// When the last registration goes, the line is masked under the same hold: with
/// nobody on it, nothing should raise it until the next owner, having
/// attached, unmasks it. Left unmasked, a level pin whose device still
/// asserts it would raise an interrupt nobody handles before the flow masked
/// it; a held line is masked already, and stays so though held is gone.
auto Line::detach(Registration &registration) -> Status {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    // erase() trusts that the registration is in this very list; erasing one that
    // is not would unlink it from whichever list it is on, or from none.
    if (registration.line_id() != m_source.id) {
        return Status::NotFound;
    }

    auto flags = m_flags;
    // Permanent registration is an explicit lifetime policy.
    if (!!(flags & LineFlags::Permanent)) {
        return Status::AlreadyBound;
    }

    m_registrations.erase(registration);
    registration.m_line.store(IrqId::Invalid, std::memory_order_release);
    // Held and, on a line left empty, the facts its last registration brought go
    // out in one store.
    flags &= ~LineFlags::Held;
    auto const emptied = m_registrations.empty();
    if (emptied) {
        flags &= ~(LineFlags::Shared | LineFlags::Oneshot);
    }
    m_flags = flags;
    if (emptied) {
        // Every line of the table has a controller that masks; only a line of a
        // test's own may have no controller at all.
        auto const *const controller = m_source.controller;
        if (controller && controller->mask) {
            controller->mask(m_source);
        }
    }
    return Status::Ok;
}

/// Under L1, so it lands wholly before or after a dispatch of the line: never
/// between the flow's check of held and its unmask, and never between a
/// detach's unlink and its mask.
///
/// The line is named by the caller rather than read from the registration: the
/// registration's line is written under that line's L1, which is only held here,
/// and a finalize races a detach of the same registration when one thread acks
/// while another destroys. Under this L1 the registration is either still on this
/// line, and owes what the line holds, or gone, and the detach has cleared
/// held and masked the line for it: then there is nothing to finalize, and
/// unmasking would hand the next owner a line it has not unmasked itself.
///
/// Completion revalidates the owner's current work while L1 excludes another
/// dispatch. The callback uses the same lock order as its handler. If a newer
/// dispatch established work after the caller released its own lock, the line
/// remains held until that work's completion. No event counter or second copy
/// of the owner's debt is needed.
///
/// A line marked for removal is finalized all the same. Skipping it would
/// leave the line held and masked if the removal is then refused and the mark
/// taken off again, with its object idle and nothing to raise the next
/// interrupt; and if the removal goes through, revoke() clears held and masks
/// the line last, under this same lock.
auto Line::finalize_oneshot(Registration const &registration) -> void {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    if (registration.line_id() != m_source.id || !is_oneshot_locked()) {
        return;
    }
    if (!registration.ready_to_complete()) {
        return;
    }
    set_held_locked(false);
    // check_registration() let the registration on only with a controller that unmasks.
    m_source.controller->unmask(m_source);
}

namespace {
    /// The controller holds back every interrupt of this one's priority and
    /// below on the CPU until it is ended, so each interrupt must be ended
    /// exactly once, on whichever path its flow returns by: a path that
    /// forgot would silence the CPU's devices, and a second end would end an
    /// interrupt still in service beneath this one. A guard makes that a
    /// matter of scope rather than of each return path.
    ///
    /// The controller is read without the lock, as Line::mask() reads it: it is
    /// fixed before the line is published. A line of the table always has a
    /// controller with an eoi (Domain::init()); a line of a test's own, outside
    /// the table, may have none, and is dispatched with Skip.
    class EoiGuard final {
      public:
        [[nodiscard]]
        EoiGuard(ControllerSource const &data, Eoi const eoi)
            : m_source(data), m_eoi(eoi) {}

        USTL_NO_MOVEABLE_AND_COPYABLE(EoiGuard);

        ~EoiGuard() {
            if (m_eoi == Eoi::Skip) {
                return;
            }
            auto const *const controller = m_source.controller;
            DEBUG_ASSERT(controller && controller->issue_eoi,
                         "line {} ends an interrupt with no controller eoi",
                         static_cast<u32>(m_source.id));
            controller->issue_eoi(m_source);
        }

      private:
        ControllerSource const &m_source;
        Eoi const m_eoi;
    };
} // namespace

/// Notify every registration under L1. Deferred work outranks synchronous
/// handling; an unclaimed registration cannot erase another handler's result.
auto Line::notify_locked() -> IrqReturn {
    auto result = IrqReturn::None;
    for (auto &registration: m_registrations) {
        auto const answer = registration.on_event();
        if (answer == IrqReturn::WakeThread) {
            result = IrqReturn::WakeThread;
        } else if (answer == IrqReturn::Handled && result == IrqReturn::None) {
            result = IrqReturn::Handled;
        }
    }
    return result;
}

/// The Guard serializes state and callback lifetime on every line. Preemption
/// stays off through callbacks even when a test dispatches from thread context.
auto Line::handle_irq(Eoi const eoi) -> IrqReturn {
    Guard<sync::SpinLock, IrqSaveAndPreemptDisable> const guard{m_lock};
    auto const result = dispatch_locked(eoi);

    // An interrupt no registration claimed is counted per line; WakeThread claims
    // it as Handled does. A line whose registrations keep answering None is only
    // counted, never shut off; a line with no registration at all has been masked
    // by its flow.
    if (result == IrqReturn::None) {
        m_spurious.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

/// Handshake and electrical holding policy are independent. The scope guard
/// completes even an unclaimed source after any mask or deferred-work update.
auto Line::dispatch_locked(Eoi const eoi) -> IrqReturn {
    if (eoi == Eoi::Send && m_source.controller->protocol == DispatchProtocol::AckEoi) {
        m_source.controller->issue_ack(m_source);
    }
    EoiGuard const completion{m_source, eoi};
    if (m_registrations.empty()) {
        mask_unclaimed();
        return IrqReturn::None;
    }
    if (m_trigger == TriggerMode::Level && is_oneshot_locked()) {
        return handle_oneshot_locked();
    }
    return notify_locked();
}

/// The line is masked before the registration runs. The registration's handler answers
/// WakeThread when it leaves the device to a thread: the device still
/// asserts the pin, and nothing will deassert it until the thread has
/// serviced it, so the line must not be unmasked when this interrupt is
/// ended. The handler only wakes the thread; it is the line that remembers
/// the debt, in held, and keeps the line masked until finalize_oneshot().
///
/// An interrupt the registration handles itself, Handled or None, owes nothing, and
/// the line is unmasked again before the end of interrupt -- unless an
/// earlier interrupt left it held, whose thread still owes its service. The
/// end of interrupt stays the flow's last step, as on every path: on the
/// IO-APIC the pin's Remote IRR holds back a level pin still asserted until
/// that end, so unmasking first raises nothing early.
///
/// Mask, registrations, unmask and end of interrupt all run under the one hold of
/// L1, which is what keeps finalize_oneshot() and detach(), which take it
/// too, from landing between the check of held and the unmask.
auto Line::handle_oneshot_locked() -> IrqReturn {
    // check_registration() let the registration on only with a controller that masks and
    // unmasks.
    auto const &controller = *m_source.controller;
    controller.mask(m_source);
    auto const result = notify_locked();
    if (result == IrqReturn::WakeThread) {
        set_held_locked(true);
    }
    if (!is_held_locked()) {
        controller.unmask(m_source);
    }
    return result;
}

/// The controller is read without the lock, as mask() reads it: it is fixed before
/// the line is published. A line of the table always has a controller that masks
/// (Domain::init()); a line of a test's own, outside the table, needs one
/// too before it is dispatched with nobody on it.
auto Line::mask_unclaimed() const -> void {
    auto const *const controller = m_source.controller;
    DEBUG_ASSERT(controller && controller->mask,
                 "line {} masks an unclaimed interrupt with no controller mask",
                 static_cast<u32>(m_source.id));
    controller->mask(m_source);
}

/// Under L1, like every other write of the line's mask -- the flow's,
/// finalize_oneshot()'s, detach()'s, configure()'s and attach()'s, with a
/// mode or without -- so the writes land in the order L1 gives them. Without
/// it, a mask could land between a Oneshot line's flow masking the line and
/// unmasking it after a registration that handled the interrupt itself, and the
/// flow's unmask would undo it at once.
///
/// The controller and its operations are fixed before the line is published, so
/// they are read before the lock is taken, as configure() reads them.
auto Line::mask() -> Status {
    auto const *const controller = m_source.controller;
    if (!controller || !controller->mask) {
        return Status::NotSupported;
    }
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    controller->mask(m_source);
    return Status::Ok;
}

/// Held is read under the same hold of L1 as the unmask it decides. Read
/// without it, the check would be check-then-act against the flow: a
/// dispatch on another CPU could hold the line between the read and the
/// unmask, and the unmask would open a level pin whose device nobody has
/// serviced, which raises the same interrupt again as soon as it is ended.
///
/// A held line answers Ok: the caller wants the line to run, and it will,
/// as soon as finalize_oneshot() says its thread is done. Nothing the caller
/// could do with a refusal would make it run sooner, and the owner that
/// starts its line after attach() must not take a refusal for a failure: an
/// interrupt played on the line in between can have held it already.
auto Line::unmask() -> Status {
    auto const *const controller = m_source.controller;
    if (!controller || !controller->unmask) {
        return Status::NotSupported;
    }
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    // A line being taken away is not started, held or not.
    if (is_dying_locked()) {
        return Status::NoResource;
    }
    if (is_held_locked()) {
        return Status::Ok;
    }
    controller->unmask(m_source);
    return Status::Ok;
}

/// Refused on a line that has a registration: changing how a live line signals
/// under its handler would hand it interrupts it was not set up for, or
/// none. Holding L1 keeps an attach from slipping in between the check and
/// the write. It does not keep another caller's configure() from landing
/// between this and a later attach(), which is why an owner sets the mode
/// through attach() with a mode instead.
auto Line::configure(TriggerMode const trigger, Polarity const polarity) -> Status {
    auto const *const controller = m_source.controller;
    if (!controller || !controller->configure) {
        return Status::NotSupported;
    }

    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    if (is_dying_locked()) {
        return Status::NoResource;
    }
    // A Permanent line always has its registration; the flag is tested for what
    // it says, not as a stand-in for the list.
    auto const flags = m_flags;
    if (!!(flags & LineFlags::Permanent) || !m_registrations.empty()) {
        return Status::AlreadyBound;
    }
    auto const status = controller->configure(m_source, trigger, polarity);
    // The flow follows the mode only once the controller has taken it: a refused
    // configure leaves the pin, and so the flow, as they were.
    if (status == Status::Ok) {
        m_trigger = trigger;
    }
    return status;
}

/// Asked of the controller rather than answered from the line's flow: the
/// controller's entry is the record of how the pin signals, the one that
/// decides how the pin is read. The line keeps only the flow chosen from it,
/// and keeps the two in step: the table is built after the platform has
/// programmed the pins, and from then on the mode changes only through
/// configure() and attach() with a mode, which choose the flow again under L1.
auto Line::trigger_mode() const -> ktl::Result<TriggerMode> {
    auto const *const controller = m_source.controller;
    if (!controller || !controller->trigger_mode) {
        return ustl::err(Status::NotSupported);
    }
    return ustl::ok(controller->trigger_mode(m_source));
}

/// Asked of the controller, as trigger_mode() is: the controller's entry is the one
/// record of the mask, and the line keeps no copy of it to answer from.
auto Line::is_masked() const -> ktl::Result<bool> {
    auto const *const controller = m_source.controller;
    if (!controller || !controller->is_masked) {
        return ustl::err(Status::NotSupported);
    }
    return ustl::ok(controller->is_masked(m_source));
}

/// Permanent registrations retain their line for the kernel's lifetime.
/// Other registrations opt into revocation by supplying a cancel hook.
auto Line::revocable_locked() const -> Status {
    if (!!(m_flags & LineFlags::Permanent)) {
        return Status::AlreadyBound;
    }
    for (auto const &registration: m_registrations) {
        if (!registration.is_revocable()) {
            return Status::AlreadyBound;
        }
    }
    return Status::Ok;
}

auto Line::check_revocable() -> Status {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    return revocable_locked();
}

/// The check and the mark are one hold of L1, so an attach that lands
/// between a check_revocable() and this cannot make the removal take an
/// registration that was not there to be asked.
auto Line::mark_dying() -> Status {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    if (auto const status = revocable_locked(); status != Status::Ok) {
        return status;
    }
    m_flags |= LineFlags::Dying;
    return Status::Ok;
}

auto Line::has_registrations() -> bool {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    return !m_registrations.empty();
}

auto Line::unmark_dying() -> void {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    m_flags &= ~LineFlags::Dying;
}

/// A revocation callback may wake a thread. The Guard defers its reschedule
/// until L1 has been released, as the dispatch path does.
///
/// The order within one registration is the point. It is unlinked first, so no
/// dispatch can reach it again, and the lock is held, so none is in flight;
/// then told; and only then is its line cleared. An owner that destroys its
/// object meanwhile reaches irq::detach(), which either reads the line still
/// valid and queues on L1 behind this, or reads INVALID, which was stored
/// after the hook returned and after the last touch of the registration, so
/// seeing it means this is done with the registration and its owner. Clearing the
/// line before telling would let the owner see INVALID, finish, and free
/// the registration under the hook.
///
/// Held goes with the registrations, as in detach(): a Oneshot line's thread
/// owes nothing to an owner that has been told it is gone. The line is
/// masked last, through the controller, which the table's lines all have.
auto Line::revoke() -> void {
    Guard<sync::SpinLock, IrqSaveAndPreemptDisable> const guard{m_lock};
    auto flags = m_flags;
    // Revocation is only valid after every registration agreed to it.
    ASSERT(!!(flags & LineFlags::Dying), "line {} revoked before it was marked",
           static_cast<u32>(m_source.id));
    ASSERT(!(flags & LineFlags::Permanent), "line {} is Permanent and revoked",
           static_cast<u32>(m_source.id));

    for (;;) {
        auto const first = m_registrations.front();
        if (first.is_none()) {
            break;
        }
        auto &registration = *first;
        ASSERT(registration.is_revocable(), "line {} revokes a registration with no cancel hook",
               static_cast<u32>(m_source.id));
        m_registrations.erase(registration);
        registration.notify_revoked();
        registration.m_line.store(IrqId::Invalid, std::memory_order_release);
    }

    flags &= ~(LineFlags::Held | LineFlags::Shared | LineFlags::Oneshot);
    m_flags = flags;
    auto const *const controller = m_source.controller;
    if (controller && controller->mask) {
        controller->mask(m_source);
    }
}

auto Line::is_kernel_owned() -> bool {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    return !!(m_flags & LineFlags::Permanent);
}

auto Line::dispatch_trigger() -> TriggerMode {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    return m_trigger;
}

} // namespace ours::irq
