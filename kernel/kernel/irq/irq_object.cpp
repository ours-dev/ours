#include <ours/irq/irq_object.hpp>
#include <ours/lockdep.hpp>

#include <heap/new.hpp>

namespace ours::irq {
IrqObject::IrqObject(HIrqNum hirq, VIrqNum virq, IrqFlags flags, char const *name)
    : m_canary(this)
    , m_lock()
    , m_flags(flags)
    , m_data(hirq, virq, nullptr /* No chip available */)
    , m_observers()
    , m_name(name) {}

auto IrqObject::create(HIrqNum hirq, VIrqNum virq, IrqFlags flags, char const *name) -> Self * {
    return new (mm::GAF_KERNEL) Self(hirq, virq, flags, name);
}

/// Refusing a second call keeps the chip what mask() and unmask(), which read
/// it without a lock, rely on: fixed once the line is published.
auto IrqObject::init(IrqChip const &chip) -> Status {
    if (m_data.chip) {
        return Status::BadState;
    }
    m_data.chip = &chip;
    return Status::Ok;
}

/// The flags are the line's, and an attach only adds to them. This used to
/// assign the word outright on the first attach, `irqflags_ = Shared or
/// None`, which wiped whatever else the line had stored -- Permanent above
/// all, so the timer's request for a permanent line was dropped on the floor.
/// Now Shared alone follows the line's occupants (set by the first, cleared
/// by detach() when the last leaves), and Permanent, once set, stays.
auto IrqObject::attach(IrqObserver &observer) -> Status {
    // The handler is fixed at construction, so it needs no lock to read.
    if (!observer.m_handler) {
        return Status::InvArgs;
    }

    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    // An observer hangs on one line at a time: its hook has room for one
    // list, and pushing a linked hook again would corrupt both lists.
    if (observer.is_attached()) {
        return Status::BadState;
    }

    // Only writers hold the lock, so under it a relaxed load sees the latest.
    auto flags = m_flags.load(std::memory_order_relaxed);
    // A Permanent line takes nobody else, shared or not: its list is walked
    // without the lock, which is only safe while nothing changes it.
    if (!!(flags & IrqFlags::Permanent)) {
        return Status::AlreadyBound;
    }
    if (!m_observers.empty()) {
        // Joining an occupied line needs both sides to agree to share, and a
        // Permanent observer never shares: it would freeze the others on the
        // line along with itself.
        auto const both_share = !!(flags & IrqFlags::Shared) && observer.is_shared();
        if (!both_share || observer.is_permanent()) {
            return Status::AlreadyBound;
        }
    } else if (observer.is_shared()) {
        flags |= IrqFlags::Shared;
    }

    m_observers.push_back(observer);
    observer.m_virqnum = m_data.virqnum;

    if (observer.is_permanent()) {
        flags |= IrqFlags::Permanent;
    }
    // Release, last: a dispatch that reads Permanent without the lock must
    // find the observer already linked.
    m_flags.store(flags, std::memory_order_release);
    return Status::Ok;
}

/// Taking the lock is the barrier. notify() holds it while the handlers run,
/// so this cannot unlink the observer while its handler runs on another CPU,
/// and after the unlink no dispatch can reach the observer again. When this
/// returns, the owner may free the observer and whatever its context points
/// to.
auto IrqObject::detach(IrqObserver &observer) -> Status {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    // erase() trusts that the observer is in this very list; erasing one that
    // is not would unlink it from whichever list it is on, or from none.
    if (observer.m_virqnum != m_data.virqnum) {
        return Status::NotFound;
    }

    auto flags = m_flags.load(std::memory_order_relaxed);
    // A Permanent line dispatches without the lock, so taking it is no
    // barrier there, and unlinking would race a dispatch walking the list.
    if (!!(flags & IrqFlags::Permanent)) {
        return Status::AlreadyBound;
    }

    m_observers.erase(observer);
    observer.m_virqnum = INVALID_V_IRQ_NUM;
    if (m_observers.empty()) {
        flags &= ~IrqFlags::Shared;
        m_flags.store(flags, std::memory_order_release);
    }
    return Status::Ok;
}

namespace {
    /// Every observer runs, since on a shared line each one asks its own
    /// device whether it raised the line, and the line counts as handled if
    /// any of them says so.
    auto notify_all(IrqObserver::List &observers) -> IrqReturn {
        auto result = IrqReturn::None;
        for (auto &observer: observers) {
            if (observer.on_event() == IrqReturn::Handled) {
                result = IrqReturn::Handled;
            }
        }
        return result;
    }
} // namespace

/// IrqSave even though a real interrupt arrives with interrupts already off:
/// it costs a flags save and restore, and lets a kernel test drive a dispatch
/// from thread context through the very same path.
auto IrqObject::notify() -> IrqReturn {
    auto result = IrqReturn::None;
    if (is_permanent()) {
        result = notify_all(permanent_observers());
    } else {
        Guard<sync::SpinLock, IrqSave> const guard{m_lock};
        result = notify_all(m_observers);
    }

    // An interrupt no observer claimed is counted per line. A line that
    // keeps firing for nobody is only counted, never shut off.
    if (result == IrqReturn::None) {
        m_spurious.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

/// No L1, on purpose: see the declaration. The chip and its operations are
/// fixed before the line is published, so reading them needs no lock either.
auto IrqObject::mask() -> Status {
    auto const *const chip = m_data.chip;
    if (!chip || !chip->mask) {
        return Status::NotSupported;
    }
    chip->mask(m_data);
    return Status::Ok;
}

auto IrqObject::unmask() -> Status {
    auto const *const chip = m_data.chip;
    if (!chip || !chip->unmask) {
        return Status::NotSupported;
    }
    chip->unmask(m_data);
    return Status::Ok;
}

/// Refused on a line that has an observer: changing how a live line signals
/// under its handler would hand it interrupts it was not set up for, or
/// none. Holding L1 keeps an attach from slipping in between the check and
/// the write; it does not keep two callers' configure-then-attach sequences
/// apart, which is theirs to serialise.
auto IrqObject::configure(TriggerMode const trigger, Polarity const polarity) -> Status {
    auto const *const chip = m_data.chip;
    if (!chip || !chip->configure) {
        return Status::NotSupported;
    }

    Guard<sync::SpinLock, IrqSave> const guard{m_lock};
    // A Permanent line always has its observer; the flag is tested for what
    // it says, not as a stand-in for the list.
    auto const flags = m_flags.load(std::memory_order_relaxed);
    if (!!(flags & IrqFlags::Permanent) || !m_observers.empty()) {
        return Status::AlreadyBound;
    }
    return chip->configure(m_data, trigger, polarity);
}

/// Asked of the chip each time rather than remembered here: the platform
/// programs a line's mode at boot without going through the line, so a copy
/// kept here could disagree with the controller, which is what decides how
/// the pin is read.
auto IrqObject::trigger_mode() const -> ktl::Result<TriggerMode> {
    auto const *const chip = m_data.chip;
    if (!chip || !chip->trigger_mode) {
        return ustl::err(Status::NotSupported);
    }
    return ustl::ok(chip->trigger_mode(m_data));
}

} // namespace ours::irq
