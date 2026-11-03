/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/irq/types.hpp>
#include <ours/platform/isa.hpp>

namespace ours::platform {
/// An ARM platform has no ISA interrupts: no number is an ISA IRQ.
auto isa_irq_line(u32) -> IrqId {
    return IrqId::Invalid;
}
} // namespace ours::platform
