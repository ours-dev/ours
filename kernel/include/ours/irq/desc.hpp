/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_DESC_HPP
#define OURS_IRQ_DESC_HPP 1

#include <ours/irq/action.hpp>
#include <ours/irq/chip.hpp>
#include <ours/irq/types.hpp>
#include <ours/status.hpp>
#include <ours/sync/spin_lock.hpp>

#include <atomic>

#include <gktl/canary.hpp>
#include <ktl/result.hpp>
#include <ustl/option.hpp>
#include <ustl/util/enum_bits.hpp>

namespace ours::irq {
/// One interrupt line, its descriptor: its flags, its hardware data, its flow
/// and the list of actions -- the handlers -- that its interrupts are
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
/// the line reaches the action again. The price is that a handler must not
/// attach or detach on its own line: it would spin on the lock its own dispatch
/// holds.
///
/// A Permanent line skips L1 when it dispatches. Its list and its flow are
/// fixed for good once Permanent is set: attach() and configure() refuse
/// everything from then on and detach() refuses its action, so there is
/// nothing for the lock to keep out, and the kernel's own lines -- the timer
/// above all, which fires on every tick -- are spared the lock on every
/// interrupt.
///
/// A Oneshot line -- one whose action said Oneshot -- keeps one more fact
/// under L1: whether it is held, masked at its controller because its action
/// answered WakeThread and has not yet called finalize_oneshot(). The level
/// flow masks such a line before its action runs and leaves it masked while
/// it is held, so a device still asserting its pin raises nothing until the
/// thread has serviced it. One bit is enough: a Oneshot action takes its
/// line alone, so the line owes at most one thread.
class Desc final {
    using Self = Desc;

  public:
    static auto create(HIrqNum hirq, VIrqNum virq, IrqFlags flags, char const *name) -> Self *;

    /// Binds the line to the controller that drives its pin, and gives the
    /// line the flow that suits how the chip says the pin signals now; the
    /// edge flow if the chip cannot say. Called once, by the line table as it
    /// builds the line and before any interrupt or any other CPU can reach
    /// it: mask() and unmask() read the chip without a lock, which is only
    /// safe because it never changes afterwards. BadState if the line has a
    /// chip already.
    [[nodiscard]]
    auto init(Chip const &chip) -> Status;

    /// See irq::attach(). An action that says Permanent may only take an
    /// empty line, and makes the line Permanent; one that says Oneshot makes
    /// it Oneshot.
    [[nodiscard]]
    auto attach(Action &action) -> Status;

    /// See irq::attach() with a mode. Holds L1 from the check that the line
    /// is free to the link, and the chip takes LI inside it to set or read the
    /// mode and to mask: the order L1 -> LI, as configure() takes it. The
    /// line's flow is chosen again, under the same hold, from the mode set
    /// or read.
    [[nodiscard]]
    auto attach(Action &action, ustl::Option<LineMode> mode) -> ktl::Result<TriggerMode>;

    /// See irq::detach(). A barrier: returns only once no handler of the line
    /// is running anywhere. Under the same hold it clears held, and when the
    /// last action goes it masks the line.
    [[nodiscard]]
    auto detach(Action &action) -> Status;

    /// See irq::finalize_oneshot(). Takes L1, then the chip takes LI.
    auto finalize_oneshot(Action const &action) -> void;

    /// Runs the line's flow, handle_edge() or handle_fasteoi(), once for
    /// each interrupt on the line, in interrupt context: every action's
    /// handler, then, if `eoi` says Send, the end of interrupt through the
    /// line's chip. Answers WakeThread if any action woke its thread, else
    /// Handled if any action handled the interrupt; one nobody handled is
    /// counted as spurious, and is ended all the same. On a line nobody is
    /// on, the flow masks the line at its controller in place of running
    /// actions, whatever `eoi` says, and ends the interrupt as `eoi` says.
    /// On a Oneshot level line it masks the line before the action and
    /// unmasks it after only if the line is not held.
    ///
    /// The end of interrupt is the flow's last step, inside the same hold of
    /// L1 as the actions: the code that knows the line and its chip sends
    /// it, once per interrupt, rather than arch, which knows only the
    /// vector. A dispatch that sends it needs a chip with an eoi, and one on
    /// a line nobody is on a chip that masks, which every line of the table
    /// has.
    [[nodiscard]]
    auto handle_irq(Eoi eoi) -> IrqReturn;

    /// See irq::mask() and irq::unmask(). They take no L1: a handler running
    /// under L1 masks its own line (an interrupt object on a level line
    /// does, until its driver acks), and would spin on the lock its dispatch
    /// holds. The chip's own lock, LI, serialises the controller's
    /// registers.
    [[nodiscard]]
    auto mask() -> Status;
    [[nodiscard]]
    auto unmask() -> Status;

    /// See irq::configure(). Takes L1, then the chip takes LI: the order
    /// L1 -> LI of the design's lock order. The line's flow follows the new
    /// mode, chosen under the same hold.
    [[nodiscard]]
    auto configure(TriggerMode trigger, Polarity polarity) -> Status;

    /// See irq::trigger_mode(). No L1, as for mask(): the controller keeps
    /// the mode, and its own lock serialises the read.
    [[nodiscard]]
    auto trigger_mode() const -> ktl::Result<TriggerMode>;

    /// See irq::is_masked_for_test(). No L1, as for trigger_mode(): the
    /// controller keeps the mask bit, and its own lock serialises the read.
    [[nodiscard]]
    auto is_masked() const -> ktl::Result<bool>;

    /// How many interrupts on this line no action handled, since boot.
    [[nodiscard]]
    auto spurious_count() const -> u64 {
        return m_spurious.load(std::memory_order_relaxed);
    }

    /// The flow the line runs now. Takes L1, so it reads the flow as a
    /// dispatch would find it: for tests and diagnostics.
    [[nodiscard]]
    auto flow() -> Flow;

  private:
    /// What the line is, as one word: the first three bits are what its
    /// actions asked for, kept as the line's own facts, and Held is the
    /// line's state. Separate from IrqFlags, which is what one action asks
    /// for, so that a caller building an action never sees Held.
    enum class LineFlags : u32 {
        None,
        Shared = 0_bit,
        Permanent = 1_bit,
        Oneshot = 2_bit,
        /// Its Oneshot action answered WakeThread for an interrupt and has
        /// not called finalize_oneshot() since. Set by the level flow,
        /// cleared by finalize_oneshot() and by detach(). Never set on a
        /// line that is not Oneshot.
        Held = 3_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(LineFlags);

    /// The line's facts that `flags` asks for, for the constructor.
    static auto line_flags(IrqFlags flags) -> LineFlags;

    Desc(HIrqNum hirq, VIrqNum virq, IrqFlags flags, char const *name);

    /// Whether this line can keep the promise of `action`'s Oneshot, for the
    /// two attach() forms before they take L1: InvArgs for an action that
    /// says Oneshot together with Permanent or Shared, NotSupported for a
    /// line whose chip cannot both mask and unmask; Ok otherwise, and for an
    /// action that does not say Oneshot.
    [[nodiscard]]
    auto check_oneshot(Action const &action) const -> Status;

    /// Links |action| into the list and publishes the line's flags, |flags|
    /// with what the action adds to them. For the two attach() forms, once
    /// each has checked that the action may join the line.
    TA_REQUIRES(m_lock)
    auto link_locked(Action &action, LineFlags flags) -> void;

    /// The flow for a line that signals as `trigger` says. For init(),
    /// configure() and attach() with a mode, the three places a line's mode
    /// is learnt or set.
    TA_REQUIRES(m_lock)
    auto select_flow_locked(TriggerMode trigger) -> void;

    /// Runs `flow` over `actions`, then ends the interrupt as `eoi` says,
    /// through a guard held across the flow. The caller holds L1, or the
    /// line is Permanent and neither the flow nor the list changes again.
    auto run_flow(Flow flow, Action::List &actions, Eoi eoi) -> IrqReturn;

    /// The two flows, each one interrupt on the line, with the actions it
    /// runs passed in, so the Permanent line's dispatch, which holds no L1,
    /// runs the same code as every other. Neither ends the interrupt:
    /// run_flow() does, once either has returned.
    auto handle_edge(Action::List &actions) -> IrqReturn;
    auto handle_fasteoi(Action::List &actions) -> IrqReturn;

    /// handle_fasteoi() on a Oneshot line: masks the line, runs the action,
    /// marks the line held if the action woke its thread, and unmasks it
    /// unless it is held. A Oneshot line is never Permanent, so its dispatch
    /// always holds L1.
    TA_REQUIRES(m_lock)
    auto handle_oneshot_locked(Action::List &actions) -> IrqReturn;

    /// Masks the line through its chip, for an interrupt that came in while
    /// nobody was on it. For both flows, under L1.
    auto mask_unclaimed() const -> void;

    /// Read without L1, by handle_irq(), as is Oneshot on a Permanent line's
    /// level flow. The acquire pairs with the release in attach() that sets
    /// it, so a dispatch that sees Permanent also sees the action linked
    /// before it.
    [[nodiscard]]
    auto is_permanent() const -> bool {
        return !!(m_flags.load(std::memory_order_acquire) & LineFlags::Permanent);
    }

    /// Whether the line's action said Oneshot. Read by the level flow, under
    /// L1, where a relaxed load sees the latest write, every write being
    /// under L1 too; on a Permanent line, read without L1, it is never set.
    [[nodiscard]]
    auto is_oneshot() const -> bool {
        return !!(m_flags.load(std::memory_order_relaxed) & LineFlags::Oneshot);
    }

    /// Held, which only L1's holder reads or writes. The bit shares the
    /// atomic word with the facts read without L1, so the analysis cannot
    /// guard it by the member; these two make it check every caller instead.
    [[nodiscard]]
    TA_REQUIRES(m_lock) auto is_held_locked() const -> bool {
        return !!(m_flags.load(std::memory_order_relaxed) & LineFlags::Held);
    }

    /// A release, like every other store to the word: an acquire load that
    /// reads it, in is_permanent(), then also sees what the release that
    /// published the line's action made visible before it.
    TA_REQUIRES(m_lock)
    auto set_held_locked(bool const held) -> void {
        auto flags = m_flags.load(std::memory_order_relaxed);
        if (held) {
            flags |= LineFlags::Held;
        } else {
            flags &= ~LineFlags::Held;
        }
        m_flags.store(flags, std::memory_order_release);
    }

    /// The list of a Permanent line, which handle_irq() walks without L1. Hidden
    /// from the thread-safety analysis, which cannot know that a Permanent
    /// line's list never changes again.
    TA_NO_THREAD_SAFETY_ANALYSIS auto permanent_actions() -> Action::List & {
        return m_actions;
    }

    /// The flow of a Permanent line, which handle_irq() reads without L1, for
    /// the same reason: configure() and attach() with a mode, the only
    /// writers after init(), both refuse a Permanent line.
    TA_NO_THREAD_SAFETY_ANALYSIS auto permanent_flow() const -> Flow {
        return m_flow;
    }

    gktl::Canary m_canary;
    sync::SpinLock m_lock;

    /// Every bit is written under `m_lock`, so one word holds them all: a
    /// store under the lock never overwrites another CPU's bit. The word is
    /// atomic only because handle_irq() reads Permanent without the lock,
    /// and a Permanent line's flow reads Oneshot, never set there, without
    /// it too; Shared and Held are read under the lock.
    std::atomic<LineFlags> m_flags;

    /// Written under `m_lock` by init(), configure() and attach() with a
    /// mode; read under it by handle_irq(), or without it on a Permanent
    /// line, where the release that publishes Permanent orders every write
    /// before it. Beside `m_flags`, in the bytes the word leaves before the
    /// next 8-byte member.
    Flow m_flow TA_GUARDED(m_lock) = Flow::Edge;

    /// Written by the constructor and, for its chip, once by init() before
    /// the line is published; read-only from then on, so read without L1.
    Data m_data;
    Action::List m_actions TA_GUARDED(m_lock);

    /// Bumped by handle_irq(), with or without `m_lock` -- a Permanent line's
    /// dispatch holds none -- so it is atomic. A statistic: relaxed.
    std::atomic<u64> m_spurious{0};

    [[maybe_unused]]
    char const *m_name;
};
} // namespace ours::irq

#endif // OURS_IRQ_DESC_HPP
