/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_MOD_HPP
#define OURS_IRQ_MOD_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/irq/types.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace arch {
struct IrqFrame;
}

namespace ours::irq {
auto init_early(usize num_irqs) -> void;
auto request_irq(VIrqNum virqnum, IrqHandler handler, IrqFlags flags, char const *name) -> Status;
auto release_irq(VIrqNum virqnum) -> Status;
auto set_affinity(VIrqNum virqnum, CpuMask const &targets) -> Status;
auto handle_irq_generic(VIrqNum virqnum) -> IrqReturn;
auto handle_irq_preamble(HIrqNum irqnum, arch::IrqFrame *frame) -> void;
auto handle_irq_postamble() -> bool;

/// Trap frame of the interrupt currently handled on this CPU, or nullptr
/// outside interrupt context.
auto current_frame() -> arch::IrqFrame *;
} // namespace ours::irq

#endif // OURS_IRQ_MOD_HPP
