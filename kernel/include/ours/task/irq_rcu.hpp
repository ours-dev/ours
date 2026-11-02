/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_TASK_IRQ_RCU_HPP
#define OURS_TASK_IRQ_RCU_HPP 1

#include <ours/assert.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>

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
///
/// A read is also never switched out. Interrupts off keep the CPU from taking
/// the rendezvous, but not a thread from being parked in the middle of its
/// read: a read that wakes another thread on this CPU asks for a reschedule,
/// and the next time the preemption count falls to zero (the release of a
/// lock that disables preemption, say) the thread is switched out with its
/// read open, and the CPU runs another thread with interrupts on. So the
/// guard holds preemption off as well, for a thread; before the first thread
/// exists, at boot, there is none to switch and nothing to hold. A handler
/// has both off by its gate and its preamble.
///
/// A CPU reads only while it is in the online mask: the rendezvous asks the
/// online CPUs. The only reads outside it are on the way in and the way out
/// of a CPU's life, with interrupts off, before the bit is set and after it
/// is cleared, and those must not touch what this protects.

/// A read of what irq_rcu_synchronize() protects: interrupts are off on this
/// CPU for the guard's life. Nests, and is legal with interrupts already off
/// (a handler, or inside a spin lock), in which case it changes nothing.
class IrqRcuReadGuard final {
  public:
    IrqRcuReadGuard() : m_preempt(AutoPreemptDisabler::Defer) {
        // After the interrupts are off, so the current thread cannot change
        // under the check, and before the thread is set up there is none.
        if (Thread::Current::get() != nullptr) {
            m_preempt.disable();
        }
    }

    USTL_NO_MOVEABLE_AND_COPYABLE(IrqRcuReadGuard);
    ~IrqRcuReadGuard() = default;

  private:
    /// Declared in this order on purpose: the members are destroyed last
    /// first, so a reschedule that waited for the read to end is delivered
    /// here, with interrupts still off and nothing left open.
    arch::AutoInterruptDisabler m_irqs;
    AutoPreemptDisabler m_preempt;
};

/// Returns once every read that began before the call has ended, and so
/// every pointer a writer had retracted before the call is no longer in use.
///
/// Thread context, interrupts and preemption on, no spin lock held, not
/// inside a guard, not on an idle thread, and not under cpu_hotplug_lock(),
/// which it takes: all asserted. A Dpc callback must not call it either,
/// since take_cpu_offline() waits for a running Dpc with that lock held (this
/// one is by convention, a Dpc worker being a thread like any other), and a
/// CPU hotplug hook runs with interrupts off, which the assert catches. It
/// waits for the longest span with interrupts off on any online CPU, as a
/// TLB shootdown does, and holds the hotplug lock for as long, so nothing
/// that can run under that lock may wait for it.
///
/// What the argument rests on for a CPU that joins while it runs, an
/// application processor that took longer than the boot waited for it being
/// the one that can: the full fence between the writer's retract and its read
/// of the online mask, and the joining CPU's store of its bit being ordered
/// before its first read (Cpu::set_state()). Either the mask read sees the
/// bit and the CPU is asked, or the CPU's read sees the retract. The hotplug
/// lock and the second read of the mask narrow what can change during the
/// call; they are not what the argument needs.
auto irq_rcu_synchronize() -> void;

/// A pointer to a thing readers find through it, which a writer installs
/// into an empty pointer and removes with retract() and
/// irq_rcu_synchronize(). Writers are serialised by their caller.
template <typename T>
class IrqRcuPointer final {
  public:
    /// Makes |object| what readers find, in a pointer that holds nothing. Its
    /// contents are written before the call: the release is what lets a
    /// reader that finds the pointer see them.
    auto publish(T *const object) -> void {
        DEBUG_ASSERT(m_object.load(std::memory_order_relaxed) == nullptr);
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
