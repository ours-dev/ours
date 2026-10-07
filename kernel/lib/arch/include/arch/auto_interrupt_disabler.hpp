/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef AUTO_INTERRUPT_DISABLER_HPP
#define AUTO_INTERRUPT_DISABLER_HPP 1

#include <arch/interrupt.hpp>

namespace arch {
/// RAII object to save/restore the current CPU's interrupt-enable state.
class AutoInterruptDisabler {
    using Self = AutoInterruptDisabler;

  public:
    enum class Defer {};

    AutoInterruptDisabler() : m_restore(true), m_state(local_irq_save_state()) {}

    AutoInterruptDisabler(Defer) : m_restore(false) {}

    ~AutoInterruptDisabler() {
        reenable();
    }

    auto reenable() -> void {
        if (m_restore) {
            local_irq_restore_state(m_state);
            m_restore = false;
        }
    }

  private:
    /// Whether this scope still owes the restore of m_state. A scope
    /// object's one fact, so it has no word to share.
    bool m_restore;
    IrqSavedState m_state;
};

} // namespace arch

#endif // AUTO_INTERRUPT_DISABLER_HPP
