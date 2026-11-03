/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_CONTROLLER_HPP
#define OURS_IRQ_CONTROLLER_HPP 1

#include <ours/irq/types.hpp>
#include <ours/status.hpp>

namespace ours::irq {
struct ControllerOps;

/// Immutable routing identity and instance context, bound before publication.
struct ControllerSource final {
    constexpr ControllerSource(HardwareIrq const source_id, IrqId const irq_id,
                               ControllerOps const *const controller = nullptr,
                               void *const context = nullptr) noexcept
        : source(source_id), id(irq_id), controller(controller), context(context) {}

    HardwareIrq source;
    IrqId id;
    ControllerOps const *controller;
    void *context;
};

/// Handshake for a known source on the receiving CPU, independent of whether
/// its electrical signal is Edge or Level. Claim reads that identify a source
/// belong to the controller entry before endpoint dispatch.
enum class DispatchProtocol : u8 {
    Invalid,
    Eoi,
    AckEoi,
};

/// Whether mask() has stopped delivery before returning. Unspecified remains
/// usable for ordinary registrations, but cannot promise a deferred hold.
enum class Masking : u8 {
    Unspecified,
    Synchronous,
};

/// A controller instance's immutable operations. Context and operations live
/// until unregistration and all controller entry calls complete. Writes and
/// dispatch hooks run under L1 with interrupts off; dispatch also disables
/// preemption. Read-only queries may run without L1. Controller register
/// locks are leaf locks inside L1. No operation sleeps or calls line control.
/// Each line has its own L1, so shared registers need controller-local locking.
struct ControllerOps final {
    DispatchProtocol protocol = DispatchProtocol::Invalid;
    Masking masking = Masking::Unspecified;
    auto (*mask)(ControllerSource const &) -> void = nullptr;
    auto (*unmask)(ControllerSource const &) -> void = nullptr;
    /// On success leave the source masked; on failure leave it unchanged.
    auto (*configure)(ControllerSource const &, TriggerMode, Polarity) -> Status = nullptr;
    /// Null means the owner must supply its mode at attachment.
    auto (*trigger_mode)(ControllerSource const &) -> TriggerMode = nullptr;
    /// Diagnostic hardware read; the generic layer keeps no mask-bit copy.
    auto (*is_masked)(ControllerSource const &) -> bool = nullptr;
    /// Exactly once after callbacks, including unclaimed or masked sources,
    /// on the CPU that received the interrupt. Never deferred to a thread.
    auto (*issue_eoi)(ControllerSource const &) -> void = nullptr;
    /// AckEoi only: once before callbacks, including an unclaimed source.
    /// It acknowledges this known source without a returned claim token.
    auto (*issue_ack)(ControllerSource const &) -> void = nullptr;

    /// Reject unused or missing handshake hooks before any source is bound.
    [[nodiscard]]
    constexpr auto is_valid() const -> bool {
        if (!mask || !issue_eoi ||
            (masking != Masking::Unspecified && masking != Masking::Synchronous)) {
            return false;
        }
        switch (protocol) {
            case DispatchProtocol::Eoi:
                return issue_ack == nullptr;
            case DispatchProtocol::AckEoi:
                return issue_ack != nullptr;
            default:
                return false;
        }
    }
};
} // namespace ours::irq

#endif // OURS_IRQ_CONTROLLER_HPP
