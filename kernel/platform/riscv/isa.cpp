/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/platform/isa.hpp>

namespace ours::platform {
/// A RISC-V platform has no ISA interrupts, so no number is an ISA IRQ to
/// move: each names its line already.
auto remap_isa_irq(u32 const irq) -> VIrqNum {
    return irq;
}
} // namespace ours::platform
