/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_IRQ_CHIP_HPP
#define OURS_IRQ_IRQ_CHIP_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours {
class CpuMask;
}

namespace ours::irq {
struct IrqChip;

/// Per-interrupt state handed to architecture interrupt-controller hooks.
/// The generic IRQ layer owns this object; a chip may use `platform_data`
/// for controller-specific routing state without changing the common ABI.
struct IrqData final {
    constexpr IrqData(HIrqNum hirq, VIrqNum virq, IrqChip *controller = nullptr,
                      void *private_data = nullptr) noexcept
        : hirqnum(hirq), virqnum(virq), chip(controller), platform_data(private_data) {}

    HIrqNum hirqnum;
    VIrqNum virqnum;
    IrqChip *chip;
    void *platform_data;
};

/// Architecture-controller operations used by the generic IRQ domain.
/// Missing optional operations are represented by null pointers.
struct IrqChip final {
    auto (*startup)(IrqData &) -> Status = nullptr;
    auto (*mask)(IrqData &) -> void = nullptr;
    auto (*unmask)(IrqData &) -> void = nullptr;
    auto (*issue_eoi)(IrqData &) -> void = nullptr;
    auto (*issue_ack)(IrqData &) -> void = nullptr;
    auto (*set_affinity)(IrqData &, CpuMask const *) -> Status = nullptr;
    auto (*dump)() -> void = nullptr;
    char const *name = nullptr;
};
} // namespace ours::irq

#endif // #ifndef OURS_IRQ_IRQ_CHIP_HPP
