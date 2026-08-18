/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ARCH_INTERRUPT_HPP
#define ARCH_INTERRUPT_HPP 1

// See arch/tick.hpp for why the architecture completion is pulled in here
// rather than left for each caller to include separately. Unlike Tick, the
// state snapshot's layout (e.g. x86's RFLAGS) is itself architecture-
// specific, so — other than the fallback branch below, which exists only so
// this header parses when no architecture is selected — there's nothing
// generic to declare up front: each arch/<arch>/interrupt.hpp defines
// ArchCpuState, IrqSavedState, and the five functions below directly.
#if defined(ARCH_X86)
#include <arch/x86/interrupt.hpp>
#elif defined(ARCH_ARM)
#include <arch/arm/interrupt.hpp>
#elif defined(ARCH_RISCV)
#include <arch/riscv/interrupt.hpp>
#else

namespace arch {

class ArchCpuState;
using IrqSavedState = ArchCpuState;

auto local_irq_disable() noexcept -> void;
auto local_irq_enable() noexcept -> void;

[[nodiscard]]
auto irqs_disabled() noexcept -> bool;

[[nodiscard]]
auto local_irq_save_state() noexcept -> IrqSavedState;

auto local_irq_restore_state(IrqSavedState state) noexcept -> void;

} // namespace arch

#endif

#endif // ARCH_INTERRUPT_HPP
