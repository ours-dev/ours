/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_TYPES_HPP
#define OURS_IRQ_TYPES_HPP 1

#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours::irq {
inline constexpr auto INVALID_V_IRQ_NUM = static_cast<VIrqNum>(-1);

enum class IrqReturn : u8 {
    None,
    Handled,
};

enum class IrqFlags : u32 {
    None,
    Shared = 0_bit,
    Permanent = 1_bit,
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
