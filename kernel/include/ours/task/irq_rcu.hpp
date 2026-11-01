/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_IRQ_RCU_HPP
#define OURS_TASK_IRQ_RCU_HPP 1

#include <ours/assert.hpp>

#include <atomic>

#include <arch/auto_interrupt_disabler.hpp>
#include <arch/interrupt.hpp>
#include <ustl/util/noncopyable.hpp>

namespace ours::task {
/// Read-copy-update for state that is read with interrupts off and removed
/// rarely: the interrupt controllers' directory (.agents/tasks/irq-multi-arch/
/// DESIGN.md, "Reclaiming a removed domain").
///
/// A reader runs with interrupts off from the moment it loads a pointer to the
/// last time it uses what the pointer names, in an IrqRcuReadGuard or in an
/// interrupt handler, whose gate has interrupts off already. A writer that
/// wants to free something unlinks it first (IrqRcuPointer::retract() or its
/// own list), then calls irq_rcu_synchronize(), and frees after it returns.
///
/// What makes that safe is that irq_rcu_synchronize() runs a function on
/// every online CPU through an inter-processor interrupt and returns only once
/// they all have, and an interrupt is not taken on a CPU that has interrupts
/// off. So each CPU takes it at an instant when no reader is open on it, and
/// every reader is wholly before that instant, and done with the object, or
/// wholly after it, and then finds the unlinked object gone. There is no
/// counter and no report from the scheduler: a CPU that is idle is woken by
/// the interrupt, and one that runs a single thread for ever is reached the
/// same way.
///
/// The price is that interrupts are off for the whole read, so a read is
/// short and never blocks: no sleeping, no allocation that can wait, no copy
/// from user memory, no mutex. That is also what a read of the line layer is
/// already, because the line lock is taken with interrupts off.

/// A read of what irq_rcu_synchronize() protects: interrupts are off on this
/// CPU for the guard's life. Nests, and is legal with interrupts already off
/// (a handler, or inside a spin lock), in which case it changes nothing.
class IrqRcuReadGuard {
  public:
    IrqRcuReadGuard() = default;
    USTL_NO_MOVEABLE_AND_COPYABLE(IrqRcuReadGuard);
    ~IrqRcuReadGuard() = default;

  private:
    arch::AutoInterruptDisabler m_irqs;
};

/// Returns once every read that began before the call has ended, and so
/// every pointer a writer had retracted before the call is no longer in use.
///
/// Thread context, interrupts and preemption on, no spin lock held, not
/// inside a guard, and not under cpu_hotplug_lock(), which it takes to hold
/// the set of online CPUs still: a Dpc callback or a CPU hotplug hook must not
/// call it. It waits for the longest span with interrupts off on any online
/// CPU, as a TLB shootdown does, and holds the hotplug lock for as long.
///
/// A CPU that comes online while it runs is reached too. The ones the lock
/// covers cannot arrive halfway; one that starts without it (an application
/// processor that took longer than the boot waited for it) is found by
/// reading the online mask again after the others have answered.
auto irq_rcu_synchronize() -> void;

/// A pointer to a thing readers find through it and a writer replaces or
/// removes under irq_rcu_synchronize().
template <typename T>
class IrqRcuPointer {
  public:
    /// Makes |object| what readers find. Its contents are written before the
    /// call: the release is what lets a reader that finds the pointer see
    /// them.
    auto publish(T *const object) -> void {
        m_object.store(object, std::memory_order_release);
    }

    /// What readers find now, for a reader in |guard|.
    [[nodiscard]]
    auto read(IrqRcuReadGuard const &) const -> T * {
        return read_with_irqs_off();
    }

    /// What readers find now, for an interrupt handler or any other code
    /// that has interrupts off by being where it is, with no guard to show.
    [[nodiscard]]
    auto read_with_irqs_off() const -> T * {
        DEBUG_ASSERT(arch::irqs_disabled());
        return m_object.load(std::memory_order_acquire);
    }

    /// Makes readers that begin from now on find nothing, and returns what
    /// they found until now. The caller owns the answer from here but may not
    /// free it until irq_rcu_synchronize() has returned.
    [[nodiscard]]
    auto retract() -> T * {
        return m_object.exchange(nullptr, std::memory_order_acq_rel);
    }

  private:
    std::atomic<T *> m_object{nullptr};
};
} // namespace ours::task

#endif // #ifndef OURS_TASK_IRQ_RCU_HPP
