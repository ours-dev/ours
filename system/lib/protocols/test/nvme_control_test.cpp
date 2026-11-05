/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The ours.nvme.Control codec (<prot/nvme_control.hpp>): the rescan and its
// bare-status reply are encoded, decoded back, and then broken on purpose,
// one malformed form at a time; another family's messages are refused by
// these decoders.
#include <prot/dev.hpp>
#include <prot/nvme_control.hpp>

#include <array>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;
using wire::Message;

constexpr auto FILL = u8{0xCC};

/// Room for one message, starting dirty so that a byte the encoder forgot
/// shows.
struct Buffer {
    std::vector<u8> bytes = std::vector<u8>(wire::MAX_MESSAGE_BYTES, FILL);
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> handles{};
};

/// A message copied out of the buffer it was encoded in, so that a test can
/// break it.
struct Owned {
    std::vector<u8> bytes;
    std::vector<Uandle> handles;

    explicit Owned(Message const message)
        : bytes(message.bytes.begin(), message.bytes.end())
        , handles(message.handles.begin(), message.handles.end()) {}

    [[nodiscard]]
    auto view() const -> Message {
        return Message{.bytes = bytes, .handles = handles};
    }

    [[nodiscard]]
    auto cut(usize const size) const -> Message {
        return Message{.bytes = std::span{bytes}.first(size), .handles = handles};
    }

    auto set_u32(usize const offset, u32 const value) -> void {
        wire::store(std::span{bytes}, offset, value);
    }
};

auto owned(ustl::Result<Message, Status> const &encoded) -> Owned {
    EXPECT_TRUE(encoded.is_ok());
    return Owned(encoded.unwrap());
}

TEST(OursNvmeRescan, RoundTrip) {
    Buffer buffer;
    auto const message = owned(prot::nvme::encode_rescan(buffer.bytes, {.txid = 9}));
    EXPECT_EQ(message.bytes.size(), wire::HEADER_SIZE);
    EXPECT_TRUE(message.handles.empty());

    auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header->ordinal, prot::nvme::CONTROL_RESCAN.ordinal);

    auto const decoded = prot::nvme::decode_rescan(message.view());
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->txid, 9u);
}

TEST(OursNvmeRescan, Refusals) {
    Buffer buffer;
    EXPECT_EQ(prot::nvme::encode_rescan(buffer.bytes, {.txid = 0}).unwrap_err(), Status::InvArgs);
    EXPECT_EQ(
        prot::nvme::encode_rescan(std::span{buffer.bytes}.first(15), {.txid = 9}).unwrap_err(),
        Status::BufferTooSmall);

    auto const good = owned(prot::nvme::encode_rescan(buffer.bytes, {.txid = 9}));
    auto refused = [](Message const message) {
        auto const decoded = prot::nvme::decode_rescan(message);
        return decoded.is_err() && decoded.unwrap_err() == Status::InvArgs;
    };
    // Exactly the header, no handles, a txid, this method, toward the
    // server.
    auto longer = good;
    longer.bytes.push_back(0);
    EXPECT_TRUE(refused(longer.view()));
    EXPECT_TRUE(refused(good.cut(15)));
    auto with_handle = good;
    with_handle.handles.push_back(Uandle{0x1003});
    EXPECT_TRUE(refused(with_handle.view()));
    auto no_txid = good;
    no_txid.set_u32(0, 0);
    EXPECT_TRUE(refused(no_txid.view()));
    auto other = good;
    wire::store(std::span{other.bytes}, 8, u64{0x1234});
    EXPECT_TRUE(refused(other.view()));
    auto reply = good;
    reply.bytes[4] = static_cast<u8>(wire::Direction::ToClient);
    EXPECT_TRUE(refused(reply.view()));
}

TEST(OursNvmeRescanReply, RoundTrip) {
    Buffer buffer;
    // Ok names a function claimed; NotFound names an unchanged machine.
    // Both ride the bare status form.
    for (auto const status: {Status::Ok, Status::NotFound}) {
        auto const message =
            owned(prot::nvme::encode_rescan_reply(buffer.bytes, 9, {.status = status}));
        EXPECT_EQ(message.bytes.size(), wire::STATUS_FORM_SIZE);
        EXPECT_TRUE(message.handles.empty());

        auto const decoded = prot::nvme::decode_rescan_reply(message.view(), 9);
        ASSERT_TRUE(decoded.is_ok());
        EXPECT_EQ(decoded->status, status);
    }
}

TEST(OursNvmeRescanReply, Refusals) {
    Buffer buffer;
    auto const good =
        owned(prot::nvme::encode_rescan_reply(buffer.bytes, 9, {.status = Status::Ok}));
    auto refused = [](Message const message, u32 const txid) {
        auto const decoded = prot::nvme::decode_rescan_reply(message, txid);
        return decoded.is_err() && decoded.unwrap_err() == Status::InvArgs;
    };
    // Another request's txid, a cut message, a direction flipped.
    EXPECT_TRUE(refused(good.view(), 8));
    EXPECT_TRUE(refused(good.cut(19), 9));
    auto request = good;
    request.bytes[4] = static_cast<u8>(wire::Direction::ToServer);
    EXPECT_TRUE(refused(request.view(), 9));
    // The reply to another method, on this one's wire shape.
    Buffer other_buffer;
    auto const other =
        owned(prot::dev::encode_unpublish_reply(other_buffer.bytes, 9, {.status = Status::Ok}));
    EXPECT_TRUE(refused(other.view(), 9));
}

// A connection speaks exactly one protocol: a driver's control refuses
// another family's request, and its client another family's reply. Without
// this, a route that bound a connection to the wrong provider would read as
// a rescan.
TEST(OursNvme, RefusesAnotherFamily) {
    Buffer request_buffer;
    auto const request = owned(prot::dev::encode_unpublish(
        request_buffer.bytes, {.txid = 9, .kind = "block", .name = "000"}));
    auto const decoded = prot::nvme::decode_rescan(request.view());
    ASSERT_TRUE(decoded.is_err());
    EXPECT_EQ(decoded.unwrap_err(), Status::InvArgs);

    Buffer reply_buffer;
    auto const reply =
        owned(prot::dev::encode_unpublish_reply(reply_buffer.bytes, 9, {.status = Status::Ok}));
    auto const decoded_reply = prot::nvme::decode_rescan_reply(reply.view(), 9);
    ASSERT_TRUE(decoded_reply.is_err());
    EXPECT_EQ(decoded_reply.unwrap_err(), Status::InvArgs);
}

TEST(OursNvmeControl, Names) {
    // The name a program finds the control under in its /svc, and the
    // selector its rescan is hashed from, spell the same library and
    // protocol.
    EXPECT_EQ(prot::nvme::CONTROL_NAME, "ours.nvme.Control");
    EXPECT_EQ(prot::nvme::CONTROL_PATH, "/svc/ours.nvme.Control");
    EXPECT_EQ(prot::nvme::CONTROL_RESCAN.selector, "ours.nvme/Control.Rescan");
    EXPECT_EQ(prot::nvme::CONTROL_RESCAN.kind, wire::MethodKind::TwoWay);
}
} // namespace
