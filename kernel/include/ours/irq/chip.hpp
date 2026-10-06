/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_CHIP_HPP
#define OURS_IRQ_CHIP_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/irq/types.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours::irq {
struct Chip;

/// Per-interrupt state handed to architecture interrupt-controller hooks.
/// The generic IRQ layer owns this object; a chip may use `platform_data`
/// for controller-specific routing state without changing the common ABI.
///
/// A chip finds the pin it drives from `hirqnum` alone: on x86 that is the
/// GSI, which names one IO-APIC and one of its pins.
struct Data final {
    constexpr Data(HIrqNum hirq, VIrqNum virq, Chip const *controller = nullptr,
                   void *private_data = nullptr) noexcept
        : hirqnum(hirq), virqnum(virq), chip(controller), platform_data(private_data) {}

    HIrqNum hirqnum;
    VIrqNum virqnum;
    Chip const *chip;
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
struct Chip final {
    auto (*startup)(Data const &) -> Status = nullptr;
    /// Masks the line at the controller. Besides the callers of irq::mask(),
    /// kernel/irq calls it under L1: the line's flow, for an interrupt on a
    /// line nobody is on and before a Oneshot line's action, and detach(),
    /// as the last action goes. Every chip a line of the table is bound to
    /// has one: init_lines() refuses a chip without.
    auto (*mask)(Data const &) -> void = nullptr;
    /// Unmasks the line at the controller. Besides the callers of
    /// irq::unmask(), kernel/irq calls it under L1: the level flow, after a
    /// Oneshot line's action, when the line is not held, and
    /// finalize_oneshot(). A line whose chip has none refuses a Oneshot
    /// action.
    auto (*unmask)(Data const &) -> void = nullptr;
    /// Sets the line's trigger mode and polarity, and leaves it masked.
    auto (*configure)(Data const &, TriggerMode, Polarity) -> Status = nullptr;
    /// The line's trigger mode now, read from the controller, which keeps it:
    /// what configure() last set, or what the platform programmed at boot.
    /// Read when the line is bound to the chip, to choose its flow; a line
    /// whose chip cannot say gets the edge flow.
    auto (*trigger_mode)(Data const &) -> TriggerMode = nullptr;
    /// Whether the line is masked at the controller now, read from the
    /// controller, which keeps the one record of it. For kernel tests only,
    /// through irq::is_masked_for_test(): kernel/irq keeps no copy of a
    /// line's mask and never asks for it, so a read is the only way a test
    /// sees what a line's flow and its owner leave at the controller.
    auto (*is_masked)(Data const &) -> bool = nullptr;
    /// Ends an interrupt on the line, so the controller delivers the next.
    /// Called by the line's flow once its actions have run, once per
    /// interrupt, in interrupt context, under L1 unless the line is
    /// Permanent. Every chip a line of the table is bound to has one:
    /// init_lines() refuses a chip without.
    auto (*issue_eoi)(Data const &) -> void = nullptr;
    auto (*issue_ack)(Data const &) -> void = nullptr;
    auto (*set_affinity)(Data const &, CpuMask const *) -> Status = nullptr;
    auto (*dump)() -> void = nullptr;
    char const *name = nullptr;
};
} // namespace ours::irq

#endif // #ifndef OURS_IRQ_CHIP_HPP
