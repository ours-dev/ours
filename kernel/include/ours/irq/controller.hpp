/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_CONTROLLER_HPP
#define OURS_IRQ_CONTROLLER_HPP 1

#include <ours/cpu_mask.hpp>
#include <ours/irq/types.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours::irq {
struct ControllerOps;

/// Per-interrupt state handed to architecture interrupt-controller hooks.
/// The generic IRQ layer owns this object; a chip may use `platform_data`
/// for controller-specific routing state without changing the common ABI.
///
/// A chip finds the pin it drives from `hirqnum` alone: on x86 that is the
/// GSI, which names one IO-APIC and one of its pins.
struct ControllerSource final {
    constexpr ControllerSource(HIrqNum hirq, VIrqNum virq,
                               ControllerOps const *controller = nullptr,
                               void *private_data = nullptr) noexcept
        : hirqnum(hirq), virqnum(virq), chip(controller), platform_data(private_data) {}

    HIrqNum hirqnum;
    VIrqNum virqnum;
    ControllerOps const *chip;
    void *platform_data;
};

/// Operations of one interrupt controller, as the line layer uses them. A
/// missing optional operation is a null pointer. Every operation takes the
/// line's data read-only.
///
/// What the chip of every controller keeps to, whatever the architecture:
///   - Where the operations run. mask(), unmask() and configure() are called
///     under the line's lock, L1, from interrupt context or from a thread
///     with interrupts off; issue_eoi() under L1 in interrupt context;
///     trigger_mode() with L1 (the bind and the attach
///     that reads the mode call it so) or without it (irq::trigger_mode());
///     is_masked() without. So no operation takes a line lock, and the chip's
///     own lock is a leaf that is safe with interrupts off. None sleeps, and
///     none waits for another CPU to act: taking the chip's own lock, held
///     across a few register accesses, is not that, and a bounded poll of a
///     register is fine.
///   - Each line has a lock of its own, so two lines of one controller are
///     written from two CPUs at once. The chip keeps nothing in the line's
///     data, and whatever its registers need is serialised by a lock of the
///     chip's own, taken inside L1 (on x86, LI in `io_apic.cpp`).
///   - issue_eoi() ends the interrupt on the CPU that took it, inside the
///     dispatch. It is never deferred to finalize_oneshot() or to a thread: a
///     controller whose end of interrupt belongs to the CPU that took the
///     interrupt (a claim and its complete, an acknowledge and its end) takes
///     it from there.
///   - issue_eoi() is called on a source that mask() has masked: for an
///     interrupt on a line nobody is on, and for a Oneshot interrupt held for
///     its owner. A controller that ignores the end of interrupt of a masked
///     source masks some other way.
///   - configure() refuses with NotSupported a mode the controller cannot do.
///     On success it leaves the source masked; on a refusal it leaves it as it
///     was. A controller with no register for the mode, whose trigger form is
///     the wiring's, accepts the mode it is told (the firmware's, which it
///     cannot check) and leaves trigger_mode() null: the line takes the flow
///     of the mode it is told, and an owner that gives none is refused, a
///     Oneshot one included (see trigger_mode()).
///   - A chip with several instances finds the one that drives a source from
///     the line's hardware number, as the IO-APIC's does. That works because
///     the hardware numbers of all the controllers of a machine are one
///     numbering, disjoint for each instance, served by one table and one chip.
///   - startup(), issue_ack() and dump() are not called; leave them null.
///     `platform_data` is null, and nothing sets it yet.
///   - A controller whose mask takes effect on another CPU, later, cannot hold
///     a Oneshot line, which stays masked from its interrupt until the owner's
///     ack. The layer cannot tell (check_registration() only asks that mask() and
///     unmask() exist), so for now such a chip leaves unmask() null: a Oneshot
///     action is refused, and so is irq::unmask(), which every owner calls to
///     start its line, so the chip serves no owner until a flag for it exists.
///     That flag is MSI's to add.
struct ControllerOps final {
    auto (*startup)(ControllerSource const &) -> Status = nullptr;
    /// Masks the line at the controller. kernel/irq calls it under L1, and
    /// only there: irq::mask(); the line's flow, for an interrupt on a line
    /// nobody is on and before a Oneshot line's action; the attach that reads
    /// the mode; and detach(), as the last action goes. Every chip a line of the
    /// table is bound to has one: register_domain() refuses a chip without.
    auto (*mask)(ControllerSource const &) -> void = nullptr;
    /// Unmasks the line at the controller. kernel/irq calls it under L1, and
    /// only when the line is not held: irq::unmask(); the level flow, after a
    /// Oneshot line's action; and finalize_oneshot(), which clears held
    /// first. A line whose chip has none refuses a Oneshot action.
    auto (*unmask)(ControllerSource const &) -> void = nullptr;
    /// Sets the line's trigger mode and polarity, and leaves it masked.
    auto (*configure)(ControllerSource const &, TriggerMode, Polarity) -> Status = nullptr;
    /// The line's trigger mode now, read from the controller, which keeps it:
    /// what configure() last set, or what the platform programmed at boot.
    /// Read when the line is bound to the chip, to choose its flow, and by the
    /// attach that reads the mode. Null on a controller that cannot read its
    /// mode: its lines get the edge flow until an owner gives them a mode
    /// (configure(), or an attach with a mode), the attach that reads the mode
    /// answers NotSupported, and so does the attach with no mode for a Oneshot
    /// action, since the edge flow does not hold a line.
    auto (*trigger_mode)(ControllerSource const &) -> TriggerMode = nullptr;
    /// Whether the line is masked at the controller now, read from the
    /// controller, which keeps the one record of it. For kernel tests only,
    /// through irq::is_masked_for_test(): kernel/irq keeps no copy of a
    /// line's mask and never asks for it, so a read is the only way a test
    /// sees what a line's flow and its owner leave at the controller.
    auto (*is_masked)(ControllerSource const &) -> bool = nullptr;
    /// Ends an interrupt on the line, so the controller delivers the next.
    /// Called by the line's flow once its actions have run, once per
    /// interrupt, in interrupt context, under L1. Every chip a line of the table is bound to has one:
    /// register_domain() refuses a chip without.
    auto (*issue_eoi)(ControllerSource const &) -> void = nullptr;
    auto (*issue_ack)(ControllerSource const &) -> void = nullptr;
    /// Aims the source at one of the CPUs in the set. Not called yet:
    /// irq::set_affinity() answers NotSupported without asking the chip, until
    /// the work of interrupt-system P4 settles the call.
    auto (*set_affinity)(ControllerSource const &, CpuMask const *) -> Status = nullptr;
    auto (*dump)() -> void = nullptr;
    char const *name = nullptr;
};
} // namespace ours::irq

#endif // #ifndef OURS_IRQ_CONTROLLER_HPP
