/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROT_NVME_CONTROL_HPP
#define PROT_NVME_CONTROL_HPP 1

#include <span>
#include <string_view>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// ours.nvme.Control, the NVMe driver's control protocol (the PCI hot-plug
/// round): what a program calls to make a resident driver look at the bus
/// again. The driver boots even where its class-01h/08h function is absent,
/// and answers this instead of leaving the machine; a function that has
/// appeared since (hot-plug) is sized, assigned, enabled and published by
/// the rescan, and the reply says whether one was found.
///
///   Control.Rescan   two-way  (nothing)
///
/// and its reply, the bare status form, the status at 16 as in every reply:
///
///   Control.Rescan   u32 status@16
///
/// Ok when the rescan found and brought up a function; NotFound when the
/// walk saw none it owns -- an answer, not a failure, since a rescan of an
/// unchanged machine is legitimate. The method carries no payload: which
/// function to claim is the driver's decision, made on the bus, not the
/// caller's.
///
/// The protocol is closed: a message with any other ordinal closes the
/// connection, as does one the decoder refuses (InvArgs), without an
/// epitaph.
namespace prot::nvme {

/// The name a program finds the driver's control under in its /svc, and the
/// path a program's fsio::connect names it by.
inline constexpr auto CONTROL_NAME = std::string_view{"ours.nvme.Control"};
inline constexpr auto CONTROL_PATH = std::string_view{"/svc/ours.nvme.Control"};

/// The method: two-way, so the caller learns whether the rescan claimed
/// anything.
inline constexpr auto CONTROL_RESCAN =
    wire::method("ours.nvme/Control.Rescan", wire::MethodKind::TwoWay);

/// The rescan. Empty by contract, kept a named type as every request here
/// is, so a payload added later moves the decoders, not the callers.
struct RescanRequest {
    u32 txid;
};

/// The reply. NotFound names an unchanged machine, Ok a function claimed.
struct RescanReply {
    ours::Status status;
};

/// Encoded by a client into |bytes| and decoded by the driver. The encoder
/// refuses a txid outside 1 to MAX_TXID (InvArgs) and a buffer too short
/// (BufferTooSmall); the decoder refuses a malformed request -- another
/// method's ordinal, a payload byte, a handle -- with InvArgs.
[[nodiscard]]
auto encode_rescan(std::span<u8> bytes, RescanRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_rescan(wire::Message message) -> ustl::Result<RescanRequest, ours::Status>;

/// The driver's reply: the bare status form, whatever the status.
[[nodiscard]]
auto encode_rescan_reply(std::span<u8> bytes, u32 txid, RescanReply const &reply)
    -> ustl::Result<wire::Message, ours::Status>;

/// A decoder's Err means the reply is malformed or answers another request;
/// the rescan's own answer is in the reply's |status|.
[[nodiscard]]
auto decode_rescan_reply(wire::Message message, u32 txid)
    -> ustl::Result<RescanReply, ours::Status>;
} // namespace prot::nvme

#endif // #ifndef PROT_NVME_CONTROL_HPP
