#include <ours/assert.hpp>
#include <ours/irq/desc.hpp>
#include <ours/lockdep.hpp>

#include <heap/new.hpp>

namespace ours::irq {
Desc::Desc(HIrqNum hirq, VIrqNum virq, IrqFlags flags, char const *name)
    : m_canary(this)
    , m_lock()
    , m_flags(flags)
    , m_data(hirq, virq, nullptr /* No chip available */)
    , m_actions()
    , m_name(name) {}

auto Desc::create(HIrqNum hirq, VIrqNum virq, IrqFlags flags, char const *name) -> Self * {
    return new (mm::GAF_KERNEL) Self(hirq, virq, flags, name);
}

/// Refusing a second call keeps the chip what mask() and unmask(), which read
/// it without a lock, rely on: fixed once the line is published.
///
/// The flow is chosen here, once, from how the pin signals as the line is
/// bound, rather than by asking the chip on every interrupt: on the IO-APIC
/// that would be a register read under LI in every dispatch. The line table
/// is built after the platform has programmed its pins, so the mode read
/// here is the one the pin delivers with.
///
/// Nobody else can reach the line yet, so L1 is never contended here; it is
/// taken because the flow is written under it everywhere else, and the read
/// goes L1 -> LI, the order attach() takes.
auto Desc::init(Chip const &chip) -> Status {
    if (m_data.chip) {
        return Status::BadState;
    }
    m_data.chip = &chip;

    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    // A chip that cannot say how its pins signal gets the edge flow, which
    // asks nothing of the chip around the actions.
    auto trigger = TriggerMode::Edge;
    if (chip.trigger_mode) {
        trigger = chip.trigger_mode(m_data);
    }
    select_flow_locked(trigger);
    return Status::Ok;
}

/// The flags are the line's, and an attach only adds to them. This used to
/// assign the word outright on the first attach, `irqflags_ = Shared or
/// None`, which wiped whatever else the line had stored -- Permanent above
/// all, so the timer's request for a permanent line was dropped on the floor.
/// Now Shared alone follows the line's occupants (set by the first, cleared
/// by detach() when the last leaves), and Permanent, once set, stays.
auto Desc::attach(Action &action) -> Status {
    // The handler is fixed at construction, so it needs no lock to read.
    if (!action.m_handler) {
        return Status::InvArgs;
    }

    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    // An action hangs on one line at a time: its hook has room for one
    // list, and pushing a linked hook again would corrupt both lists.
    if (action.is_attached()) {
        return Status::BadState;
    }

    // Only writers hold the lock, so under it a relaxed load sees the latest.
    auto const flags = m_flags.load(std::memory_order_relaxed);
    // A Permanent line takes nobody else, shared or not: its list is walked
    // without the lock, which is only safe while nothing changes it.
    if (!!(flags & IrqFlags::Permanent)) {
        return Status::AlreadyBound;
    }
    if (!m_actions.empty()) {
        // Joining an occupied line needs both sides to agree to share, and a
        // Permanent action never shares: it would freeze the others on the
        // line along with itself.
        auto const both_share = !!(flags & IrqFlags::Shared) && action.is_shared();
        if (!both_share || action.is_permanent()) {
            return Status::AlreadyBound;
        }
    }

    link_locked(action, flags);
    return Status::Ok;
}

/// The check that the line is free, the mode and the link happen under one
/// hold of L1. That is the whole point: configure() refuses a line someone is
/// on, so once the action is linked nobody can change the mode under it,
/// and nobody could between the check and the link either. Set by
/// configure() and then attach(), two holds of L1 with the lock free in
/// between, the mode a caller set could be overwritten by another caller's
/// configure() before its attach(), and the line taken in a mode its owner
/// never asked for.
///
/// The line is masked before the action is linked: configure() leaves it
/// masked, and a read masks it here, so whichever way the mode is settled the
/// line raises nothing until the caller, now knowing how it signals, unmasks
/// it. Masking a line nobody is on disturbs no one.
///
/// The flow is chosen in the same hold, from the mode just set or read, so
/// the action's first interrupt runs the flow of the mode it took the line
/// in.
auto Desc::attach(Action &action, ustl::Option<LineMode> const mode) -> ktl::Result<TriggerMode> {
    if (!action.m_handler) {
        return ustl::err(Status::InvArgs);
    }
    // The chip is fixed before the line is published, so it is read without
    // the lock, as mask() reads it.
    auto const *const chip = m_data.chip;
    if (mode.is_some()) {
        if (!chip || !chip->configure) {
            return ustl::err(Status::NotSupported);
        }
    } else if (!chip || !chip->trigger_mode || !chip->mask) {
        return ustl::err(Status::NotSupported);
    }

    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    if (action.is_attached()) {
        return ustl::err(Status::BadState);
    }
    // Only an empty line: its mode is about to be set or read for this
    // action, and the mode of a line others are on is theirs. A Permanent
    // line always has its action; the flag is tested for what it says.
    auto const flags = m_flags.load(std::memory_order_relaxed);
    if (!!(flags & IrqFlags::Permanent) || !m_actions.empty()) {
        return ustl::err(Status::AlreadyBound);
    }

    auto trigger = TriggerMode::Edge;
    if (mode.is_some()) {
        if (auto const status = chip->configure(m_data, mode->trigger, mode->polarity);
            status != Status::Ok) {
            return ustl::err(status);
        }
        trigger = mode->trigger;
    } else {
        chip->mask(m_data);
        trigger = chip->trigger_mode(m_data);
    }
    select_flow_locked(trigger);

    link_locked(action, flags);
    return ustl::ok(trigger);
}

auto Desc::select_flow_locked(TriggerMode const trigger) -> void {
    if (trigger == TriggerMode::Level) {
        m_flow = Flow::FastEoi;
    } else {
        m_flow = Flow::Edge;
    }
}

/// The first action decides whether the line shares; one that joins later
/// got here only because both it and the line said Shared. Permanent, once
/// set, stays.
auto Desc::link_locked(Action &action, IrqFlags flags) -> void {
    if (m_actions.empty() && action.is_shared()) {
        flags |= IrqFlags::Shared;
    }

    m_actions.push_back(action);
    action.m_virqnum = m_data.virqnum;

    if (action.is_permanent()) {
        flags |= IrqFlags::Permanent;
    }
    // Release, last: a dispatch that reads Permanent without the lock must
    // find the action already linked.
    m_flags.store(flags, std::memory_order_release);
}

/// Taking the lock is the barrier. handle_irq() holds it while the handlers run,
/// so this cannot unlink the action while its handler runs on another CPU,
/// and after the unlink no dispatch can reach the action again. When this
/// returns, the owner may free the action and whatever its context points
/// to.
auto Desc::detach(Action &action) -> Status {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    // erase() trusts that the action is in this very list; erasing one that
    // is not would unlink it from whichever list it is on, or from none.
    if (action.m_virqnum != m_data.virqnum) {
        return Status::NotFound;
    }

    auto flags = m_flags.load(std::memory_order_relaxed);
    // A Permanent line dispatches without the lock, so taking it is no
    // barrier there, and unlinking would race a dispatch walking the list.
    if (!!(flags & IrqFlags::Permanent)) {
        return Status::AlreadyBound;
    }

    m_actions.erase(action);
    action.m_virqnum = INVALID_V_IRQ_NUM;
    if (m_actions.empty()) {
        flags &= ~IrqFlags::Shared;
        m_flags.store(flags, std::memory_order_release);
    }
    return Status::Ok;
}

namespace {
    /// Every action runs, since on a shared line each one asks its own
    /// device whether it raised the line, and the line counts as handled if
    /// any of them says so.
    auto notify_all(Action::List &actions) -> IrqReturn {
        auto result = IrqReturn::None;
        for (auto &action: actions) {
            if (action.on_event() == IrqReturn::Handled) {
                result = IrqReturn::Handled;
            }
        }
        return result;
    }
} // namespace

/// IrqSave even though a real interrupt arrives with interrupts already off:
/// it costs a flags save and restore, and lets a kernel test drive a dispatch
/// from thread context through the very same path, with Skip.
///
/// The flow is read under the same hold of L1 that it runs under, so a
/// configure() or an attach() that changes it lands wholly before this
/// interrupt or wholly after. A Permanent line's flow runs without L1, like
/// its actions; it reads only which flow the line runs, its list and its
/// chip, and none of them changes once Permanent is set.
auto Desc::handle_irq(Eoi const eoi) -> IrqReturn {
    auto result = IrqReturn::None;
    if (is_permanent()) {
        result = run_flow(permanent_flow(), permanent_actions(), eoi);
    } else {
        Guard<sync::SpinLock, IrqSave> const guard{m_lock};
        result = run_flow(m_flow, m_actions, eoi);
    }

    // An interrupt no action claimed is counted per line. A line whose
    // actions keep answering None is only counted, never shut off; a line
    // with no action at all has been masked by its flow.
    if (result == IrqReturn::None) {
        m_spurious.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

auto Desc::run_flow(Flow const flow, Action::List &actions, Eoi const eoi) -> IrqReturn {
    switch (flow) {
        case Flow::Edge:
            return handle_edge(actions, eoi);
        case Flow::FastEoi:
            return handle_fasteoi(actions, eoi);
    }
    unreachable();
}

/// An edge is one event: once the pin has signalled, nothing raises the line
/// again until the device signals anew, so the line stays unmasked while its
/// actions run: an edge that comes meanwhile waits at the controller and is
/// delivered once this interrupt is ended.
///
/// The end of interrupt goes out after the actions, whether or not they
/// handled the interrupt: the controller delivered it either way, and keeps
/// it in service until it is ended. L1 is held across the actions, so a
/// second edge taken on another CPU waits for L1 rather than running beside
/// them, and ending the interrupt first would gain nothing.
///
/// An interrupt that finds nobody on the line masks it: the device that sent
/// it may keep sending, each edge an interrupt nobody handles, and the line
/// stays quiet until an owner attaches and unmasks it. The end of interrupt
/// goes out all the same; the controller delivered the interrupt either way.
auto Desc::handle_edge(Action::List &actions, Eoi const eoi) -> IrqReturn {
    if (actions.empty()) {
        mask_unclaimed();
        end_irq(eoi);
        return IrqReturn::None;
    }

    auto const result = notify_all(actions);
    end_irq(eoi);
    return result;
}

/// A level pin stays asserted until its device is serviced, and the one end of
/// interrupt after the actions is what tells the controller the line was
/// serviced: on the IO-APIC it clears the pin's Remote IRR, and a pin still
/// asserted then delivers again at once.
///
/// So a level interrupt that finds nobody on the line must mask it before it
/// is ended: left unmasked, the pin would raise the same interrupt again the
/// moment it is ended, for as long as its device holds it asserted, a storm
/// that nobody handles and nothing stops.
///
/// With an action, the steps are handle_edge()'s: an action that has to keep a
/// level line quiet until its device is serviced -- an interrupt object, until
/// its driver acks -- masks the line itself. This flow is kept apart because a
/// mask held across the handling of a level line belongs here, in the line's
/// own flow, and an edge line never needs one.
auto Desc::handle_fasteoi(Action::List &actions, Eoi const eoi) -> IrqReturn {
    if (actions.empty()) {
        mask_unclaimed();
        end_irq(eoi);
        return IrqReturn::None;
    }

    auto const result = notify_all(actions);
    end_irq(eoi);
    return result;
}

/// The chip is read without the lock, as mask() reads it: it is fixed before
/// the line is published. A line of the table always has a chip that masks
/// (DescTable::init()); a line of a test's own, outside the table, needs one
/// too before it is dispatched with nobody on it.
auto Desc::mask_unclaimed() const -> void {
    auto const *const chip = m_data.chip;
    DEBUG_ASSERT(chip && chip->mask, "line {} masks an unclaimed interrupt with no chip mask",
                 m_data.virqnum);
    chip->mask(m_data);
}

/// The chip is read without the lock, as mask() reads it: it is fixed before
/// the line is published. A line of the table always has a chip with an eoi
/// (DescTable::init()); a line of a test's own, outside the table, may have
/// none, and is dispatched with Skip.
auto Desc::end_irq(Eoi const eoi) const -> void {
    if (eoi == Eoi::Skip) {
        return;
    }
    auto const *const chip = m_data.chip;
    DEBUG_ASSERT(chip && chip->issue_eoi, "line {} ends an interrupt with no chip eoi",
                 m_data.virqnum);
    chip->issue_eoi(m_data);
}

/// No L1, on purpose: see the declaration. The chip and its operations are
/// fixed before the line is published, so reading them needs no lock either.
auto Desc::mask() -> Status {
    auto const *const chip = m_data.chip;
    if (!chip || !chip->mask) {
        return Status::NotSupported;
    }
    chip->mask(m_data);
    return Status::Ok;
}

auto Desc::unmask() -> Status {
    auto const *const chip = m_data.chip;
    if (!chip || !chip->unmask) {
        return Status::NotSupported;
    }
    chip->unmask(m_data);
    return Status::Ok;
}

/// Refused on a line that has an action: changing how a live line signals
/// under its handler would hand it interrupts it was not set up for, or
/// none. Holding L1 keeps an attach from slipping in between the check and
/// the write. It does not keep another caller's configure() from landing
/// between this and a later attach(), which is why an owner sets the mode
/// through attach() with a mode instead.
auto Desc::configure(TriggerMode const trigger, Polarity const polarity) -> Status {
    auto const *const chip = m_data.chip;
    if (!chip || !chip->configure) {
        return Status::NotSupported;
    }

    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    // A Permanent line always has its action; the flag is tested for what
    // it says, not as a stand-in for the list.
    auto const flags = m_flags.load(std::memory_order_relaxed);
    if (!!(flags & IrqFlags::Permanent) || !m_actions.empty()) {
        return Status::AlreadyBound;
    }
    auto const status = chip->configure(m_data, trigger, polarity);
    // The flow follows the mode only once the chip has taken it: a refused
    // configure leaves the pin, and so the flow, as they were.
    if (status == Status::Ok) {
        select_flow_locked(trigger);
    }
    return status;
}

/// Asked of the chip rather than answered from the line's flow: the
/// controller's entry is the record of how the pin signals, the one that
/// decides how the pin is read. The line keeps only the flow chosen from it,
/// and keeps the two in step: the table is built after the platform has
/// programmed the pins, and from then on the mode changes only through
/// configure() and attach() with a mode, which choose the flow again under L1.
auto Desc::trigger_mode() const -> ktl::Result<TriggerMode> {
    auto const *const chip = m_data.chip;
    if (!chip || !chip->trigger_mode) {
        return ustl::err(Status::NotSupported);
    }
    return ustl::ok(chip->trigger_mode(m_data));
}

auto Desc::flow() -> Flow {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    return m_flow;
}

} // namespace ours::irq
