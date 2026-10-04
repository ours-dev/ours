/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef FUCHSIA_KERNEL_FUCHSIA_KERNEL_HPP
#define FUCHSIA_KERNEL_FUCHSIA_KERNEL_HPP 1

#include <span>
#include <string_view>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// The part of fuchsia.kernel that ours speaks, written by hand on
/// <wire/wire.hpp> (component-framework design S6, D19, D20, "The protocol
/// subset"): DebugResource.Get, through which a component that is routed
/// the protocol gets the Debug resource. It is a built-in of the root, and
/// the only way to the resource: no component gets it as a startup handle.
///
/// The names and the ordinal are Fuchsia's (sdk/fidl/fuchsia.kernel/
/// debug-resource.fidl); the bodies are flat, at fixed offsets from the
/// start of the message:
///
///   DebugResource.Get   two-way  (nothing)
///
/// and its Ok reply, the status at 16 as in every reply:
///
///   DebugResource.Get   u32 resource@20 (handle)
///
/// Fuchsia's reply is the resource alone; ours puts a status in front of it,
/// as every reply here has one, so a root that cannot duplicate the
/// resource answers with the 24-byte status form rather than dropping the
/// connection.
///
/// DebugResource is a closed protocol: a message with any other ordinal
/// closes the connection, as does one its decoder refuses (InvArgs), without
/// an epitaph.
namespace fuchsia_kernel {
/// The name a component finds DebugResource under in its /svc
/// (@discoverable).
inline constexpr auto DEBUG_RESOURCE_NAME = std::string_view{"fuchsia.kernel.DebugResource"};

// The method, declared beside the selector its ordinal is hashed from.
// wire/test/ordinals_test.py recomputes the ordinal from the selector.

/// A handle to the Debug resource (DebugResource.Get).
inline constexpr auto DEBUG_RESOURCE_GET = wire::Method{
    .selector = "fuchsia.kernel/DebugResource.Get",
    .ordinal = 0x1d79'd77e'a12a'6474,
    .kind = wire::MethodKind::TwoWay,
};

struct DebugResourceGetRequest {
    u32 txid;
};

/// The reply. |resource| is there when, and only when, |status| is Ok, so a
/// failure is written {.status = ...}.
struct DebugResourceGetReply {
    ours::Status status;
    ours::Uandle resource = ours::Uandle::Invalid;
};

/// The request, encoded by a client into |bytes| and decoded by the server.
/// The encoder refuses a txid outside 1 to MAX_TXID (InvArgs) and a buffer
/// too short (BufferTooSmall); the decoder refuses a malformed request with
/// InvArgs.
[[nodiscard]]
auto encode_debug_resource_get(std::span<u8> bytes, DebugResourceGetRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_debug_resource_get(wire::Message message)
    -> ustl::Result<DebugResourceGetRequest, ours::Status>;

/// The reply to the request |txid|, encoded by the server and decoded by the
/// client. A reply whose status is not Ok is the status form: the encoder
/// refuses a resource next to a failure (InvArgs). A decoder's Err means the
/// reply is malformed; the server's own failure is in the reply's |status|.
[[nodiscard]]
auto encode_debug_resource_get_reply(std::span<u8> bytes, std::span<ours::Uandle> handles, u32 txid,
                                     DebugResourceGetReply const &reply)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_debug_resource_get_reply(wire::Message message, u32 txid)
    -> ustl::Result<DebugResourceGetReply, ours::Status>;
} // namespace fuchsia_kernel

#endif // #ifndef FUCHSIA_KERNEL_FUCHSIA_KERNEL_HPP
