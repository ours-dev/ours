/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KERNEL_IRQ_LINE_HPP
#define KERNEL_IRQ_LINE_HPP 1

#include <ours/irq/controller.hpp>
#include <ours/irq/registration.hpp>
#include <ours/irq/types.hpp>
#include <ours/status.hpp>
#include <ours/sync/spin_lock.hpp>

#include <atomic>

#include <gktl/canary.hpp>
#include <ktl/result.hpp>
#include <ustl/option.hpp>
#include <ustl/util/enum_bits.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::irq {
/// One interrupt line, its descriptor: its flags, its hardware data, its flow
/// and the list of registrations -- the handlers -- that its interrupts are
/// dispatched to.
///
/// Its lock, L1 in `.agents/tasks/microkernel-interrupts/DESIGN.md`, is a
/// spin lock always taken with interrupts off, because the same line is
/// walked from two contexts: attach() and detach() in thread context, and
/// handle_irq() in interrupt context, on whichever CPU took the interrupt. A
/// sleeping lock could not be taken in the second, and a spin lock taken
/// with interrupts on could be interrupted by its own line on the CPU that
/// holds it, which would then spin on itself.
///
/// handle_irq() holds L1 while the handlers run. That is what makes detach() a
/// barrier: it takes the same lock, so it cannot return while a handler of this
/// line is still running on another CPU, and once it has returned no handler of
/// the line reaches the registration again. The price is that a handler must not
/// attach or detach on its own line: it would spin on the lock its own dispatch
/// holds.
///
/// Every dispatch takes L1 through a lockdep Guard, including Permanent
/// registrations. Permanent is a lifetime policy, not a second synchronization
/// protocol. Callbacks run with interrupts and preemption disabled.
///
/// A Oneshot line -- one whose registration said Oneshot -- keeps one more fact
/// under L1: whether it is held, masked at its controller because its registration
/// answered WakeThread and has not yet called finalize_oneshot(). The level
/// flow masks such a line before its registration runs and leaves it masked while
/// it is held, so a device still asserting its pin raises nothing until the
/// thread has serviced it, and unmask() leaves it masked too. One bit is
/// enough: a Oneshot registration takes its line alone, so the line owes at most
/// one thread.
class Line final {
  public:
    Line(HardwareIrq source, IrqId id);
    USTL_NO_MOVEABLE_AND_COPYABLE(Line);

    /// Binds the line to the controller that drives its pin, and gives the
    /// line the flow that suits how the controller says the pin signals now; the
    /// edge flow if the controller cannot say. Called once, by the line table as it
    /// builds the line and before any interrupt or any other CPU can reach
    /// it: mask() and unmask() read the controller without a lock, which is only
    /// safe because it never changes afterwards. BadState if the line has a
    /// controller already.
    [[nodiscard]]
    auto init(ControllerOps const &controller, void *context = nullptr) -> Status;

    /// See irq::attach(). An registration that says Permanent may only take an
    /// empty line, and makes the line Permanent; one that says Oneshot makes
    /// it Oneshot, and needs a controller that can say how its pins signal
    /// (NotSupported otherwise): without a mode the line keeps the flow it
    /// has, and only a mode, or a controller that reads one, sets the level flow.
    [[nodiscard]]
    auto attach(Registration &registration) -> Status;

    /// See irq::attach() with a mode. Holds L1 from the check that the line
    /// is free to the link, and the controller takes LI inside it to set or read the
    /// mode and to mask: the order L1 -> LI, as configure() takes it. The
    /// line's flow is chosen again, under the same hold, from the mode set
    /// or read.
    [[nodiscard]]
    auto attach(Registration &registration, ustl::Option<LineMode> mode)
        -> ktl::Result<TriggerMode>;

    /// See irq::detach(). A barrier: returns only once no handler of the line
    /// is running anywhere. Under the same hold it clears held, and when the
    /// last registration goes it masks the line.
    [[nodiscard]]
    auto detach(Registration &registration) -> Status;

    /// See irq::finalize_oneshot(). Takes L1, then the controller takes LI.
    auto finalize_oneshot(Registration const &registration) -> void;

    /// The kernel taking the line away from its registrations, for the removal of
    /// the domain it belongs to. The caller runs two steps over all the
    /// domain's lines, the first over every line before the second over any,
    /// and may run a read-only check before them:
    ///
    ///   0. check_revocable(): mark_dying() without the mark, for a caller
    ///      that wants to refuse before it marks anything.
    ///   1. mark_dying(): refuses a line a removal could not empty (AlreadyBound
    ///      for a Permanent registration, which cannot be revoked, and for
    ///      a line with a registration that has no cancel
    ///      hook), and otherwise marks it. From then on attach(), unmask() and
    ///      configure() answer NoResource, before they touch the controller; mask()
    ///      and finalize_oneshot() work as before. The caller undoes the marks
    ///      it made with unmark_dying() if a later line refuses, so a refused
    ///      removal changes nothing but a short window in which those calls
    ///      may have answered NoResource.
    ///   2. revoke(): takes every registration off the line, tells each through its
    ///      cancel hook, masks the line.
    ///
    /// The loops over all of a domain's lines are Domain's
    /// (begin_removal(), finish_removal()).
    [[nodiscard]]
    auto check_revocable() -> Status;
    [[nodiscard]]
    auto mark_dying() -> Status;
    auto unmark_dying() -> void;
    /// Whether any registration is on the line. Takes L1.
    [[nodiscard]]
    auto has_registrations() -> bool;
    /// Under L1, so it is a barrier as detach() is: it returns with no
    /// handler of the line in flight, and none reaches a registration again.
    /// Each registration is unlinked, then told (IrqRevocationHandler), and its line is cleared
    /// last, with a release store: that store is the removal's last touch of
    /// the registration, and an owner whose irq::detach() reads INVALID (acquire) may
    /// free the registration. An owner that reads a valid line instead queues on L1
    /// behind this and then finds the registration gone (NotFound). The line must be
    /// marked dying, and not Permanent.
    auto revoke() -> void;

    /// Runs the controller handshake and electrical holding policy once for
    /// each interrupt on the line, in interrupt context: every registration's
    /// handler, then, if `eoi` says Send, the end of interrupt through the
    /// line's controller. Answers WakeThread if any registration woke its thread, else
    /// Handled if any registration handled the interrupt; one nobody handled is
    /// counted as spurious, and is ended all the same. On a line nobody is
    /// on, the flow masks the line at its controller in place of running
    /// registrations, whatever `eoi` says, and ends the interrupt as `eoi` says.
    /// On a Oneshot level line it masks the line before the registration and
    /// unmasks it after only if the line is not held.
    ///
    /// The end of interrupt is the flow's last step, inside the same hold of
    /// L1 as the registrations: the code that knows the line and its controller sends
    /// it, once per interrupt, rather than the root handler, which knows only
    /// the hardware number. A dispatch that sends it needs a controller with an eoi, and one on
    /// a line nobody is on a controller that masks, which every line of the table
    /// has.
    [[nodiscard]]
    auto handle_irq(Eoi eoi) -> IrqReturn;

    /// See irq::mask() and irq::unmask(). Each takes L1, then the controller takes
    /// LI, as every other write of the line's mask does, so the writes land
    /// in the order L1 gives them; unmask() reads held in the same hold and
    /// leaves a held line masked. The thread-safety analysis refuses a call
    /// made with L1 held, as from inside a flow, which would spin on itself;
    /// a handler, called through a pointer, is beyond its reach, so the rule
    /// that no handler calls them stays a rule: on its own line it would spin
    /// on its own dispatch, and on another it would hold two lines' L1, which
    /// have no order between them.
    [[nodiscard]]
    TA_EXCLUDES(m_lock) auto mask() -> Status;
    [[nodiscard]]
    TA_EXCLUDES(m_lock) auto unmask() -> Status;

    /// See irq::configure(). Takes L1, then the controller takes LI: the order
    /// L1 -> LI of the design's lock order. The line's flow follows the new
    /// mode, chosen under the same hold.
    [[nodiscard]]
    auto configure(TriggerMode trigger, Polarity polarity) -> Status;

    /// See irq::trigger_mode(). No L1: it reads nothing L1 guards, the
    /// controller keeps the mode, and its own lock serialises the read.
    [[nodiscard]]
    auto trigger_mode() const -> ktl::Result<TriggerMode>;

    /// See irq::is_masked_for_test(). No L1, as for trigger_mode(): the
    /// controller keeps the mask bit, and its own lock serialises the read.
    [[nodiscard]]
    auto is_masked() const -> ktl::Result<bool>;

    /// How many interrupts on this line no registration handled, since boot.
    [[nodiscard]]
    auto spurious_count() const -> u64 {
        return m_spurious.load(std::memory_order_relaxed);
    }

    /// The flow the line runs now. Takes L1, so it reads the flow as a
    /// dispatch would find it: for tests and diagnostics.
    [[nodiscard]]
    auto dispatch_trigger() -> TriggerMode;

    /// Whether the line is held for the kernel's lifetime. Takes L1.
    [[nodiscard]]
    auto is_kernel_owned() -> bool;

  private:
    /// What the line is, as one word: the first three bits are what its
    /// registrations asked for, kept as the line's own facts, and Held and Dying
    /// are the line's state. Separate from RegistrationOptions, which is what one
    /// registration asks for, so that a caller building a registration never sees them.
    enum class LineFlags : u32 {
        None,
        Shared = 0_bit,
        Permanent = 1_bit,
        Oneshot = 2_bit,
        /// Its Oneshot registration answered WakeThread for an interrupt and has
        /// not called finalize_oneshot() since. Set by the level flow,
        /// cleared by finalize_oneshot() and by detach(), and read by the
        /// level flow and unmask(), which leave the line masked while it is
        /// set. Never set on a line that is not Oneshot.
        Held = 3_bit,
        /// A removal of the line's domain has begun: set by mark_dying(),
        /// cleared by unmark_dying(). The line takes no new registration and is
        /// not unmasked or reconfigured.
        Dying = 4_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(LineFlags);

    /// Checks immutable registration policy before either attach touches the
    /// line or its controller. Deferred work also requires mask and unmask.
    [[nodiscard]]
    auto check_registration(Registration const &registration) const -> Status;

    /// Links |registration| into the list and publishes the line's flags, |flags|
    /// with what the registration adds to them. For the two attach() forms, once
    /// each has checked that the registration may join the line.
    TA_REQUIRES(m_lock)
    auto link_locked(Registration &registration, LineFlags flags) -> void;

    [[nodiscard]]
    TA_REQUIRES(m_lock) auto notify_locked() -> IrqReturn;
    TA_REQUIRES(m_lock)
    auto dispatch_locked(Eoi eoi) -> IrqReturn;
    TA_REQUIRES(m_lock)
    auto handle_oneshot_locked() -> IrqReturn;
    TA_REQUIRES(m_lock)
    auto mask_unclaimed() const -> void;

    /// Whether a removal has marked the line. Under L1, like Held.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto is_dying_locked() const -> bool {
        return !!(m_flags & LineFlags::Dying);
    }

    /// The reason mark_dying() would refuse the line, Ok if it would not.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto revocable_locked() const -> Status;

    [[nodiscard]]
    TA_REQUIRES(m_lock) auto is_oneshot_locked() const -> bool {
        return !!(m_flags & LineFlags::Oneshot);
    }

    [[nodiscard]]
    TA_REQUIRES(m_lock) auto is_held_locked() const -> bool {
        return !!(m_flags & LineFlags::Held);
    }

    TA_REQUIRES(m_lock)
    auto set_held_locked(bool const held) -> void {
        if (held) {
            m_flags |= LineFlags::Held;
        } else {
            m_flags &= ~LineFlags::Held;
        }
    }

    gktl::Canary m_canary;
    sync::SpinLock m_lock;

    LineFlags m_flags TA_GUARDED(m_lock);
    TriggerMode m_trigger TA_GUARDED(m_lock) = TriggerMode::Edge;

    /// Written by the constructor and, for its controller, once by init() before
    /// the line is published; read-only from then on, so read without L1.
    ControllerSource m_source;
    Registration::List m_registrations TA_GUARDED(m_lock);

    /// A statistic read without taking L1; dispatch updates it under L1.
    std::atomic<u64> m_spurious{0};
};
} // namespace ours::irq

#endif // KERNEL_IRQ_LINE_HPP
