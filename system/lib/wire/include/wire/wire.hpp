/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef WIRE_WIRE_HPP
#define WIRE_WIRE_HPP 1

#include <array>
#include <bit>
#include <span>
#include <string_view>
#include <type_traits>

#include <cstring>

#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>

/// The framing every hand-written protocol codec here shares (component-framework
/// design D20, docs/design/260924/userspace-filesystem.md §2.1).
///
/// A message is a 16-byte header shaped like Zircon's fidl_message_header_t,
/// then a flat body: fields at fixed offsets from 16, no implicit padding,
/// and at most one trailing byte array, whose length is the u32 field just
/// before it. FIDL's out-of-line objects, tables and envelopes are left out:
/// every message of the subset fits this shape. The protocol names and the
/// ordinals are Fuchsia's, so a reader can look each method up in the FIDL
/// files.
///
/// Three forms are fixed here, the same for every protocol:
///
///   - a request: the header, direction ToServer, and the method's body;
///   - a reply: the header with the request's txid and ordinal, direction
///     ToClient, then a u32 status at 16. Ok is followed by the method's
///     fields; anything else is the 24-byte status form below, no handles;
///   - an epitaph: the 24-byte status form with txid 0 and EPITAPH_ORDINAL,
///     the last message a server writes before it closes the channel.
///
/// The status form is {header, u32 status, u32 0}: 24 bytes, the size of
/// Zircon's fidl_epitaph_t.
///
/// Nothing here touches a channel. A codec turns bytes and handle values into
/// structs and back, so it runs on the host, where its tests are; a program
/// hands what an encoder returns to channel_write.
namespace wire {
/// Which way a message travels.
///
/// Not in fidl_message_header_t, whose byte 4 is the first at-rest flag; the
/// direction takes its place. It is what keeps a reply from ever reading as a
/// request (design invariant I5): a File.Read reply and a File.Read request
/// can both be 24 bytes, and without this byte a client that handed a server
/// both ends of one channel could make the server answer its own answers for
/// ever. A server decodes only ToServer and a client only ToClient.
enum class Direction : u8 {
    ToServer = 0,
    ToClient = 1,
};

/// Header::magic. Zircon's kFidlWireFormatMagicNumberInitial is 1 too.
inline constexpr auto MAGIC = u8{1};

/// What a handle field holds in the bytes. The handle itself travels in the
/// message's handle array, in the order of the handle fields (Zircon
/// FIDL_HANDLE_PRESENT). Every handle field of the subset is required, so
/// there is no absent value.
inline constexpr auto HANDLE_PRESENT = u32{0xFFFF'FFFF};

/// The ordinal of an epitaph (Zircon kFidlOrdinalEpitaph). Method ordinals
/// have their top bit clear, so none can be mistaken for it.
inline constexpr auto EPITAPH_ORDINAL = u64{0xFFFF'FFFF'FFFF'FFFF};

/// The largest txid a client may give a two-way request. The top bit is left
/// alone, as Zircon leaves it for zx_channel_call's own txids.
inline constexpr auto MAX_TXID = u32{0x7FFF'FFFF};

/// What one channel message may carry (<ours/syscall.hpp>).
inline constexpr auto MAX_MESSAGE_BYTES = usize{ours::CHANNEL_MAX_MSG_BYTES};
inline constexpr auto MAX_MESSAGE_HANDLES = usize{ours::CHANNEL_MAX_MSG_HANDLES};

/// The first 16 bytes of every message.
struct Header {
    /// 0 for a one-way request and an epitaph. A two-way request carries one
    /// from 1 to MAX_TXID, and its reply carries the same.
    u32 txid;
    Direction direction;
    /// Zero.
    std::array<u8, 2> reserved;
    u8 magic;
    u64 ordinal;
};
static_assert(sizeof(Header) == 16);
static_assert(std::is_trivially_copyable_v<Header>);

inline constexpr auto HEADER_SIZE = sizeof(Header);

/// The size of the status form: the header, a u32 status and a zero u32.
inline constexpr auto STATUS_FORM_SIZE = usize{24};

/// The offset of a reply's status, and of an epitaph's.
inline constexpr auto STATUS_OFFSET = usize{16};

// The bodies are written in the machine's order, as fidl's are little-endian
// and every target here is too.
static_assert(std::endian::native == std::endian::little);

/// Whether a method has a reply.
enum class MethodKind : u8 {
    OneWay,
    TwoWay,
};

/// One method of a protocol, as its codec declares it: the selector its
/// ordinal is hashed from, the ordinal, and its kind.
///
/// The ordinal is fidlc's: the first 8 bytes of SHA-256(selector), read
/// little-endian, top bit cleared (tools/fidl/fidlc/src/compiler.cc,
/// Sha256MethodHasher). The tree has no SHA-256 in C++, so test/ordinals_test.py
/// reads every selector and ordinal declared with this type out of the codec
/// headers and recomputes each with Python's hashlib. Write a declaration as
/// one designated initializer, .selector first and .ordinal right after it,
/// for that test to find it.
struct Method {
    std::string_view selector;
    u64 ordinal;
    MethodKind kind;
};

/// A message as it arrived from a channel, or as an encoder left it: its bytes
/// and its handles, the handles in the order of the handle fields. Neither is
/// owned.
struct Message {
    std::span<u8 const> bytes;
    std::span<ours::Uandle const> handles;
};

/// Copies a T out of |bytes| at |offset|. The caller has checked that it lies
/// inside. memcpy, because nothing promises that a channel buffer is aligned
/// for T.
template <typename T>
    requires std::is_trivially_copyable_v<T>
[[nodiscard]]
inline auto load(std::span<u8 const> const bytes, usize const offset) -> T {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

/// Copies |value| into |bytes| at |offset|. The caller has checked that it
/// lies inside.
template <typename T>
    requires std::is_trivially_copyable_v<T>
inline auto store(std::span<u8> const bytes, usize const offset, T const &value) -> void {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

/// Reads the header of |bytes| and checks what every message must have,
/// whichever protocol it belongs to:
///
///   - between HEADER_SIZE and MAX_MESSAGE_BYTES bytes;
///   - MAGIC, and zero reserved bytes;
///   - |direction|.
///
/// The ordinal and the txid are left to the caller, which knows which methods
/// exist. Every refusal is InvArgs.
[[nodiscard]]
auto decode_header(std::span<u8 const> bytes, Direction direction)
    -> ustl::Result<Header, ours::Status>;

/// Checks |message| as a request for |method|: decode_header() with ToServer,
/// the method's ordinal, and its txid rule, 0 for a one-way method and 1 to
/// MAX_TXID for a two-way one. Returns the header; a reply carries its txid.
/// Every refusal is InvArgs. The body is the method's codec's to check.
[[nodiscard]]
auto decode_request_header(Message message, Method const &method)
    -> ustl::Result<Header, ours::Status>;

/// Checks |message| as the reply to the request |txid| of the two-way
/// |method|, as far as every reply goes, and returns the status it carries:
///
///   - decode_header() with ToClient, the method's ordinal, and |txid|;
///   - a status at STATUS_OFFSET that ours::Status names;
///   - for any status but Ok, the status form: 24 bytes, zero in the last
///     four, and no handles.
///
/// Ok carries the server's status, which may itself be a failure; Err
/// (InvArgs) means the reply is malformed. The rest of an Ok reply is the
/// method's codec's to check.
[[nodiscard]]
auto decode_reply_status(Message message, Method const &method, u32 txid)
    -> ustl::Result<ours::Status, ours::Status>;

/// Starts a message of |size| bytes at the front of |out|: zeroes those bytes,
/// so that nothing of an earlier message survives in a reused buffer, and
/// writes |header| with MAGIC and zero reserved bytes. Returns
/// out.first(size). InvArgs when |size| is below HEADER_SIZE or above
/// MAX_MESSAGE_BYTES; BufferTooSmall when |out| is shorter than |size|.
[[nodiscard]]
auto start_message(std::span<u8> out, usize size, u32 txid, Direction direction, u64 ordinal)
    -> ustl::Result<std::span<u8>, ours::Status>;

/// start_message() for a request of |method|, refusing a txid that breaks
/// the method's rule (InvArgs).
[[nodiscard]]
auto start_request(std::span<u8> out, usize size, Method const &method, u32 txid)
    -> ustl::Result<std::span<u8>, ours::Status>;

/// start_message() for the Ok reply to the request |txid| of the two-way
/// |method|, with Ok written at STATUS_OFFSET. InvArgs for a one-way method,
/// a txid outside 1 to MAX_TXID, or a size below STATUS_FORM_SIZE.
[[nodiscard]]
auto start_ok_reply(std::span<u8> out, usize size, Method const &method, u32 txid)
    -> ustl::Result<std::span<u8>, ours::Status>;

/// The status form of a reply to the request |txid| of the two-way |method|:
/// every reply that is not Ok, and the Ok reply of a method that returns
/// nothing. InvArgs for a one-way method, a txid outside 1 to MAX_TXID, or a
/// status ours::Status does not name; BufferTooSmall when |out| is shorter
/// than STATUS_FORM_SIZE.
[[nodiscard]]
auto encode_status_reply(std::span<u8> out, Method const &method, u32 txid, ours::Status status)
    -> ustl::Result<Message, ours::Status>;

/// An epitaph carrying |status|. Ok is allowed: it says the server closed the
/// connection with nothing wrong, as in FIDL. InvArgs for a status
/// ours::Status does not name; BufferTooSmall when |out| is shorter than
/// STATUS_FORM_SIZE.
[[nodiscard]]
auto encode_epitaph(std::span<u8> out, ours::Status status) -> ustl::Result<Message, ours::Status>;

/// The status an epitaph carries. Err (InvArgs) when |message| is not an
/// epitaph in the status form: txid 0, EPITAPH_ORDINAL, ToClient, 24 bytes,
/// zero in the last four, no handles, and a status ours::Status names.
[[nodiscard]]
auto decode_epitaph(Message message) -> ustl::Result<ours::Status, ours::Status>;

/// The handle of the field at |offset|, the |index|th handle field of the
/// message: the field must hold HANDLE_PRESENT and handle |index| must exist
/// and be valid. The caller has checked that the field lies inside the bytes
/// and that the message carries as many handles as it has handle fields.
/// InvArgs on every refusal.
[[nodiscard]]
auto decode_handle(Message message, usize offset, usize index)
    -> ustl::Result<ours::Uandle, ours::Status>;

/// Writes HANDLE_PRESENT into the field at |offset| of |bytes| and |handle|
/// into |handles| at |index|. The caller has checked that the field lies
/// inside the bytes. InvArgs for Uandle::Invalid, since every handle field is
/// required; BufferTooSmall when |handles| has no |index|.
[[nodiscard]]
auto encode_handle(std::span<u8> bytes, usize offset, std::span<ours::Uandle> handles, usize index,
                   ours::Uandle handle) -> ours::Status;
} // namespace wire

#endif // #ifndef WIRE_WIRE_HPP
