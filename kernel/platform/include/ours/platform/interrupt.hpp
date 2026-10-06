/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PLATFORM_INTERRUPT_HPP
#define OURS_PLATFORM_INTERRUPT_HPP 1

#include <ours/types.hpp>

#include <arch/x86/interrupt.hpp>

namespace ours {
/// Hands the interrupt on platform interrupt number `irqnum` to the line of
/// that number, whose flow ends it. Answers false if no line has the number:
/// the interrupt has not been ended, and the caller ends it.
[[nodiscard]]
auto platform_handle_irq(HIrqNum irqnum, arch::IrqFrame *) -> bool;

auto platform_start_handling_irq(HIrqNum irqnum, arch::IrqFrame *) -> void;

auto platform_finish_handling_irq(HIrqNum irqnum, arch::IrqFrame *) -> bool;

} // namespace ours

#endif // #ifndef OURS_PLATFORM_INTERRUPT_HPP