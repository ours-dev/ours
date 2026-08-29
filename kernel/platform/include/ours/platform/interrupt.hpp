/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PLATFORM_INTERRUPT_HPP
#define OURS_PLATFORM_INTERRUPT_HPP 1

#include <arch/x86/interrupt.hpp>
#include <ours/types.hpp>

namespace ours {
auto platform_handle_irq(HIrqNum irqnum, arch::IrqFrame *) -> void;

auto platform_start_handling_irq(HIrqNum irqnum, arch::IrqFrame *) -> void;

auto platform_finish_handling_irq(HIrqNum irqnum, arch::IrqFrame *) -> bool;

} // namespace ours

#endif // #ifndef OURS_PLATFORM_INTERRUPT_HPP