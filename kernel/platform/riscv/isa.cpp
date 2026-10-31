/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/irq/types.hpp>
#include <ours/platform/isa.hpp>

namespace ours::platform {
/// A RISC-V platform has no ISA interrupts: no number is an ISA IRQ.
auto isa_irq_line(u32) -> VIrqNum {
    return irq::INVALID_V_IRQ_NUM;
}
} // namespace ours::platform
