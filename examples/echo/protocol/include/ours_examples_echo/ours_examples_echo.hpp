/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_EXAMPLES_ECHO_OURS_EXAMPLES_ECHO_HPP
#define OURS_EXAMPLES_ECHO_OURS_EXAMPLES_ECHO_HPP 1

#include <span>
#include <string_view>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// ours.examples.echo, the protocol //examples/echo provides, written by hand
/// on <wire/wire.hpp> (component-framework design D20; PLAN step 17).
///
/// EchoString answers with the string it was given: a client that gets its
/// string back through an offer from a sibling has proved that route, the one
/// this protocol exists to put on the target. Stop asks the provider to exit:
/// the routing checks send it so that they can see the next route to it answer
/// NotFound (design D23). A client, not the parent, stops the provider because
/// a parent has no channel here through which to stop a child, and the client
/// that checks the route is the one that has to know when the provider is
/// gone.
///
/// The bodies are flat, at fixed offsets from the start of the message:
///
///   Echo.EchoString   two-way  u32 size@16, the string@20
///   Echo.Stop         two-way  (nothing)
///
/// and their Ok replies, the status at 16 as in every reply:
///
///   Echo.EchoString   u32 size@20, the string@24
///   Echo.Stop         the status form alone
///
/// The string is never absent: it is always there, possibly empty, since a
/// flat body has no absent marker.
///
/// Echo is a closed protocol: a message with any other ordinal closes the
/// connection, as does one its decoder refuses (InvArgs), without an epitaph.
namespace ours_examples_echo {
/// The name a component finds the protocol under in its /svc, and the one a
/// provider serves it under in its outgoing directory's svc/
/// (@discoverable). A route may rename it on the way (`as`), so a client
/// does not always find it here.
inline constexpr auto ECHO_NAME = std::string_view{"ours.examples.echo.Echo"};

/// The longest string EchoString carries.
inline constexpr auto MAX_STRING_LENGTH = usize{64};

// The methods, each declared beside the selector its ordinal is hashed
// from. wire/test/ordinals_test.py recomputes the ordinals from the
// selectors.

/// Answers with the string it was given (Echo.EchoString).
inline constexpr auto ECHO_STRING = wire::Method{
    .selector = "ours.examples.echo/Echo.EchoString",
    .ordinal = 0x6e80'6bd7'7638'6789,
    .kind = wire::MethodKind::TwoWay,
};

/// Answers Ok, then the provider exits with code 0 (Echo.Stop).
inline constexpr auto STOP = wire::Method{
    .selector = "ours.examples.echo/Echo.Stop",
    .ordinal = 0x1156'5ac0'dbe3'72a7,
    .kind = wire::MethodKind::TwoWay,
};

struct EchoStringRequest {
    u32 txid;
    /// At most MAX_STRING_LENGTH bytes. Decoded, it views the message's
    /// bytes.
    std::string_view value;
};

/// The reply. |response| is there only when |status| is Ok, so a failure
/// is written {.status = ...}.
struct EchoStringReply {
    ours::Status status;
    /// At most MAX_STRING_LENGTH bytes. Decoded, it views the message's
    /// bytes.
    std::string_view response = {};
};

struct StopRequest {
    u32 txid;
};

/// Each request, encoded by a client into |bytes| and decoded by the server.
/// The encoders refuse a txid outside 1 to MAX_TXID and a string longer than
/// MAX_STRING_LENGTH (InvArgs), and a buffer too short (BufferTooSmall). The
/// decoders refuse a malformed request with InvArgs.
[[nodiscard]]
auto encode_echo_string(std::span<u8> bytes, EchoStringRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_echo_string(wire::Message message) -> ustl::Result<EchoStringRequest, ours::Status>;

[[nodiscard]]
auto encode_stop(std::span<u8> bytes, StopRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_stop(wire::Message message) -> ustl::Result<StopRequest, ours::Status>;

/// The replies to the request |txid|, encoded by the server and decoded by
/// the client. EchoString's encoder refuses a response next to a failure and
/// one longer than MAX_STRING_LENGTH (InvArgs). Stop's reply is the status
/// form, whatever its status. A decoder's Err means the reply is malformed;
/// the server's own failure is in the reply's status.
[[nodiscard]]
auto encode_echo_string_reply(std::span<u8> bytes, u32 txid, EchoStringReply const &reply)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_echo_string_reply(wire::Message message, u32 txid)
    -> ustl::Result<EchoStringReply, ours::Status>;

[[nodiscard]]
auto encode_stop_reply(std::span<u8> bytes, u32 txid, ours::Status status)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_stop_reply(wire::Message message, u32 txid) -> ustl::Result<ours::Status, ours::Status>;
} // namespace ours_examples_echo

#endif // #ifndef OURS_EXAMPLES_ECHO_OURS_EXAMPLES_ECHO_HPP
