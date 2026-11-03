/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The ours.block codec: round trips for both methods, and the refusals a
// closed protocol's decoders owe -- malformed bodies, a count out of range,
// data next to a failure.
#include <prot/block.hpp>

#include <array>
#include <span>
#include <vector>

#include <gtest/gtest.h>
#include <ours/status.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;
using prot::block::BLOCK_SIZE;
using prot::block::MAX_READ_BLOCKS;

constexpr auto TXID = u32{7};

class Buffer {
  public:
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> handles{};

    [[nodiscard]]
    auto view(usize const size) const -> wire::Message {
        return {.bytes = std::span{bytes}.first(size), .handles = {}};
    }

    [[nodiscard]]
    auto view(std::span<u8 const> const prefix) const -> wire::Message {
        return {.bytes = prefix, .handles = {}};
    }
};

} // namespace

TEST(OursBlockGetInfo, RoundTrip) {
    auto buffer = Buffer{};
    auto const request = prot::block::encode_get_info(buffer.bytes, {.txid = TXID});
    ASSERT_TRUE(request.is_ok());
    EXPECT_EQ(request->bytes.size(), 16u);
    auto const decoded = prot::block::decode_get_info(buffer.view(request->bytes));
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->txid, TXID);

    auto const reply = prot::block::encode_get_info_reply(
        buffer.bytes, TXID, {.status = Status::Ok, .block_size = BLOCK_SIZE, .blocks = 131072});
    ASSERT_TRUE(reply.is_ok());
    EXPECT_EQ(reply->bytes.size(), 32u);
    auto const read = prot::block::decode_get_info_reply(buffer.view(reply->bytes), TXID);
    ASSERT_TRUE(read.is_ok());
    EXPECT_EQ(read->status, Status::Ok);
    EXPECT_EQ(read->block_size, BLOCK_SIZE);
    EXPECT_EQ(read->blocks, 131072u);

    // A failure is the status form, with nothing else in it.
    auto const failed =
        prot::block::encode_get_info_reply(buffer.bytes, TXID, {.status = Status::NotFound});
    ASSERT_TRUE(failed.is_ok());
    EXPECT_EQ(failed->bytes.size(), 24u);
    auto const denied = prot::block::decode_get_info_reply(buffer.view(failed->bytes), TXID);
    ASSERT_TRUE(denied.is_ok());
    EXPECT_EQ(denied->status, Status::NotFound);
    EXPECT_EQ(denied->block_size, 0u);
}

TEST(OursBlockRead, RoundTrip) {
    auto buffer = Buffer{};
    auto const request =
        prot::block::encode_read(buffer.bytes, {.txid = TXID, .lba = 64, .count = 4});
    ASSERT_TRUE(request.is_ok());
    EXPECT_EQ(request->bytes.size(), 28u);
    auto const decoded = prot::block::decode_read(buffer.view(request->bytes));
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->txid, TXID);
    EXPECT_EQ(decoded->lba, 64u);
    EXPECT_EQ(decoded->count, 4u);

    std::vector<u8> const data(BLOCK_SIZE * 2, 0x5A);
    auto const reply =
        prot::block::encode_read_reply(buffer.bytes, TXID, {.status = Status::Ok, .data = data});
    ASSERT_TRUE(reply.is_ok());
    EXPECT_EQ(reply->bytes.size(), 20 + data.size());
    auto const read = prot::block::decode_read_reply(buffer.view(reply->bytes), TXID);
    ASSERT_TRUE(read.is_ok());
    EXPECT_EQ(read->status, Status::Ok);
    EXPECT_TRUE(std::equal(read->data.begin(), read->data.end(), data.begin()));
}

TEST(OursBlockRead, Refusals) {
    auto buffer = Buffer{};
    // A count of zero, and one past what a reply can carry.
    EXPECT_EQ(
        prot::block::encode_read(buffer.bytes, {.txid = 1, .lba = 0, .count = 0}).unwrap_err(),
        Status::InvArgs);
    EXPECT_EQ(
        prot::block::encode_read(buffer.bytes, {.txid = 1, .lba = 0, .count = MAX_READ_BLOCKS + 1})
            .unwrap_err(),
        Status::InvArgs);
    // A txid nobody may use.
    EXPECT_EQ(
        prot::block::encode_read(buffer.bytes, {.txid = 0, .lba = 0, .count = 1}).unwrap_err(),
        Status::InvArgs);

    // The decoder refuses a body of the wrong length, and the same count
    // out of range.
    auto const request = prot::block::encode_read(buffer.bytes, {.txid = 7, .lba = 1, .count = 1});
    ASSERT_TRUE(request.is_ok());
    // A body cut short by a byte, and the same body with a count past the
    // maximum.
    auto const short_body = request->bytes.first(request->bytes.size() - 1);
    EXPECT_TRUE(prot::block::decode_read(buffer.view(short_body)).is_err());
    auto broken = buffer.bytes;
    std::ranges::copy(request->bytes, broken.begin());
    wire::store(broken, 24, prot::block::MAX_READ_BLOCKS + 1);
    EXPECT_TRUE(prot::block::decode_read(buffer.view(broken)).is_err());

    // A reply whose data is not whole blocks is malformed; so is data next
    // to a failure.
    std::array<u8, 3> const partial{1, 2, 3};
    EXPECT_EQ(
        prot::block::encode_read_reply(buffer.bytes, 7, {.status = Status::Ok, .data = partial})
            .unwrap_err(),
        Status::InvArgs);
    std::array<u8, 4> const one_block{1, 2, 3, 4};
    EXPECT_EQ(prot::block::encode_read_reply(buffer.bytes, 7,
                                             {.status = Status::AccessDenied, .data = one_block})
                  .unwrap_err(),
              Status::InvArgs);
}
