/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_TYPES_HPP
#define OURS_IRQ_TYPES_HPP 1

#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours::irq {
inline constexpr auto INVALID_V_IRQ_NUM = static_cast<VIrqNum>(-1);

/// What a handler answers for one interrupt on its line.
enum class IrqReturn : u8 {
    /// The interrupt was not this handler's device's. A line whose actions
    /// all answer None counts the interrupt as spurious.
    None,
    /// The device raised it, and the handler is done with it.
    Handled,
    /// The device raised it, and the handler has handed the rest of the work
    /// to a thread, which it has woken. Counts as handled. On a Oneshot line
    /// it also leaves the line held: masked until the action's
    /// irq::finalize_oneshot() says the thread is done.
    WakeThread,
};

enum class IrqFlags : u32 {
    None,
    Shared = 0_bit,
    Permanent = 1_bit,
    /// Asks for the line to stay masked from an interrupt the action's
    /// handler answers WakeThread for until the action calls
    /// irq::finalize_oneshot(). On a level line that is what keeps a device
    /// still asserting its pin from raising the line again before the thread
    /// has serviced it; an edge line raises nothing until the device signals
    /// anew, so its flow ignores the flag. Refused together with Permanent
    /// or Shared.
    Oneshot = 2_bit,
};
USTL_ENABLE_ENUM_BITMASK(IrqFlags);

/// Whether a dispatch of a line ends the interrupt at the line's controller,
/// through its chip, once the actions have run.
enum class Eoi : u8 {
    /// A real interrupt. It stays in service until it is ended, and while it
    /// is, the controller holds back every interrupt of its priority and
    /// below on that CPU.
    Send,
    /// An interrupt a kernel test plays from thread context. Nothing is in
    /// service on this CPU to end, and an end of interrupt that reaches the
    /// line's controller itself would tell it the line was serviced while a
    /// real interrupt from it may still be in service on another CPU.
    Skip,
};

/// How a line signals: by an edge, once per event, or by a level held until
/// the device is serviced. A controller's own encoding stays in its chip,
/// which translates.
enum class TriggerMode : u8 {
    Edge,
    Level,
};

/// Which edge, or which level, means "asserted".
enum class Polarity : u8 {
    ActiveHigh,
    ActiveLow,
};

/// A line's flow: the order in which an interrupt on the line masks the
/// line, runs its actions and ends the interrupt at the controller. It
/// follows the line's trigger mode, because the two kinds of pin ask
/// different things of the line while an interrupt is handled: a level pin
/// stays asserted until its device is serviced and raises the line again
/// the moment it is unmasked and ended, while an edge is one event that
/// nothing raises a second time.
///
/// Chosen when the line is bound to its chip, from how the chip says the pin
/// signals, and chosen again whenever the line's mode is set: by configure()
/// and by an attach that sets or reads the mode.
enum class Flow : u8 {
    /// For an edge-triggered line: handle_edge.
    Edge,
    /// For a level-triggered line, on a controller that is told the
    /// interrupt is over by one end of interrupt after the actions:
    /// handle_fasteoi.
    FastEoi,
};

/// How a line signals, whole: what an owner asks for when it sets a line's
/// mode as it takes the line (attach() with a mode).
struct LineMode {
    TriggerMode trigger;
    Polarity polarity;
};

/// An interrupt handler. `context` is whatever the action that carries the
/// handler was constructed with (Action), so the handler finds its
/// device's state there rather than in a global.
using IrqHandler = auto (*)(VIrqNum virqnum, void *context) -> IrqReturn;
} // namespace ours::irq

#endif // OURS_IRQ_TYPES_HPP
