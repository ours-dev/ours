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
                                                    .klass = "block",
                                                    .name = "000",
                                                    .protocol = "ours.block.Block",
                                                    .directory = DIRECTORY});
    ASSERT_TRUE(request.is_ok());
    auto const message = buffer.view(request.unwrap().bytes.size());

    auto const decoded = prot::dev::decode_publish(message);
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->txid, TXID);
    EXPECT_EQ(decoded->klass, "block");
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
                                                    .klass = "block",
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
                                          .klass = "block/../svc",
                                          .name = "000",
                                          .protocol = "ours.block.Block",
                                          .directory = DIRECTORY})
                   .unwrap_err()),
              Status::InvArgs);
    EXPECT_EQ((prot::dev::encode_publish(buffer.bytes, handles,
                                         {.txid = TXID,
                                          .klass = "block",
                                          .name = "0/0",
                                          .protocol = "ours.block.Block",
                                          .directory = DIRECTORY})
                   .unwrap_err()),
              Status::InvArgs);
    // A NUL is a path's end, same refusal.
    EXPECT_EQ((prot::dev::encode_publish(buffer.bytes, handles,
                                         {.txid = TXID,
                                          .klass = std::string_view{"blo\0k", 5},
                                          .name = "000",
                                          .protocol = "ours.block.Block",
                                          .directory = DIRECTORY})
                   .unwrap_err()),
              Status::InvArgs);
    // The bounds, each by one over.
    auto long_class = std::string(prot::dev::MAX_CLASS_LENGTH + 1, 'x');
    EXPECT_EQ((prot::dev::encode_publish(buffer.bytes, handles,
                                         {.txid = TXID,
                                          .klass = long_class,
                                          .name = "000",
                                          .protocol = "ours.block.Block",
                                          .directory = DIRECTORY})
                   .unwrap_err()),
              Status::InvArgs);
    auto long_name = std::string(prot::dev::MAX_NODE_NAME + 1, 'x');
    EXPECT_EQ((prot::dev::encode_publish(buffer.bytes, handles,
                                         {.txid = TXID,
                                          .klass = "block",
                                          .name = long_name,
                                          .protocol = "ours.block.Block",
                                          .directory = DIRECTORY})
                   .unwrap_err()),
              Status::InvArgs);
    // No handle, no publish.
    EXPECT_EQ((prot::dev::encode_publish(buffer.bytes, handles,
                                         {.txid = TXID,
                                          .klass = "block",
                                          .name = "000",
                                          .protocol = "ours.block.Block",
                                          .directory = Uandle::Invalid})
                   .unwrap_err()),
              Status::InvArgs);
    // An empty name decodes as nothing to mount.
    EXPECT_EQ((prot::dev::encode_publish(buffer.bytes, handles,
                                         {.txid = TXID,
                                          .klass = "block",
                                          .name = "",
                                          .protocol = "ours.block.Block",
                                          .directory = DIRECTORY})
                   .unwrap_err()),
              Status::InvArgs);
}

TEST(OursDevPublish, DecodeRefusals) {
    auto buffer = Buffer{};
    auto const request = prot::dev::encode_publish(buffer.bytes, buffer.handles,
                                                   {.txid = TXID,
                                                    .klass = "block",
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
