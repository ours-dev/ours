/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_BLOCK_OURS_BLOCK_HPP
#define OURS_BLOCK_OURS_BLOCK_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>

#include <span>
#include <string_view>

#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// ours.block, the protocol the block device driver provides, written by hand
/// on <wire/wire.hpp> as the ours.* codecs are (component-framework design
/// D20).
///
/// A block device, wrapped: the driver that drove the hardware registers it
/// in its outgoing directory under BLOCK_NAME, and a client reaches it
/// through a route to that name. What the protocol promises is the one thing
/// a disk is: blocks of a fixed size, named from the start, read by number.
///
///   Block.GetInfo     two-way  (nothing)
///   Block.Read        two-way  u64 lba@16, u32 count@24
///
/// and their Ok replies, the status at 16 as in every reply:
///
///   Block.GetInfo     u32 block_size@20, u64 blocks@24
///   Block.Read        u8 data[]@20, count * block_size bytes
///
/// GetInfo's block_size is 512 and blocks is the device's capacity in those
/// blocks. Read reads |count| whole blocks starting at block |lba|; a count
/// above MAX_READ_SECTORS is refused by the encoder (InvArgs), since one
/// reply must fit a channel message. A failure is written {.status = ...}
/// and carries no data.
///
/// Block is a closed protocol: a message with any other ordinal closes the
/// connection, as does one its decoder refuses (InvArgs), without an epitaph.
namespace ours_block {
/// The name a component finds the protocol under in its /svc, and the one a
/// provider serves it under in its outgoing directory's svc/ (@discoverable).
/// A route may rename it on the way (`as`), so a client does not always find
/// it here.
inline constexpr auto BLOCK_NAME = std::string_view{"ours.block.Block"};

/// The size of a block, in bytes. The devices this protocol reaches so far
/// are virtio-blk's, whose block is a 512-byte sector.
inline constexpr auto BLOCK_SIZE = u32{512};

/// The most blocks one Read carries: the data, behind the reply's status,
/// must fit a channel message.
inline constexpr auto MAX_READ_BLOCKS = usize{7};

// The methods, each declared beside the selector its ordinal is hashed from.
// wire/test/ordinals_test.py recomputes the ordinals from the selectors.

/// Answers with the device's geometry (Block.GetInfo).
inline constexpr auto GET_INFO = wire::Method{
    .selector = "ours.block/Block.GetInfo",
    .ordinal = 0x3605'3f7a'5455'1371,
    .kind = wire::MethodKind::TwoWay,
};

/// Answers Ok and ends the connection (ours.io/Node.Close): the client's
/// fsio::close sends it, and a server that closes without answering leaves
/// the client's close reading PeerClosed.
inline constexpr auto CLOSE = wire::Method{
    .selector = "ours.io/Node.Close",
    .ordinal = 0x67bf'e5a2'cd64'acda,
    .kind = wire::MethodKind::TwoWay,
};

/// Reads |count| blocks starting at |lba| (Block.Read).
inline constexpr auto READ = wire::Method{
    .selector = "ours.block/Block.Read",
    .ordinal = 0x4694'd3e4'398f'ba3d,
    .kind = wire::MethodKind::TwoWay,
};

struct CloseRequest {
    u32 txid;
};

struct CloseReply {
    ours::Status status;
};

struct GetInfoRequest {
    u32 txid;
};

struct GetInfoReply {
    ours::Status status;
    u32 block_size = 0;
    u64 blocks = 0;
};

struct ReadRequest {
    u32 txid;
    /// The first block to read, numbered from the start of the device.
    u64 lba = 0;
    /// How many blocks, at most MAX_READ_SECTORS.
    u32 count = 0;
};

/// The reply. |data| views the message's bytes and is there only when
/// |status| is Ok, so a failure is written {.status = ...}.
struct ReadReply {
    ours::Status status;
    std::span<u8 const> data = {};
};

/// Each request, encoded by a client into |bytes| and decoded by the server.
/// The encoders refuse a txid outside 1 to MAX_TXID and a count above
/// MAX_READ_SECTORS (InvArgs), and a buffer too short (BufferTooSmall). The
/// decoders refuse a malformed request with InvArgs.
[[nodiscard]]
auto encode_close(std::span<u8> bytes, CloseRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_close(wire::Message message) -> ustl::Result<CloseRequest, ours::Status>;

[[nodiscard]]
auto encode_close_reply(std::span<u8> bytes, u32 txid, CloseReply const &reply)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto encode_get_info(std::span<u8> bytes, GetInfoRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_get_info(wire::Message message) -> ustl::Result<GetInfoRequest, ours::Status>;

[[nodiscard]]
auto encode_read(std::span<u8> bytes, ReadRequest const &request)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_read(wire::Message message) -> ustl::Result<ReadRequest, ours::Status>;

/// The replies to the request |txid|, encoded by the server and decoded by
/// the client. GetInfo's and Read's encoders refuse data next to a failure
/// (InvArgs). A decoder's Err means the reply is malformed; the server's own
/// failure is in the reply's status.
[[nodiscard]]
auto encode_get_info_reply(std::span<u8> bytes, u32 txid, GetInfoReply const &reply)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_get_info_reply(wire::Message message, u32 txid)
    -> ustl::Result<GetInfoReply, ours::Status>;

[[nodiscard]]
auto encode_read_reply(std::span<u8> bytes, u32 txid, ReadReply const &reply)
    -> ustl::Result<wire::Message, ours::Status>;

[[nodiscard]]
auto decode_read_reply(wire::Message message, u32 txid) -> ustl::Result<ReadReply, ours::Status>;
} // namespace ours_block

#endif // #ifndef OURS_BLOCK_OURS_BLOCK_HPP
