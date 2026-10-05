/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_IRQ_CHIP_HPP
#define OURS_IRQ_IRQ_CHIP_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/irq/types.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours::irq {
struct IrqChip;

/// Per-interrupt state handed to architecture interrupt-controller hooks.
/// The generic IRQ layer owns this object; a chip may use `platform_data`
/// for controller-specific routing state without changing the common ABI.
///
/// A chip finds the pin it drives from `hirqnum` alone: on x86 that is the
/// GSI, which names one IO-APIC and one of its pins.
struct IrqData final {
    constexpr IrqData(HIrqNum hirq, VIrqNum virq, IrqChip const *controller = nullptr,
                      void *private_data = nullptr) noexcept
        : hirqnum(hirq), virqnum(virq), chip(controller), platform_data(private_data) {}

    HIrqNum hirqnum;
    VIrqNum virqnum;
    IrqChip const *chip;
    void *platform_data;
};

/// Architecture-controller operations used by the generic IRQ domain.
/// Missing optional operations are represented by null pointers.
///
/// Every operation takes the line's data read-only. mask() and unmask() run
/// without the line's lock, from interrupt context on one CPU while another
/// CPU calls them from a system call, so the chip cannot keep anything in the
/// line's data; whatever the controller's registers need is serialised by
/// the chip's own lock (on x86, LI in `io_apic.cpp`).
struct IrqChip final {
    auto (*startup)(IrqData const &) -> Status = nullptr;
    auto (*mask)(IrqData const &) -> void = nullptr;
    auto (*unmask)(IrqData const &) -> void = nullptr;
    /// Sets the line's trigger mode and polarity, and leaves it masked.
    auto (*configure)(IrqData const &, TriggerMode, Polarity) -> Status = nullptr;
    auto (*issue_eoi)(IrqData const &) -> void = nullptr;
    auto (*issue_ack)(IrqData const &) -> void = nullptr;
    auto (*set_affinity)(IrqData const &, CpuMask const *) -> Status = nullptr;
    auto (*dump)() -> void = nullptr;
    char const *name = nullptr;
};
} // namespace ours::irq

#endif // #ifndef OURS_IRQ_IRQ_CHIP_HPP
