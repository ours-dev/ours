/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The ours.dev codec: Publish's round trip, and the refusals -- strings that
// do not fit, that name a path, that carry a NUL, a missing handle, a body
// one byte short or long.
#include <prot/dev.hpp>

#include <array>
#include <span>
#include <string>

#include <gtest/gtest.h>
#include <ours/status.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;

constexpr auto TXID = u32{7};
constexpr auto DIRECTORY = Uandle{0xDEAD'0000};

class Buffer {
  public:
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> handles{};

    [[nodiscard]]
    auto view(usize const size, usize const handle_count = 1) -> wire::Message {
        return {.bytes = std::span{bytes}.first(size),
                .handles = std::span{handles}.first(handle_count)};
    }
};

} // namespace

TEST(OursDevPublish, RoundTrip) {
    auto buffer = Buffer{};
    auto const request = prot::dev::encode_publish(buffer.bytes, buffer.handles,
                                                   {.txid = TXID,
                                                    .kind = "block",
                                                    .name = "000",
                                                    .protocol = "ours.block.Block",
                                                    .directory = DIRECTORY});
    ASSERT_TRUE(request.is_ok());
    auto const message = buffer.view(request.unwrap().bytes.size());

    auto const decoded = prot::dev::decode_publish(message);
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->txid, TXID);
    EXPECT_EQ(decoded->kind, "block");
    EXPECT_EQ(decoded->name, "000");
    EXPECT_EQ(decoded->directory, DIRECTORY);

    auto const reply =
        prot::dev::encode_publish_reply(buffer.bytes, TXID, {.status = Status::Ok, .name = "000"});
    ASSERT_TRUE(reply.is_ok());
    auto const answered =
        prot::dev::decode_publish_reply(buffer.view(reply.unwrap().bytes.size(), 0), TXID);
    ASSERT_TRUE(answered.is_ok());
    EXPECT_EQ(answered->status, Status::Ok);
    EXPECT_EQ(answered->name, "000");
    // A failure is the bare status form, no name in it.
    auto const refused = prot::dev::encode_publish_reply(
        buffer.bytes, TXID, {.status = Status::NoResource, .name = {}});
    ASSERT_TRUE(refused.is_ok());
    auto const refused_reply =
        prot::dev::decode_publish_reply(buffer.view(refused.unwrap().bytes.size(), 0), TXID);
    ASSERT_TRUE(refused_reply.is_ok());
    EXPECT_EQ(refused_reply->status, Status::NoResource);
    EXPECT_TRUE(refused_reply->name.empty());
}

TEST(OursDevPublish, StringsViewTheMessage) {
    auto buffer = Buffer{};
    auto const request = prot::dev::encode_publish(buffer.bytes, buffer.handles,
                                                   {.txid = TXID,
                                                    .kind = "block",
                                                    .name = "001",
                                                    .protocol = "ours.block.Block",
                                                    .directory = DIRECTORY});
    ASSERT_TRUE(request.is_ok());
    // The two strings lie back to back, class first: the byte order the
    // decoder's arithmetic depends on.
    EXPECT_EQ(request.unwrap().bytes.size(), usize{36 + 5 + 3 + 16});
    auto const bytes = request.unwrap().bytes;
    EXPECT_EQ((std::string_view{reinterpret_cast<char const *>(bytes.data()) + 36, 5}), "block");
    EXPECT_EQ((std::string_view{reinterpret_cast<char const *>(bytes.data()) + 41, 3}), "001");
    EXPECT_EQ((std::string_view{reinterpret_cast<char const *>(bytes.data()) + 44, 16}),
              "ours.block.Block");
}

TEST(OursDevPublish, Refusals) {
    auto buffer = Buffer{};
    auto handles = std::array<Uandle, wire::MAX_MESSAGE_HANDLES>{};

    // A path in either field: the registry mounts at class/<class>/<name>,
    // so a separator would name a location, not a node.
    EXPECT_EQ((prot::dev::encode_publish(buffer.bytes, handles,
                                         {.txid = TXID,
                                          .kind = "block/../svc",
                                          .name = "000",
                                          .protocol = "ours.block.Block",
                                          .directory = DIRECTORY})
                   .unwrap_err()),
              Status::InvArgs);
    EXPECT_EQ((prot::dev::encode_publish(buffer.bytes, handles,
                                         {.txid = TXID,
                                          .kind = "block",
                                          .name = "0/0",
                                          .protocol = "ours.block.Block",
                                          .directory = DIRECTORY})
                   .unwrap_err()),
              Status::InvArgs);
    // A NUL is a path's end, same refusal.
    EXPECT_EQ((prot::dev::encode_publish(buffer.bytes, handles,
                                         {.txid = TXID,
                                          .kind = std::string_view{"blo\0k", 5},
                                          .name = "000",
                                          .protocol = "ours.block.Block",
                                          .directory = DIRECTORY})
                   .unwrap_err()),
              Status::InvArgs);
    // The bounds, each by one over.
    auto long_class = std::string(prot::dev::MAX_CLASS_NAME + 1, 'x');
    EXPECT_EQ((prot::dev::encode_publish(buffer.bytes, handles,
                                         {.txid = TXID,
                                          .kind = long_class,
                                          .name = "000",
                                          .protocol = "ours.block.Block",
                                          .directory = DIRECTORY})
                   .unwrap_err()),
              Status::InvArgs);
    auto long_name = std::string(prot::dev::MAX_NODE_NAME + 1, 'x');
    EXPECT_EQ((prot::dev::encode_publish(buffer.bytes, handles,
                                         {.txid = TXID,
                                          .kind = "block",
                                          .name = long_name,
                                          .protocol = "ours.block.Block",
                                          .directory = DIRECTORY})
                   .unwrap_err()),
              Status::InvArgs);
    // No handle, no publish.
    EXPECT_EQ((prot::dev::encode_publish(buffer.bytes, handles,
                                         {.txid = TXID,
                                          .kind = "block",
                                          .name = "000",
                                          .protocol = "ours.block.Block",
                                          .directory = Uandle::Invalid})
                   .unwrap_err()),
              Status::InvArgs);
    // An empty name decodes as nothing to mount.
    EXPECT_EQ((prot::dev::encode_publish(buffer.bytes, handles,
                                         {.txid = TXID,
                                          .kind = "block",
                                          .name = "",
                                          .protocol = "ours.block.Block",
                                          .directory = DIRECTORY})
                   .unwrap_err()),
              Status::InvArgs);
}

TEST(OursDevPublish, BoundsAcceptedExactly) {
    auto buffer = Buffer{};
    // Each string at its exact bound is a legal publish: the refusal is one
    // over, not at.
    auto full_class = std::string(prot::dev::MAX_CLASS_NAME, 'c');
    auto full_name = std::string(prot::dev::MAX_NODE_NAME, 'n');
    auto full_protocol = std::string(prot::dev::MAX_PROTOCOL_NAME, 'p');
    buffer.handles[0] = DIRECTORY;
    auto const request = prot::dev::encode_publish(buffer.bytes, buffer.handles,
                                                   {.txid = TXID,
                                                    .kind = full_class,
                                                    .name = full_name,
                                                    .protocol = full_protocol,
                                                    .directory = DIRECTORY});
    ASSERT_TRUE(request.is_ok());
    auto const decoded = prot::dev::decode_publish(buffer.view(request.unwrap().bytes.size()));
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->kind.size(), prot::dev::MAX_CLASS_NAME);
    EXPECT_EQ(decoded->name.size(), prot::dev::MAX_NODE_NAME);
    EXPECT_EQ(decoded->protocol.size(), prot::dev::MAX_PROTOCOL_NAME);
}

TEST(OursDevPublish, ThreeStringsBackToBack) {
    auto buffer = Buffer{};
    auto handles = std::array<Uandle, wire::MAX_MESSAGE_HANDLES>{};
    auto const request = prot::dev::encode_publish(
        buffer.bytes, handles,
        {.txid = TXID, .kind = "ab", .name = "cde", .protocol = "fghij", .directory = DIRECTORY});
    ASSERT_TRUE(request.is_ok());
    // class@36, name@38, protocol@41 -- the arithmetic the decoder repeats.
    auto const bytes = request.unwrap().bytes;
    EXPECT_EQ(request.unwrap().bytes.size(), usize{36 + 2 + 3 + 5});
    EXPECT_EQ((std::string_view{reinterpret_cast<char const *>(bytes.data()) + 36, 2}), "ab");
    EXPECT_EQ((std::string_view{reinterpret_cast<char const *>(bytes.data()) + 38, 3}), "cde");
    EXPECT_EQ((std::string_view{reinterpret_cast<char const *>(bytes.data()) + 41, 5}), "fghij");
}

TEST(OursDevPublish, SmallBufferRefused) {
    auto handles = std::array<Uandle, wire::MAX_MESSAGE_HANDLES>{};
    auto tiny = std::array<u8, 32>{};
    EXPECT_EQ((prot::dev::encode_publish(tiny, handles,
                                         {.txid = TXID,
                                          .kind = "block",
                                          .name = "000",
                                          .protocol = "ours.block.Block",
                                          .directory = DIRECTORY})
                   .unwrap_err()),
              Status::BufferTooSmall);
}

TEST(OursDevPublish, ReplyRefusals) {
    auto buffer = Buffer{};
    // An Ok reply carrying the granted name and a handle is not this
    // protocol's reply: the name rides alone.
    auto const reply =
        prot::dev::encode_publish_reply(buffer.bytes, TXID, {.status = Status::Ok, .name = "001"});
    ASSERT_TRUE(reply.is_ok());
    auto with_handle = wire::Message{.bytes = std::span{buffer.bytes}.first(reply->bytes.size()),
                                     .handles = std::span{buffer.handles}.first(1)};
    EXPECT_EQ(prot::dev::decode_publish_reply(with_handle, TXID).unwrap_err(), Status::InvArgs);
    // A granted name past the bound is refused by the encoder itself: the
    // registry cannot grant what the codec cannot say.
    auto long_name = std::string(prot::dev::MAX_NODE_NAME + 1, 'x');
    EXPECT_EQ((prot::dev::encode_publish_reply(buffer.bytes, TXID,
                                               {.status = Status::Ok, .name = long_name})
                   .unwrap_err()),
              Status::InvArgs);
    // An Ok reply naming nothing is malformed too: the registry always
    // grants a name.
    auto const empty = prot::dev::encode_publish_reply(
        buffer.bytes, TXID, {.status = Status::Ok, .name = std::string_view{}});
    EXPECT_TRUE(empty.is_err());
    // The wrong txid is not this conversation's reply.
    auto const right =
        prot::dev::encode_publish_reply(buffer.bytes, TXID, {.status = Status::Ok, .name = "000"});
    ASSERT_TRUE(right.is_ok());
    auto right_message =
        wire::Message{.bytes = std::span{buffer.bytes}.first(right->bytes.size()), .handles = {}};
    EXPECT_EQ(prot::dev::decode_publish_reply(right_message, TXID + 1).unwrap_err(),
              Status::InvArgs);
}

TEST(OursDevPublish, DecodeRefusals) {
    auto buffer = Buffer{};
    auto const request = prot::dev::encode_publish(buffer.bytes, buffer.handles,
                                                   {.txid = TXID,
                                                    .kind = "block",
                                                    .name = "000",
                                                    .protocol = "ours.block.Block",
                                                    .directory = DIRECTORY});
    ASSERT_TRUE(request.is_ok());

    // One byte short of the body: malformed.
    EXPECT_EQ(
        prot::dev::decode_publish(buffer.view(request.unwrap().bytes.size() - 1)).unwrap_err(),
        Status::InvArgs);
    // One byte over: the sizes and the length disagree.
    EXPECT_EQ(
        prot::dev::decode_publish(buffer.view(request.unwrap().bytes.size() + 1)).unwrap_err(),
        Status::InvArgs);
    // Without the handle: no directory to mount.
    auto const bare = wire::Message{
        .bytes = std::span{buffer.bytes}.first(request.unwrap().bytes.size()), .handles = {}};
    EXPECT_EQ(prot::dev::decode_publish(bare).unwrap_err(), Status::InvArgs);
}

/// The un-publish codec: the round trip, and the shape refusals -- the
/// same bound and separator checks a publish's strings carry.
TEST(OursDevUnpublish, RoundTrip) {
    auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
    auto const request =
        prot::dev::encode_unpublish(bytes, {.txid = TXID, .kind = "block", .name = "000"});
    ASSERT_TRUE(request.is_ok());
    auto const decoded = prot::dev::decode_unpublish(wire::Message{
        .bytes = std::span{bytes}.first(request.unwrap().bytes.size()), .handles = {}});
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->txid, TXID);
    EXPECT_EQ(decoded->kind, "block");
    EXPECT_EQ(decoded->name, "000");

    auto const reply = prot::dev::encode_unpublish_reply(bytes, TXID, {.status = Status::Ok});
    ASSERT_TRUE(reply.is_ok());
    auto const answered = prot::dev::decode_unpublish_reply(
        wire::Message{.bytes = std::span{bytes}.first(reply.unwrap().bytes.size()), .handles = {}},
        TXID);
    ASSERT_TRUE(answered.is_ok());
    EXPECT_EQ(answered->status, Status::Ok);
}

TEST(OursDevUnpublish, Refusals) {
    auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
    // The bounds, each by one over; a separator and a NUL in either field.
    auto long_kind = std::string(prot::dev::MAX_CLASS_NAME + 1, 'k');
    auto long_name = std::string(prot::dev::MAX_NODE_NAME + 1, 'n');
    EXPECT_EQ((prot::dev::encode_unpublish(bytes, {.txid = TXID, .kind = long_kind, .name = "000"})
                   .unwrap_err()),
              Status::InvArgs);
    EXPECT_EQ(
        (prot::dev::encode_unpublish(bytes, {.txid = TXID, .kind = "block", .name = long_name})
             .unwrap_err()),
        Status::InvArgs);
    EXPECT_EQ((prot::dev::encode_unpublish(bytes, {.txid = TXID, .kind = "blo/ck", .name = "000"})
                   .unwrap_err()),
              Status::InvArgs);
    EXPECT_EQ((prot::dev::encode_unpublish(
                   bytes, {.txid = TXID, .kind = "block", .name = std::string_view{"0\000", 3}})
                   .unwrap_err()),
              Status::InvArgs);
    // An empty field names nothing.
    EXPECT_EQ((prot::dev::encode_unpublish(bytes, {.txid = TXID, .kind = "", .name = "000"})
                   .unwrap_err()),
              Status::InvArgs);
}

TEST(OursDevUnpublish, DecodeRefusals) {
    auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
    auto const request =
        prot::dev::encode_unpublish(bytes, {.txid = TXID, .kind = "block", .name = "000"});
    ASSERT_TRUE(request.is_ok());
    // One byte short, and one over: the sizes and the length disagree.
    EXPECT_EQ(prot::dev::decode_unpublish(
                  wire::Message{.bytes = std::span{bytes}.first(request.unwrap().bytes.size() - 1),
                                .handles = {}})
                  .unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(prot::dev::decode_unpublish(
                  wire::Message{.bytes = std::span{bytes}.first(request.unwrap().bytes.size() + 1),
                                .handles = {}})
                  .unwrap_err(),
              Status::InvArgs);
    // A handle on an un-publish is not this protocol's request.
    auto handles = std::array<Uandle, 1>{DIRECTORY};
    auto with_handle =
        prot::dev::encode_unpublish(bytes, {.txid = TXID, .kind = "block", .name = "000"});
    ASSERT_TRUE(with_handle.is_ok());
    EXPECT_EQ(prot::dev::decode_unpublish(
                  wire::Message{.bytes = std::span{bytes}.first(with_handle.unwrap().bytes.size()),
                                .handles = handles})
                  .unwrap_err(),
              Status::InvArgs);
    // The wrong txid is not this conversation's reply.
    auto const reply = prot::dev::encode_unpublish_reply(bytes, TXID, {.status = Status::Ok});
    ASSERT_TRUE(reply.is_ok());
    EXPECT_EQ(prot::dev::decode_unpublish_reply(
                  wire::Message{.bytes = std::span{bytes}.first(reply.unwrap().bytes.size()),
                                .handles = {}},
                  TXID + 1)
                  .unwrap_err(),
              Status::InvArgs);
    // A failure reply is the bare status form; a longer one is malformed.
    auto const refused =
        prot::dev::encode_unpublish_reply(bytes, TXID, {.status = Status::NotFound});
    ASSERT_TRUE(refused.is_ok());
    EXPECT_EQ(prot::dev::decode_unpublish_reply(
                  wire::Message{.bytes = std::span{bytes}.first(refused.unwrap().bytes.size()),
                                .handles = {}},
                  TXID)
                  .unwrap()
                  .status,
              Status::NotFound);
}

/// Hand-built frames for the refusals no well-behaved encoder can produce:
/// each pins one guard of the codec, the way a hostile peer would hit it.
namespace {
/// A publish frame with the fields the caller names, valid or not: the
/// hostile peer's tool for reaching the decoders' guards.
auto raw_publish_frame(std::span<u8> out, u32 const txid, std::string_view const kind,
                       std::string_view const name, std::string_view const protocol,
                       bool const with_handle) -> usize {
    auto const total = usize{36} + kind.size() + name.size() + protocol.size();
    auto started = wire::start_request(out, total, prot::dev::PUBLISH, txid);
    EXPECT_TRUE(started.is_ok());
    if (with_handle) {
        wire::store(out.first(total), 32, wire::HANDLE_PRESENT);
    }
    wire::store(out.first(total), 20, static_cast<u32>(kind.size()));
    wire::store(out.first(total), 24, static_cast<u32>(name.size()));
    wire::store(out.first(total), 28, static_cast<u32>(protocol.size()));
    auto at = usize{36};
    for (auto const text: {kind, name, protocol}) {
        std::memcpy(out.data() + at, text.data(), text.size());
        at += text.size();
    }
    return total;
}
} // namespace

TEST(OursDevPublish, HostileFrames) {
    auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
    auto handles = std::array<Uandle, 1>{DIRECTORY};
    auto const message_for = [&](usize const size, bool const with_handle) {
        return wire::Message{.bytes = std::span{bytes}.first(size),
                             .handles = with_handle ? std::span{handles}.first(1)
                                                    : std::span<Uandle const>{}};
    };

    // A header that is not a message at all: the decode refuses.
    auto garbage = std::array<u8, 64>{};
    garbage.fill(0x5A);
    EXPECT_EQ(prot::dev::decode_publish(
                  wire::Message{.bytes = std::span{garbage}.first(64), .handles = {}})
                  .unwrap_err(),
              Status::InvArgs);

    // A protocol field that names a path: the wire carries it, the decoder
    // refuses it.
    auto const size = raw_publish_frame(bytes, TXID, "block", "000", "ours/blocked", true);
    EXPECT_EQ(prot::dev::decode_publish(message_for(size, true)).unwrap_err(), Status::InvArgs);

    // The handle field present but Invalid: the decode refuses.
    handles[0] = Uandle::Invalid;
    auto const invalid_size =
        raw_publish_frame(bytes, TXID, "block", "000", "ours.block.Block", true);
    EXPECT_EQ(prot::dev::decode_publish(message_for(invalid_size, true)).unwrap_err(),
              Status::InvArgs);
    handles[0] = DIRECTORY;

    // The handle field absent: PRP-less publish, refused.
    auto const bare_size =
        raw_publish_frame(bytes, TXID, "block", "000", "ours.block.Block", false);
    EXPECT_EQ(prot::dev::decode_publish(message_for(bare_size, false)).unwrap_err(),
              Status::InvArgs);

    // A reply whose name size disagrees with its length: malformed.
    auto reply = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
    auto started = wire::start_ok_reply(reply, 24 + 4, prot::dev::PUBLISH, TXID);
    ASSERT_TRUE(started.is_ok());
    wire::store(std::span{reply}.first(24), 20, u32{4});
    std::memcpy(reply.data() + 24, "abc", 3);
    EXPECT_EQ(prot::dev::decode_publish_reply(
                  wire::Message{.bytes = std::span{reply}.first(27), .handles = {}}, TXID)
                  .unwrap_err(),
              Status::InvArgs);
}

TEST(OursDevPublish, SmallReplyBufferRefused) {
    // An Ok reply with the shortest legal granted name needs 24 bytes plus
    // the name; a reply buffer under that is refused by the start itself.
    auto tiny = std::array<u8, 16>{};
    EXPECT_EQ((prot::dev::encode_publish_reply(tiny, TXID, {.status = Status::Ok, .name = "000"})
                   .unwrap_err()),
              Status::BufferTooSmall);
}

TEST(OursDevUnpublish, HostileFrames) {
    auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
    // A header that is not a message: refused.
    auto garbage = std::array<u8, 64>{};
    garbage.fill(0x5A);
    EXPECT_EQ(prot::dev::decode_unpublish(
                  wire::Message{.bytes = std::span{garbage}.first(64), .handles = {}})
                  .unwrap_err(),
              Status::InvArgs);

    // A kind that names a path on the wire: carried, refused.
    auto const total = usize{28} + 7 + 3;
    auto started = wire::start_request(bytes, total, prot::dev::UNPUBLISH, TXID);
    ASSERT_TRUE(started.is_ok());
    wire::store(std::span{bytes}.first(total), 20, u32{7});
    wire::store(std::span{bytes}.first(total), 24, u32{3});
    std::memcpy(bytes.data() + 28, "blo/ck", 6);
    std::memcpy(bytes.data() + 34, "000", 3);
    EXPECT_EQ(prot::dev::decode_unpublish(
                  wire::Message{.bytes = std::span{bytes}.first(total), .handles = {}})
                  .unwrap_err(),
              Status::InvArgs);

    // The encode into a buffer under the frame's size: BufferTooSmall.
    auto tiny = std::array<u8, 24>{};
    EXPECT_EQ((prot::dev::encode_unpublish(tiny, {.txid = TXID, .kind = "block", .name = "000"})
                   .unwrap_err()),
              Status::BufferTooSmall);
}
