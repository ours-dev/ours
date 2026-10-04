/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_AUTO_INTERRUPT_ENABLER_HPP
#define ARCH_AUTO_INTERRUPT_ENABLER_HPP 1

#include <arch/interrupt.hpp>

namespace arch {
/// RAII object that turns the current CPU's interrupts on for its scope, and
/// puts back the state it found when the scope ends: off again if they were
/// off. AutoInterruptDisabler's counterpart.
///
/// For the stretches of an exception or system call path that may block --
/// a page fault being resolved, a system call's handler, a user exception
/// being delivered -- entered with interrupts off and left with them off.
/// Written as a scope rather than as a local_irq_enable() and a matching
/// local_irq_disable(), so that no early return can leave the scope with them
/// on.
///
/// A path that turns them on and never comes back (a thread that exits, the
/// idle loop) has no scope to give back, and calls local_irq_enable() itself.
class AutoInterruptEnabler {
    using Self = AutoInterruptEnabler;

  public:
    AutoInterruptEnabler() : m_were_disabled(irqs_disabled()) {
        local_irq_enable();
    }

    ~AutoInterruptEnabler() {
        if (m_were_disabled) {
            local_irq_disable();
        }
    }

    AutoInterruptEnabler(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

  private:
    bool const m_were_disabled;
};

} // namespace arch

#endif // ARCH_AUTO_INTERRUPT_ENABLER_HPP
