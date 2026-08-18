/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_RISCV_INTERRUPT_HPP
#define ARCH_RISCV_INTERRUPT_HPP 1

// Only ever reached via <arch/interrupt.hpp>'s own #include — don't include
// this file directly.

#include <arch/types.hpp>

namespace arch {

/// Just the prior SIE (S-mode Interrupt Enable, sstatus bit 1) state, not a
/// full register snapshot — SIE is the only bit local_irq_*() ever touches,
/// so a bool is enough (mirrors Zircon's own riscv64
/// `interrupt_saved_state_t`, which is likewise a plain bool rather than a
/// full-register capture like x86/arm64 use here).
class ArchCpuState final {
  public:
    constexpr ArchCpuState() noexcept = default;

    explicit constexpr ArchCpuState(bool sie) noexcept : m_sie(sie) {}

    [[nodiscard]]
    constexpr auto sie() const noexcept -> bool {
        return m_sie;
    }

  private:
    bool m_sie{};
};

using IrqSavedState = ArchCpuState;

inline auto local_irq_disable() noexcept -> void {
    // csrci: clear bit 1 (SIE) of sstatus.
    asm volatile("csrci sstatus, 2" ::: "memory");
}

inline auto local_irq_enable() noexcept -> void {
    // csrsi: set bit 1 (SIE) of sstatus.
    asm volatile("csrsi sstatus, 2" ::: "memory");
}

[[nodiscard]]
inline auto irqs_disabled() noexcept -> bool {
    usize sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    return (sstatus & usize(2)) == 0;
}

[[nodiscard]]
inline auto local_irq_save_state() noexcept -> IrqSavedState {
    usize prev;
    // csrrci: atomically read sstatus, then clear bit 1 (SIE).
    asm volatile("csrrci %0, sstatus, 2" : "=r"(prev)::"memory");
    return ArchCpuState((prev & usize(2)) != 0);
}

inline auto local_irq_restore_state(IrqSavedState state) noexcept -> void {
    if (state.sie()) {
        asm volatile("csrsi sstatus, 2" ::: "memory");
    }
}

} // namespace arch

#endif // ARCH_RISCV_INTERRUPT_HPP
