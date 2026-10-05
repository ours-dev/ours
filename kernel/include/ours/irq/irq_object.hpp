/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_IRQ_OBJECT_HPP
#define OURS_IRQ_IRQ_OBJECT_HPP 1

#include <ours/irq/irq_chip.hpp>
#include <ours/irq/irq_observer.hpp>
#include <ours/irq/types.hpp>
#include <ours/status.hpp>
#include <ours/sync/spin_lock.hpp>

#include <atomic>

#include <gktl/canary.hpp>

namespace ours::irq {
/// One interrupt line: its flags, its hardware data and the list of
/// observers -- the handlers -- that its interrupts are dispatched to.
///
/// Its lock, L1 in `.agents/tasks/microkernel-interrupts/DESIGN.md`, is a
/// spin lock always taken with interrupts off, because the same line is
/// walked from two contexts: attach() and detach() in thread context, and
/// notify() in interrupt context, on whichever CPU took the interrupt. A
/// sleeping lock could not be taken in the second, and a spin lock taken
/// with interrupts on could be interrupted by its own line on the CPU that
/// holds it, which would then spin on itself.
///
/// notify() holds L1 while the handlers run. That is what makes detach() a
/// barrier: it takes the same lock, so it cannot return while a handler of this
/// line is still running on another CPU, and once it has returned no handler of
/// the line reaches the observer again. The price is that a handler must not
/// attach or detach on its own line: it would spin on the lock its own dispatch
/// holds.
///
/// A Permanent line skips L1 when it dispatches. Its list is fixed for good
/// once Permanent is set: attach() refuses everything from then on and
/// detach() refuses its observer, so there is nothing for the lock to keep
/// out, and the kernel's own lines -- the timer above all, which fires on
/// every tick -- are spared the lock on every interrupt.
class IrqObject final {
    using Self = IrqObject;

  public:
    static auto create(HIrqNum hirq, VIrqNum virq, IrqFlags flags, char const *name) -> Self *;

    /// Binds the line to the controller that drives its pin. Called once,
    /// by the line table as it builds the line and before any interrupt or
    /// any other CPU can reach it: mask() and unmask() read the chip without
    /// a lock, which is only safe because it never changes afterwards.
    /// BadState if the line has a chip already.
    [[nodiscard]]
    auto init(IrqChip const &chip) -> Status;

    /// See irq::attach(). An observer that says Permanent may only take an
    /// empty line, and makes the line Permanent.
    [[nodiscard]]
    auto attach(IrqObserver &observer) -> Status;

    /// See irq::detach(). A barrier: returns only once no handler of the line
    /// is running anywhere.
    [[nodiscard]]
    auto detach(IrqObserver &observer) -> Status;

    /// Calls every observer's handler, in interrupt context, and answers
    /// Handled if any of them handled the interrupt. An interrupt nobody
    /// handled is counted as spurious.
    [[nodiscard]]
    auto notify() -> IrqReturn;

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
    /// L1 -> LI of the design's lock order.
    [[nodiscard]]
    auto configure(TriggerMode trigger, Polarity polarity) -> Status;

    /// How many interrupts on this line no observer handled, since boot.
    [[nodiscard]]
    auto spurious_count() const -> u64 {
        return m_spurious.load(std::memory_order_relaxed);
    }

  private:
    IrqObject(HIrqNum hirq, VIrqNum virq, IrqFlags flags, char const *name);

    /// Read without L1, by notify(): the one bit of the flags that is. The
    /// acquire pairs with the release in attach() that sets it, so a
    /// dispatch that sees Permanent also sees the observer linked before it.
    [[nodiscard]]
    auto is_permanent() const -> bool {
        return !!(m_flags.load(std::memory_order_acquire) & IrqFlags::Permanent);
    }

    /// The list of a Permanent line, which notify() walks without L1. Hidden
    /// from the thread-safety analysis, which cannot know that a Permanent
    /// line's list never changes again.
    TA_NO_THREAD_SAFETY_ANALYSIS auto permanent_observers() -> IrqObserver::List & {
        return m_observers;
    }

    gktl::Canary m_canary;
    sync::SpinLock m_lock;
    /// Every bit is written under `m_lock`. The word is atomic only because
    /// notify() reads Permanent without it; Shared is read under the lock.
    std::atomic<IrqFlags> m_flags;
    /// Written by the constructor and, for its chip, once by init() before
    /// the line is published; read-only from then on, so read without L1.
    IrqData m_data;
    IrqObserver::List m_observers TA_GUARDED(m_lock);
    /// Bumped by notify(), with or without `m_lock` -- a Permanent line's
    /// dispatch holds none -- so it is atomic. A statistic: relaxed.
    std::atomic<u64> m_spurious{0};
    [[maybe_unused]]
    char const *m_name;
};
} // namespace ours::irq

#endif // OURS_IRQ_IRQ_OBJECT_HPP
