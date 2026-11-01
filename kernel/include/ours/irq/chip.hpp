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

/// Operations of one interrupt controller, as the line layer uses them. A
/// missing optional operation is a null pointer. Every operation takes the
/// line's data read-only.
///
/// What the chip of every controller keeps to, whatever the architecture:
///   - Where the operations run. mask(), unmask() and configure() are called
///     under the line's lock, L1, from interrupt context or from a thread
///     with interrupts off; issue_eoi() in interrupt context, under L1 unless
///     the line is Permanent; trigger_mode() and is_masked() without L1. None
///     sleeps, and none waits for another CPU; a bounded poll of a register is
///     fine.
///   - Each line has a lock of its own, so two lines of one controller are
///     written from two CPUs at once. The chip keeps nothing in the line's
///     data, and whatever its registers need is serialised by a lock of the
///     chip's own, taken inside L1 (on x86, LI in `io_apic.cpp`). A chip may
///     keep a table of its own, an entry for each source, for a fact the
///     controller has no register for (below); trigger_mode() reads it
///     without L1, so its entries are atomic.
///   - issue_eoi() ends the interrupt on the CPU that took it, inside the
///     dispatch. It is never deferred to finalize_oneshot() or to a thread: a
///     controller whose end of interrupt belongs to the CPU that took the
///     interrupt (a claim and its complete, an acknowledge and its end) takes
///     it from there.
///   - configure() refuses with NotSupported a mode the controller cannot do,
///     and leaves the source masked. A controller with no register for the
///     mode records the mode it was told, and answers it from trigger_mode().
///   - A chip with several instances finds the one that drives a source from
///     the line's hardware number, as the IO-APIC's does. `platform_data` is
///     for a fact that number does not give; nothing needs it yet.
///   - A controller whose mask takes effect on another CPU, later, cannot hold
///     a Oneshot line, which stays masked from its interrupt until the owner's
///     ack. The layer cannot tell (check_oneshot() only asks that mask() and
///     unmask() exist), so such a chip leaves unmask() null: a Oneshot action
///     is refused, and so is irq::unmask(). A flag for it is MSI's to add.
struct Chip final {
    auto (*startup)(Data const &) -> Status = nullptr;
    /// Masks the line at the controller. kernel/irq calls it under L1, and
    /// only there: irq::mask(); the line's flow, for an interrupt on a line
    /// nobody is on and before a Oneshot line's action; attach() with no
    /// mode; and detach(), as the last action goes. Every chip a line of the
    /// table is bound to has one: init_lines() refuses a chip without.
    auto (*mask)(Data const &) -> void = nullptr;
    /// Unmasks the line at the controller. kernel/irq calls it under L1, and
    /// only when the line is not held: irq::unmask(); the level flow, after a
    /// Oneshot line's action; and finalize_oneshot(), which clears held
    /// first. A line whose chip has none refuses a Oneshot action.
    auto (*unmask)(Data const &) -> void = nullptr;
    /// Sets the line's trigger mode and polarity, and leaves it masked.
    auto (*configure)(Data const &, TriggerMode, Polarity) -> Status = nullptr;
    /// The line's trigger mode now, read from the controller, which keeps it:
    /// what configure() last set, or what the platform programmed at boot.
    /// Read when the line is bound to the chip, to choose its flow; a line
    /// whose chip cannot say gets the edge flow, and refuses a Oneshot action
    /// that comes without a mode, since that flow does not hold a line.
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
    /// Aims the source at one of the CPUs in the set. Not called yet:
    /// irq::set_affinity() answers NotSupported without asking the chip, until
    /// the work of interrupt-system P4 settles the call.
    auto (*set_affinity)(Data const &, CpuMask const *) -> Status = nullptr;
    auto (*dump)() -> void = nullptr;
    char const *name = nullptr;
};
} // namespace ours::irq

#endif // #ifndef OURS_IRQ_CHIP_HPP
