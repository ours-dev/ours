#include <ours/assert.hpp>
#include <ours/irq/line.hpp>
#include <ours/lockdep.hpp>
#include <ours/task/auto_preempt_disabler.hpp>

#include <heap/new.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::irq {
Line::Line(HIrqNum hirq, VIrqNum virq, IrqFlags flags, char const *name)
    : m_canary(this)
    , m_lock()
    , m_flags(line_flags(flags))
    , m_source(hirq, virq, nullptr /* No chip available */)
    , m_registrations()
    , m_name(name) {}

/// The bits share their positions with IrqFlags today, but they are
/// translated one by one, so that the two types may part without anyone
/// noticing a silent reinterpretation.
auto Line::line_flags(IrqFlags const flags) -> LineFlags {
    auto line = LineFlags::None;
    if (!!(flags & IrqFlags::Shared)) {
        line |= LineFlags::Shared;
    }
    if (!!(flags & IrqFlags::Permanent)) {
        line |= LineFlags::Permanent;
    }
    if (!!(flags & IrqFlags::Oneshot)) {
        line |= LineFlags::Oneshot;
    }
    return line;
}

auto Line::create(HIrqNum hirq, VIrqNum virq, IrqFlags flags, char const *name) -> Self * {
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
auto Line::init(ControllerOps const &chip) -> Status {
    if (m_source.chip) {
        return Status::BadState;
    }
    m_source.chip = &chip;

    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    // A chip that cannot say how its pins signal gets the edge flow, which
    // asks nothing of the chip around the actions.
    auto trigger = TriggerMode::Edge;
    if (chip.trigger_mode) {
        trigger = chip.trigger_mode(m_source);
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
auto Line::attach(Registration &action) -> Status {
    if (auto const status = check_registration(action); status != Status::Ok) {
        return status;
    }
    // Only the level flow holds a Oneshot line masked. With no mode the line
    // keeps the flow it has, chosen when it was bound or by the last configure
    // or attach with a mode, and a chip that cannot say how its pins signal
    // gave it the edge flow. So such a chip's Oneshot action is refused here:
    // this looks at what the chip can say, not at the flow the line has now,
    // and the action needs a mode (the other attach). The chip is fixed before
    // the line is published, so it is read without the lock.
    if (action.is_oneshot() && (m_source.chip == nullptr || !m_source.chip->trigger_mode)) {
        return Status::NotSupported;
    }

    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    // A line the kernel is taking away takes nobody new.
    if (is_dying_locked()) {
        return Status::NoResource;
    }
    // An action hangs on one line at a time: its hook has room for one
    // list, and pushing a linked hook again would corrupt both lists.
    if (action.is_attached()) {
        return Status::BadState;
    }

    // Only writers hold the lock, so under it a relaxed load sees the latest.
    auto const flags = m_flags.load(std::memory_order_relaxed);
    // A Permanent line takes nobody else, shared or not: its list is walked
    // without the lock, which is only safe while nothing changes it.
    if (!!(flags & LineFlags::Permanent)) {
        return Status::AlreadyBound;
    }
    if (!m_registrations.empty()) {
        // Joining an occupied line needs both sides to agree to share, and a
        // Permanent action never shares: it would freeze the others on the
        // line along with itself.
        auto const both_share = !!(flags & LineFlags::Shared) && action.is_shared();
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
auto Line::attach(Registration &action, ustl::Option<LineMode> const mode)
    -> ktl::Result<TriggerMode> {
    if (auto const status = check_registration(action); status != Status::Ok) {
        return ustl::err(status);
    }
    // The chip is fixed before the line is published, so it is read without
    // the lock, as mask() reads it.
    auto const *const chip = m_source.chip;
    if (mode.is_some()) {
        if (!chip || !chip->configure) {
            return ustl::err(Status::NotSupported);
        }
    } else if (!chip || !chip->trigger_mode || !chip->mask) {
        return ustl::err(Status::NotSupported);
    }

    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    // Before the chip is touched: the mode of a line being taken away is
    // not ours to set.
    if (is_dying_locked()) {
        return ustl::err(Status::NoResource);
    }
    if (action.is_attached()) {
        return ustl::err(Status::BadState);
    }
    // Only an empty line: its mode is about to be set or read for this
    // action, and the mode of a line others are on is theirs. A Permanent
    // line always has its action; the flag is tested for what it says.
    auto const flags = m_flags.load(std::memory_order_relaxed);
    if (!!(flags & LineFlags::Permanent) || !m_registrations.empty()) {
        return ustl::err(Status::AlreadyBound);
    }

    auto trigger = TriggerMode::Edge;
    if (mode.is_some()) {
        if (auto const status = chip->configure(m_source, mode->trigger, mode->polarity);
            status != Status::Ok) {
            return ustl::err(status);
        }
        trigger = mode->trigger;
    } else {
        chip->mask(m_source);
        trigger = chip->trigger_mode(m_source);
    }
    select_flow_locked(trigger);

    link_locked(action, flags);
    return ustl::ok(trigger);
}

/// Each refusal is a promise Oneshot could not keep:
/// - Permanent: a Permanent line dispatches without L1, and held is kept
///   under L1.
/// - Shared: the line keeps one held bit, which records one thread's debt.
///   With two Oneshot actions on one line, the second one's finalize would
///   unmask the line while the first one's thread still owed its device a
///   service.
/// - A chip that cannot mask and unmask: the level flow masks a Oneshot line
///   before its action and unmasks it after, and finalize unmasks it.
///
/// The flags and the handler are fixed at construction, and the chip before
/// the line is published, so none of this needs L1.
auto Line::check_registration(Registration const &action) const -> Status {
    if (!action.is_valid()) {
        return Status::InvArgs;
    }
    if (!action.is_oneshot()) {
        return Status::Ok;
    }
    auto const *const chip = m_source.chip;
    if (!chip || !chip->mask || !chip->unmask) {
        return Status::NotSupported;
    }
    return Status::Ok;
}

auto Line::select_flow_locked(TriggerMode const trigger) -> void {
    if (trigger == TriggerMode::Level) {
        m_flow = Flow::FastEoi;
    } else {
        m_flow = Flow::Edge;
    }
}

/// The first action decides whether the line shares; one that joins later
/// got here only because both it and the line said Shared. Permanent, once
/// set, stays.
///
/// Oneshot is the line's for as long as its action is on it. A Oneshot
/// action never shares, so it only ever takes an empty line, and detach()
/// clears the flag as the line empties.
auto Line::link_locked(Registration &action, LineFlags flags) -> void {
    if (m_registrations.empty() && action.is_shared()) {
        flags |= LineFlags::Shared;
    }

    m_registrations.push_back(action);
    action.m_virqnum.store(m_source.virqnum, std::memory_order_release);

    if (action.is_permanent()) {
        flags |= LineFlags::Permanent;
    }
    if (action.is_oneshot()) {
        flags |= LineFlags::Oneshot;
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
///
/// The same hold clears held. A Oneshot line has one action, this one, so
/// whatever the line still owes is this action's thread's, and it goes with
/// the action rather than waiting for a finalize: the thread may be a user
/// thread that never acks, and the line must not stay held against the next
/// owner, which would then find every interrupt it handles itself leaving
/// the line masked.
///
/// When the last action goes, the line is masked under the same hold: with
/// nobody on it, nothing should raise it until the next owner, having
/// attached, unmasks it. Left unmasked, a level pin whose device still
/// asserts it would raise an interrupt nobody handles before the flow masked
/// it; a held line is masked already, and stays so though held is gone.
auto Line::detach(Registration &action) -> Status {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    // erase() trusts that the action is in this very list; erasing one that
    // is not would unlink it from whichever list it is on, or from none.
    if (action.virqnum() != m_source.virqnum) {
        return Status::NotFound;
    }

    auto flags = m_flags.load(std::memory_order_relaxed);
    // A Permanent line dispatches without the lock, so taking it is no
    // barrier there, and unlinking would race a dispatch walking the list.
    if (!!(flags & LineFlags::Permanent)) {
        return Status::AlreadyBound;
    }

    m_registrations.erase(action);
    action.m_virqnum.store(INVALID_V_IRQ_NUM, std::memory_order_release);
    // Held and, on a line left empty, the facts its last action brought go
    // out in one store.
    flags &= ~LineFlags::Held;
    auto const emptied = m_registrations.empty();
    if (emptied) {
        flags &= ~(LineFlags::Shared | LineFlags::Oneshot);
    }
    m_flags.store(flags, std::memory_order_release);
    if (emptied) {
        // Every line of the table has a chip that masks; only a line of a
        // test's own may have no chip at all.
        auto const *const chip = m_source.chip;
        if (chip && chip->mask) {
            chip->mask(m_source);
        }
    }
    return Status::Ok;
}

/// Under L1, so it lands wholly before or after a dispatch of the line: never
/// between the flow's check of held and its unmask, and never between a
/// detach's unlink and its mask.
///
/// The line is named by the caller rather than read from the action: the
/// action's line is written under that line's L1, which is only held here,
/// and a finalize races a detach of the same action when one thread acks
/// while another destroys. Under this L1 the action is either still on this
/// line, and owes what the line holds, or gone, and the detach has cleared
/// held and masked the line for it: then there is nothing to finalize, and
/// unmasking would hand the next owner a line it has not unmasked itself.
///
/// The unmask does not ask whether the line was held: held says the line owes
/// a thread, not whether it is masked, which only the controller records. A
/// finalize that comes when nothing was owed unmasks a line its flow left
/// unmasked already, and the controller's unmask is idempotent.
///
/// A line marked for removal is finalized all the same. Skipping it would
/// leave the line held and masked if the removal is then refused and the mark
/// taken off again, with its object idle and nothing to raise the next
/// interrupt; and if the removal goes through, revoke() clears held and masks
/// the line last, under this same lock.
auto Line::finalize_oneshot(Registration const &action) -> void {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    if (action.virqnum() != m_source.virqnum || !is_oneshot()) {
        return;
    }
    set_held_locked(false);
    // check_registration() let the action on only with a chip that unmasks.
    m_source.chip->unmask(m_source);
}

namespace {
    /// Every action runs, since on a shared line each one asks its own
    /// device whether it raised the line, and the line counts as handled if
    /// any of them says so. WakeThread says so too, and more: a thread still
    /// owes the device its service, which is what the level flow of a
    /// Oneshot line needs to know, so it outranks Handled in the answer.
    auto notify_all(Registration::List &actions) -> IrqReturn {
        auto result = IrqReturn::None;
        for (auto &action: actions) {
            auto const answer = action.on_event();
            if (answer == IrqReturn::WakeThread) {
                result = IrqReturn::WakeThread;
            } else if (answer == IrqReturn::Handled && result == IrqReturn::None) {
                result = IrqReturn::Handled;
            }
        }
        return result;
    }

    /// One interrupt in service at the line's controller, ended through the
    /// line's chip when the guard goes out of scope, unless `eoi` says Skip.
    ///
    /// The controller holds back every interrupt of this one's priority and
    /// below on the CPU until it is ended, so each interrupt must be ended
    /// exactly once, on whichever path its flow returns by: a path that
    /// forgot would silence the CPU's devices, and a second end would end an
    /// interrupt still in service beneath this one. A guard makes that a
    /// matter of scope rather than of each return path.
    ///
    /// The chip is read without the lock, as Line::mask() reads it: it is
    /// fixed before the line is published. A line of the table always has a
    /// chip with an eoi (DescTable::init()); a line of a test's own, outside
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
            auto const *const chip = m_source.chip;
            DEBUG_ASSERT(chip && chip->issue_eoi, "line {} ends an interrupt with no chip eoi",
                         m_source.virqnum);
            chip->issue_eoi(m_source);
        }

      private:
        ControllerSource const &m_source;
        Eoi const m_eoi;
    };
} // namespace

/// IrqSave even though a real interrupt arrives with interrupts already off:
/// it costs a flags save and restore, and lets a kernel test drive a dispatch
/// from thread context through the very same path, with Skip.
///
/// The flow is read under the same hold of L1 that it runs under, so a
/// configure() or an attach() that changes it lands wholly before this
/// interrupt or wholly after. A Permanent line's flow runs without L1, like
/// its actions; it reads only which flow the line runs, its list, its chip
/// and, on the level flow, the Oneshot flag, which attach() never sets with
/// Permanent, and none of them changes once Permanent is set.
auto Line::handle_irq(Eoi const eoi) -> IrqReturn {
    auto result = IrqReturn::None;
    if (is_permanent()) {
        result = run_flow(permanent_flow(), permanent_registrations(), eoi);
    } else {
        Guard<sync::SpinLock, IrqSave> const guard{m_lock};
        result = run_flow(m_flow, m_registrations, eoi);
    }

    // An interrupt no action claimed is counted per line; WakeThread claims
    // it as Handled does. A line whose actions keep answering None is only
    // counted, never shut off; a line with no action at all has been masked
    // by its flow.
    if (result == IrqReturn::None) {
        m_spurious.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

/// The guard is constructed before the flow runs, so it is destroyed after
/// the flow has returned: the end of interrupt comes after everything the
/// flow does to the line -- the mask of a line nobody is on, the unmask of a
/// Oneshot line that is not held -- and, on a line that is not Permanent,
/// still inside the caller's hold of L1.
auto Line::run_flow(Flow const flow, Registration::List &actions, Eoi const eoi) -> IrqReturn {
    EoiGuard const end_of_interrupt{m_source, eoi};
    switch (flow) {
        case Flow::Edge:
            return handle_edge(actions);
        case Flow::FastEoi:
            return handle_fasteoi(actions);
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
///
/// Oneshot is ignored here, and the line is never held: an edge raises
/// nothing again until the device signals anew, so there is nothing to hold
/// off while a thread services the device, and a masked edge pin would drop
/// the edges that came meanwhile, which nothing replays.
auto Line::handle_edge(Registration::List &actions) -> IrqReturn {
    if (actions.empty()) {
        mask_unclaimed();
        return IrqReturn::None;
    }
    return notify_all(actions);
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
/// A Oneshot line is held masked across its thread's work: see
/// handle_oneshot_locked().
///
/// With an action that is not Oneshot, the steps are handle_edge()'s: the
/// action services its device in its handler, before the end of interrupt,
/// or keeps the line quiet itself. An interrupt object, whose driver
/// services the device long after the handler, says Oneshot instead.
auto Line::handle_fasteoi(Registration::List &actions) -> IrqReturn {
    if (actions.empty()) {
        mask_unclaimed();
        return IrqReturn::None;
    }
    if (is_oneshot()) {
        // check_registration() refuses Oneshot on a Permanent action, so a Oneshot
        // line is dispatched under L1, and held, which L1 guards, may be read.
        m_lock.assert_held();
        return handle_oneshot_locked(actions);
    }
    return notify_all(actions);
}

/// The line is masked before the action runs. The action's handler answers
/// WakeThread when it leaves the device to a thread: the device still
/// asserts the pin, and nothing will deassert it until the thread has
/// serviced it, so the line must not be unmasked when this interrupt is
/// ended. The handler only wakes the thread; it is the line that remembers
/// the debt, in held, and keeps the line masked until finalize_oneshot().
///
/// An interrupt the action handles itself, Handled or None, owes nothing, and
/// the line is unmasked again before the end of interrupt -- unless an
/// earlier interrupt left it held, whose thread still owes its service. The
/// end of interrupt stays the flow's last step, as on every path: on the
/// IO-APIC the pin's Remote IRR holds back a level pin still asserted until
/// that end, so unmasking first raises nothing early.
///
/// Mask, actions, unmask and end of interrupt all run under the one hold of
/// L1, which is what keeps finalize_oneshot() and detach(), which take it
/// too, from landing between the check of held and the unmask.
auto Line::handle_oneshot_locked(Registration::List &actions) -> IrqReturn {
    // check_registration() let the action on only with a chip that masks and
    // unmasks.
    auto const &chip = *m_source.chip;
    chip.mask(m_source);
    auto const result = notify_all(actions);
    if (result == IrqReturn::WakeThread) {
        set_held_locked(true);
    }
    if (!is_held_locked()) {
        chip.unmask(m_source);
    }
    return result;
}

/// The chip is read without the lock, as mask() reads it: it is fixed before
/// the line is published. A line of the table always has a chip that masks
/// (DescTable::init()); a line of a test's own, outside the table, needs one
/// too before it is dispatched with nobody on it.
auto Line::mask_unclaimed() const -> void {
    auto const *const chip = m_source.chip;
    DEBUG_ASSERT(chip && chip->mask, "line {} masks an unclaimed interrupt with no chip mask",
                 m_source.virqnum);
    chip->mask(m_source);
}

/// Under L1, like every other write of the line's mask -- the flow's,
/// finalize_oneshot()'s, detach()'s, configure()'s and attach()'s, with a
/// mode or without -- so the writes land in the order L1 gives them. Without
/// it, a mask could land between a Oneshot line's flow masking the line and
/// unmasking it after an action that handled the interrupt itself, and the
/// flow's unmask would undo it at once.
///
/// The chip and its operations are fixed before the line is published, so
/// they are read before the lock is taken, as configure() reads them.
auto Line::mask() -> Status {
    auto const *const chip = m_source.chip;
    if (!chip || !chip->mask) {
        return Status::NotSupported;
    }
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    chip->mask(m_source);
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
    auto const *const chip = m_source.chip;
    if (!chip || !chip->unmask) {
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
    chip->unmask(m_source);
    return Status::Ok;
}

/// Refused on a line that has an action: changing how a live line signals
/// under its handler would hand it interrupts it was not set up for, or
/// none. Holding L1 keeps an attach from slipping in between the check and
/// the write. It does not keep another caller's configure() from landing
/// between this and a later attach(), which is why an owner sets the mode
/// through attach() with a mode instead.
auto Line::configure(TriggerMode const trigger, Polarity const polarity) -> Status {
    auto const *const chip = m_source.chip;
    if (!chip || !chip->configure) {
        return Status::NotSupported;
    }

    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    if (is_dying_locked()) {
        return Status::NoResource;
    }
    // A Permanent line always has its action; the flag is tested for what
    // it says, not as a stand-in for the list.
    auto const flags = m_flags.load(std::memory_order_relaxed);
    if (!!(flags & LineFlags::Permanent) || !m_registrations.empty()) {
        return Status::AlreadyBound;
    }
    auto const status = chip->configure(m_source, trigger, polarity);
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
auto Line::trigger_mode() const -> ktl::Result<TriggerMode> {
    auto const *const chip = m_source.chip;
    if (!chip || !chip->trigger_mode) {
        return ustl::err(Status::NotSupported);
    }
    return ustl::ok(chip->trigger_mode(m_source));
}

/// Asked of the chip, as trigger_mode() is: the controller's entry is the one
/// record of the mask, and the line keeps no copy of it to answer from.
auto Line::is_masked() const -> ktl::Result<bool> {
    auto const *const chip = m_source.chip;
    if (!chip || !chip->is_masked) {
        return ustl::err(Status::NotSupported);
    }
    return ustl::ok(chip->is_masked(m_source));
}

/// Why a removal could not empty this line: a Permanent line dispatches
/// without L1, so nothing can wait for a handler of it, and its owner is the
/// kernel's own code, which holds it for good; and an action that gave no
/// cancel hook has not agreed to be taken off, so its owner would never be
/// told. Both are AlreadyBound, as an attach on such a line answers.
auto Line::revocable_locked() const -> Status {
    if (!!(m_flags.load(std::memory_order_relaxed) & LineFlags::Permanent)) {
        return Status::AlreadyBound;
    }
    for (auto const &action: m_registrations) {
        if (!action.is_revocable()) {
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
/// action that was not there to be asked.
auto Line::mark_dying() -> Status {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    if (auto const status = revocable_locked(); status != Status::Ok) {
        return status;
    }
    m_flags.store(m_flags.load(std::memory_order_relaxed) | LineFlags::Dying,
                  std::memory_order_release);
    return Status::Ok;
}

auto Line::has_registrations() -> bool {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    return !m_registrations.empty();
}

auto Line::unmark_dying() -> void {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    m_flags.store(m_flags.load(std::memory_order_relaxed) & ~LineFlags::Dying,
                  std::memory_order_release);
}

/// IrqSaveAndPreemptDisable, not IrqSave as every dispatch of the line has
/// it: a cancel hook may wake a thread, and the reschedule that asks for
/// has to wait until L1 is dropped, as in a handler's context. A real
/// interrupt has both off already; this is thread context.
///
/// The order within one action is the point. It is unlinked first, so no
/// dispatch can reach it again, and the lock is held, so none is in flight;
/// then told; and only then is its line cleared. An owner that destroys its
/// object meanwhile reaches irq::detach(), which either reads the line still
/// valid and queues on L1 behind this, or reads INVALID, which was stored
/// after the hook returned and after the last touch of the action, so
/// seeing it means this is done with the action and its owner. Clearing the
/// line before telling would let the owner see INVALID, finish, and free
/// the action under the hook.
///
/// Held goes with the actions, as in detach(): a Oneshot line's thread
/// owes nothing to an owner that has been told it is gone. The line is
/// masked last, through the chip, which the table's lines all have.
auto Line::revoke() -> void {
    Guard<sync::SpinLock, IrqSaveAndPreemptDisable> const guard{m_lock};
    auto flags = m_flags.load(std::memory_order_relaxed);
    // Checked in the release image too: a line not marked may have an action
    // that never agreed to be taken off, and a Permanent line's list is walked
    // without this lock.
    ASSERT(!!(flags & LineFlags::Dying), "line {} revoked before it was marked", m_source.virqnum);
    ASSERT(!(flags & LineFlags::Permanent), "line {} is Permanent and revoked", m_source.virqnum);

    for (;;) {
        auto const first = m_registrations.front();
        if (first.is_none()) {
            break;
        }
        auto &action = *first;
        ASSERT(action.is_revocable(), "line {} revokes an action with no cancel hook",
               m_source.virqnum);
        m_registrations.erase(action);
        action.cancel();
        action.m_virqnum.store(INVALID_V_IRQ_NUM, std::memory_order_release);
    }

    flags &= ~(LineFlags::Held | LineFlags::Shared | LineFlags::Oneshot);
    m_flags.store(flags, std::memory_order_release);
    auto const *const chip = m_source.chip;
    if (chip && chip->mask) {
        chip->mask(m_source);
    }
}

auto Line::flow() -> Flow {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    return m_flow;
}

} // namespace ours::irq
