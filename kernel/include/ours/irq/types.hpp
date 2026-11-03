/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_TYPES_HPP
#define OURS_IRQ_TYPES_HPP 1

#include <ours/types.hpp>

#include <ustl/util/enum_bits.hpp>

namespace ours::irq {
/// Names an interrupt domain: a controller's own table of lines. A number the
/// domain's registrant chooses, by the rule of the platform that has more
/// than one: the root controller is Root on every platform, and the kernel
/// only refuses a duplicate. Not a name that outlives the domain: the id of a
/// removed domain may be registered again, for a controller that is not the
/// one it named, and only a virq is never reused. So a (domain, hardware
/// number) pair is a scoped name, resolved to the virq that is the line's
/// canonical one, and a program asks for the pair and does not cache it.
enum class DomainId : u32 {
    Root = 0,
    Invalid = 0xFFFF'FFFF,
};

/// What a handler answers for one interrupt on its line.
enum class IrqReturn : u8 {
    /// The interrupt was not this handler's device's. A line whose registrations
    /// all answer None counts the interrupt as spurious.
    None,
    /// The device raised it, and the handler is done with it.
    Handled,
    /// The device raised it, and the handler has handed the rest of the work
    /// to a thread, which it has woken. Counts as handled. On a Oneshot line
    /// it also leaves the line held: masked until the registration's
    /// irq::finalize_oneshot() says the thread is done.
    WakeThread,
};

enum class RegistrationOptions : u32 {
    None,
    Shared = 0_bit,
    Permanent = 1_bit,
    /// Asks for the line to stay masked from an interrupt the registration's
    /// handler answers WakeThread for until the registration calls
    /// irq::finalize_oneshot(). On a level line that is what keeps a device
    /// still asserting its pin from raising the line again before the thread
    /// has serviced it; an edge line raises nothing until the device signals
    /// anew, so its flow ignores the flag. Refused together with Permanent
    /// or Shared.
    Oneshot = 2_bit,
};
USTL_ENABLE_ENUM_BITMASK(RegistrationOptions);

/// Whether a dispatch of a line ends the interrupt at the line's controller,
/// through its controller, once the registrations have run.
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
/// the device is serviced. A controller's own encoding stays in its controller,
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

/// An interrupt handler. `context` is whatever the registration that carries the
/// handler was constructed with (Registration), so the handler finds its
/// device's state there rather than in a global.
using IrqHandler = auto (*)(IrqId id, void *context) -> IrqReturn;

/// Revalidates deferred work while the line holds L1. The owner checks its
/// current pending work under its handler lock, preserving the handler's lock
/// order. The callback cannot sleep or call line control operations.
using IrqCompletionCheck = auto (*)(void *context) -> bool;

/// What an registration may ask to be told when the kernel takes its line away
/// from it (Line::revoke()), instead of its owner letting the line go: the
/// line's virq and the registration's context, as the handler gets them.
///
/// Called with the line's lock (L1) held, interrupts off and preemption off,
/// once the registration is off the line and before the registration's line is cleared.
/// So it runs where a handler runs: it may wake a thread and take the locks a
/// handler takes after L1, and must not call into the irq layer, which would
/// take L1 again. Its owner may free the registration and its context only after
/// irq::detach() has returned, which waits for this call to finish.
using IrqRevocationHandler = auto (*)(IrqId id, void *context) -> void;
} // namespace ours::irq

#endif // OURS_IRQ_TYPES_HPP
