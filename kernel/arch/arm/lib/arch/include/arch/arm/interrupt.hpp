/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_ARM_INTERRUPT_HPP
#define ARCH_ARM_INTERRUPT_HPP 1

// Only ever reached via <arch/interrupt.hpp>'s own #include — don't include
// this file directly.

#include <arch/types.hpp>

namespace arch {

/// Snapshot of PSTATE.DAIF (the whole register, not just the I bit — mirrors
/// x86's ArchCpuState capturing all of RFLAGS). Only maskable IRQs (the I
/// bit) are touched by local_irq_disable()/enable(), matching the scope of
/// x86's cli/sti; save/restore round-trip the full register so anything
/// else in it is left exactly as found.
class ArchCpuState final {
  public:
    constexpr ArchCpuState() noexcept = default;

    [[nodiscard]]
    static auto read() noexcept -> ArchCpuState {
        usize value;
        asm volatile("mrs %0, daif" : "=r"(value));
        return ArchCpuState(value);
    }

    auto write() const noexcept -> void {
        asm volatile("msr daif, %0" : : "r"(m_value) : "memory");
    }

  private:
    explicit constexpr ArchCpuState(usize value) noexcept : m_value(value) {}

    usize m_value{};
};

using IrqSavedState = ArchCpuState;

inline auto local_irq_disable() noexcept -> void {
    // DAIFSet/DAIFClr immediate bit 1 is the I (IRQ mask) bit.
    asm volatile("msr daifset, #2" ::: "memory");
}

inline auto local_irq_enable() noexcept -> void {
    asm volatile("msr daifclr, #2" ::: "memory");
}

[[nodiscard]]
inline auto irqs_disabled() noexcept -> bool {
    usize daif;
    asm volatile("mrs %0, daif" : "=r"(daif));
    // The DAIF system register (as opposed to the DAIFSet/DAIFClr
    // immediates) packs D/A/I/F into bits [9:6]; I is bit 7.
    return (daif & (usize(1) << 7)) != 0;
}

[[nodiscard]]
inline auto local_irq_save_state() noexcept -> IrqSavedState {
    auto const state = ArchCpuState::read();
    local_irq_disable();
    return state;
}

inline auto local_irq_restore_state(IrqSavedState state) noexcept -> void {
    state.write();
}

} // namespace arch

#endif // ARCH_ARM_INTERRUPT_HPP
