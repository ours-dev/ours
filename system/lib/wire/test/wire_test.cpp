/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The framing (<wire/wire.hpp>): the header every message starts with, the
// status form of a reply, the epitaph and handle fields. Each is written,
// read back, and broken byte by byte, since a server reads what programs it
// does not trust send it.
#include <wire/wire.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <utility>
#include <vector>

#include <cstddef>

#include <gtest/gtest.h>

namespace {
using ours::Status;
using ours::Uandle;
using wire::Direction;
using wire::Header;
using wire::Message;
using wire::Method;
using wire::MethodKind;

// The header is fidl_message_header_t's shape, with the direction in byte 4.
static_assert(offsetof(Header, txid) == 0);
static_assert(offsetof(Header, direction) == 4);
static_assert(offsetof(Header, reserved) == 5);
static_assert(offsetof(Header, magic) == 7);
static_assert(offsetof(Header, ordinal) == 8);

/// Two made-up methods. Their selectors are never hashed: only the codec
/// headers' declarations are checked by ordinals_test.py.
constexpr auto TWO_WAY = Method{
    .selector = "test/Protocol.TwoWay",
    .ordinal = 0x1234'5678'9abc'def0,
    .kind = MethodKind::TwoWay,
};
constexpr auto ONE_WAY = Method{
    .selector = "test/Protocol.OneWay",
    .ordinal = 0x0fed'cba9'8765'4321,
    .kind = MethodKind::OneWay,
};

constexpr auto FILL = u8{0xCC};

/// A buffer that starts full of FILL, so that a byte an encoder forgot to
/// write shows.
auto dirty_buffer() -> std::vector<u8> {
    return std::vector<u8>(wire::MAX_MESSAGE_BYTES, FILL);
}

/// A copy of |message|'s bytes, to be broken on purpose.
auto bytes_of(Message const message) -> std::vector<u8> {
    return {message.bytes.begin(), message.bytes.end()};
}

auto as_message(std::vector<u8> const &bytes, std::span<Uandle const> const handles = {})
    -> Message {
    return Message{.bytes = bytes, .handles = handles};
}

auto encoded_status_reply(std::vector<u8> &buffer, u32 const txid, Status const status) -> Message {
    auto encoded = wire::encode_status_reply(buffer, TWO_WAY, txid, status);
    EXPECT_TRUE(encoded.is_ok());
    return encoded.unwrap();
}

TEST(WireHeader, StartMessageZeroesAndWritesTheHeader) {
    auto buffer = dirty_buffer();
    auto started = wire::start_message(buffer, 40, 7, Direction::ToServer, TWO_WAY.ordinal);
    ASSERT_TRUE(started.is_ok());
    auto const bytes = started.unwrap();
    EXPECT_EQ(bytes.data(), buffer.data());
    ASSERT_EQ(bytes.size(), 40u);
    for (usize at = wire::HEADER_SIZE; at < bytes.size(); ++at) {
        EXPECT_EQ(bytes[at], 0) << "byte " << at;
    }
    // Only the message's own bytes are written.
    EXPECT_EQ(buffer[40], FILL);

    auto const header = wire::decode_header(bytes, Direction::ToServer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header->txid, 7u);
    EXPECT_EQ(header->direction, Direction::ToServer);
    EXPECT_EQ(header->magic, wire::MAGIC);
    EXPECT_EQ(header->ordinal, TWO_WAY.ordinal);
    EXPECT_EQ(bytes[5], 0);
    EXPECT_EQ(bytes[6], 0);
}

TEST(WireHeader, StartMessageRefusals) {
    auto buffer = dirty_buffer();
    EXPECT_EQ(
        wire::start_message(buffer, wire::HEADER_SIZE - 1, 0, Direction::ToServer, 1).unwrap_err(),
        Status::InvArgs);
    EXPECT_EQ(wire::start_message(buffer, wire::MAX_MESSAGE_BYTES + 1, 0, Direction::ToServer, 1)
                  .unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(wire::start_message(std::span{buffer}.first(23), 24, 0, Direction::ToServer, 1)
                  .unwrap_err(),
              Status::BufferTooSmall);
}

TEST(WireHeader, DecodeRefusals) {
    auto buffer = dirty_buffer();
    auto const good = bytes_of(
        Message{.bytes = wire::start_message(buffer, 24, 0, Direction::ToServer, 1).unwrap(),
                .handles = {}});
    ASSERT_TRUE(wire::decode_header(good, Direction::ToServer).is_ok());

    // Shorter than a header, and longer than a channel message.
    EXPECT_TRUE(wire::decode_header(std::span{good}.first(15), Direction::ToServer).is_err());
    std::vector<u8> too_long(wire::MAX_MESSAGE_BYTES + 1, 0);
    std::copy(good.begin(), good.end(), too_long.begin());
    EXPECT_TRUE(wire::decode_header(too_long, Direction::ToServer).is_err());

    // Each of the bytes that must hold a fixed value.
    for (auto const at: {usize{5}, usize{6}, usize{7}}) {
        auto broken = good;
        broken[at] ^= 0x40;
        EXPECT_EQ(wire::decode_header(broken, Direction::ToServer).unwrap_err(), Status::InvArgs)
            << "byte " << at;
    }

    // A server never reads a reply as a request, nor a client the reverse.
    EXPECT_TRUE(wire::decode_header(good, Direction::ToClient).is_err());
    auto reply = good;
    reply[4] = std::to_underlying(Direction::ToClient);
    EXPECT_TRUE(wire::decode_header(reply, Direction::ToServer).is_err());
    auto unknown_direction = good;
    unknown_direction[4] = 2;
    EXPECT_TRUE(wire::decode_header(unknown_direction, Direction::ToServer).is_err());
    EXPECT_TRUE(wire::decode_header(unknown_direction, Direction::ToClient).is_err());
}

TEST(WireRequest, TxidRules) {
    auto buffer = dirty_buffer();

    // A one-way request carries txid 0, a two-way one 1 to MAX_TXID.
    EXPECT_TRUE(wire::start_request(buffer, 24, ONE_WAY, 0).is_ok());
    EXPECT_EQ(wire::start_request(buffer, 24, ONE_WAY, 1).unwrap_err(), Status::InvArgs);
    EXPECT_EQ(wire::start_request(buffer, 24, TWO_WAY, 0).unwrap_err(), Status::InvArgs);
    EXPECT_TRUE(wire::start_request(buffer, 24, TWO_WAY, wire::MAX_TXID).is_ok());
    EXPECT_EQ(wire::start_request(buffer, 24, TWO_WAY, wire::MAX_TXID + 1).unwrap_err(),
              Status::InvArgs);

    auto const request = bytes_of(Message{
        .bytes = wire::start_request(buffer, 24, TWO_WAY, 9).unwrap(),
        .handles = {},
    });
    auto const header = wire::decode_request_header(as_message(request), TWO_WAY);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header->txid, 9u);

    // The same bytes are not a request of another method.
    EXPECT_TRUE(wire::decode_request_header(as_message(request), ONE_WAY).is_err());

    // A two-way request with txid 0, or one above MAX_TXID.
    for (auto const txid: {u32{0}, wire::MAX_TXID + 1}) {
        auto broken = request;
        wire::store(std::span{broken}, 0, txid);
        EXPECT_TRUE(wire::decode_request_header(as_message(broken), TWO_WAY).is_err())
            << "txid " << txid;
    }
    // A one-way request with a txid.
    auto one_way = bytes_of(Message{
        .bytes = wire::start_request(buffer, 24, ONE_WAY, 0).unwrap(),
        .handles = {},
    });
    EXPECT_TRUE(wire::decode_request_header(as_message(one_way), ONE_WAY).is_ok());
    wire::store(std::span{one_way}, 0, u32{3});
    EXPECT_TRUE(wire::decode_request_header(as_message(one_way), ONE_WAY).is_err());
}

TEST(WireReply, StatusFormRoundTrip) {
    for (auto const status:
         {Status::Ok, Status::AccessDenied, Status::NotFound, Status::BufferTooSmall}) {
        auto buffer = dirty_buffer();
        auto const reply = encoded_status_reply(buffer, 42, status);
        ASSERT_EQ(reply.bytes.size(), wire::STATUS_FORM_SIZE);
        EXPECT_TRUE(reply.handles.empty());
        EXPECT_EQ(wire::load<u32>(reply.bytes, 20), 0u);

        auto const decoded = wire::decode_reply_status(reply, TWO_WAY, 42);
        ASSERT_TRUE(decoded.is_ok());
        EXPECT_EQ(decoded.unwrap(), status);
    }
}

TEST(WireReply, StatusFormEncodeRefusals) {
    auto buffer = dirty_buffer();
    EXPECT_EQ(wire::encode_status_reply(buffer, ONE_WAY, 0, Status::Ok).unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(wire::encode_status_reply(buffer, TWO_WAY, 0, Status::Ok).unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(
        wire::encode_status_reply(buffer, TWO_WAY, wire::MAX_TXID + 1, Status::Ok).unwrap_err(),
        Status::InvArgs);
    EXPECT_EQ(wire::encode_status_reply(buffer, TWO_WAY, 1, Status::MaxCount).unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(
        wire::encode_status_reply(std::span{buffer}.first(23), TWO_WAY, 1, Status::Ok).unwrap_err(),
        Status::BufferTooSmall);
}

TEST(WireReply, DecodeRefusals) {
    auto buffer = dirty_buffer();
    auto const good = bytes_of(encoded_status_reply(buffer, 42, Status::AccessDenied));
    ASSERT_TRUE(wire::decode_reply_status(as_message(good), TWO_WAY, 42).is_ok());

    // Another request's reply, another method's, or a one-way method's.
    EXPECT_TRUE(wire::decode_reply_status(as_message(good), TWO_WAY, 43).is_err());
    EXPECT_TRUE(wire::decode_reply_status(as_message(good), ONE_WAY, 42).is_err());
    auto other_ordinal = good;
    wire::store(std::span{other_ordinal}, 8, TWO_WAY.ordinal + 1);
    EXPECT_TRUE(wire::decode_reply_status(as_message(other_ordinal), TWO_WAY, 42).is_err());

    // The txid the client passes must itself be a two-way one.
    auto zero_txid = good;
    wire::store(std::span{zero_txid}, 0, u32{0});
    EXPECT_TRUE(wire::decode_reply_status(as_message(zero_txid), TWO_WAY, 0).is_err());

    // A request is never read as a reply.
    auto request = good;
    request[4] = std::to_underlying(Direction::ToServer);
    EXPECT_TRUE(wire::decode_reply_status(as_message(request), TWO_WAY, 42).is_err());

    // No status at all.
    EXPECT_TRUE(
        wire::decode_reply_status(as_message({good.begin(), good.begin() + 19}), TWO_WAY, 42)
            .is_err());

    // A status ours::Status does not name.
    auto unknown = good;
    wire::store(std::span{unknown}, 16, std::to_underlying(Status::MaxCount));
    EXPECT_TRUE(wire::decode_reply_status(as_message(unknown), TWO_WAY, 42).is_err());

    // A failure is exactly the status form: no extra byte, a zero pad, no
    // handle.
    auto longer = good;
    longer.push_back(0);
    EXPECT_TRUE(wire::decode_reply_status(as_message(longer), TWO_WAY, 42).is_err());
    auto padded = good;
    padded[20] = 1;
    EXPECT_TRUE(wire::decode_reply_status(as_message(padded), TWO_WAY, 42).is_err());
    std::array const handle{Uandle{0x13}};
    EXPECT_TRUE(wire::decode_reply_status(as_message(good, handle), TWO_WAY, 42).is_err());
}

TEST(WireReply, OkLeavesTheBodyToTheMethod) {
    auto buffer = dirty_buffer();
    auto started = wire::start_ok_reply(buffer, 32, TWO_WAY, 5);
    ASSERT_TRUE(started.is_ok());
    auto const bytes = started.unwrap();
    EXPECT_EQ(wire::load<u32>(bytes, wire::STATUS_OFFSET), 0u);

    // An Ok reply may be longer than the status form and carry handles:
    // what it may carry is the method's codec's to check.
    std::array const handle{Uandle{0x13}};
    auto const decoded =
        wire::decode_reply_status(Message{.bytes = bytes, .handles = handle}, TWO_WAY, 5);
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded.unwrap(), Status::Ok);

    EXPECT_EQ(wire::start_ok_reply(buffer, 32, ONE_WAY, 0).unwrap_err(), Status::InvArgs);
    EXPECT_EQ(wire::start_ok_reply(buffer, 32, TWO_WAY, 0).unwrap_err(), Status::InvArgs);
    EXPECT_EQ(wire::start_ok_reply(buffer, 20, TWO_WAY, 5).unwrap_err(), Status::InvArgs);
}

TEST(WireEpitaph, RoundTrip) {
    for (auto const status: {Status::Ok, Status::BadPath, Status::NotFound}) {
        auto buffer = dirty_buffer();
        auto encoded = wire::encode_epitaph(buffer, status);
        ASSERT_TRUE(encoded.is_ok());
        auto const epitaph = encoded.unwrap();
        ASSERT_EQ(epitaph.bytes.size(), wire::STATUS_FORM_SIZE);
        EXPECT_TRUE(epitaph.handles.empty());

        auto const header = wire::decode_header(epitaph.bytes, Direction::ToClient);
        ASSERT_TRUE(header.is_ok());
        EXPECT_EQ(header->txid, 0u);
        EXPECT_EQ(header->ordinal, wire::EPITAPH_ORDINAL);

        auto const decoded = wire::decode_epitaph(epitaph);
        ASSERT_TRUE(decoded.is_ok());
        EXPECT_EQ(decoded.unwrap(), status);
    }
}

TEST(WireEpitaph, Refusals) {
    auto buffer = dirty_buffer();
    EXPECT_EQ(wire::encode_epitaph(buffer, Status::MaxCount).unwrap_err(), Status::InvArgs);
    EXPECT_EQ(wire::encode_epitaph(std::span{buffer}.first(23), Status::Ok).unwrap_err(),
              Status::BufferTooSmall);

    auto const good = bytes_of(wire::encode_epitaph(buffer, Status::NotFound).unwrap());
    ASSERT_TRUE(wire::decode_epitaph(as_message(good)).is_ok());

    auto with_txid = good;
    wire::store(std::span{with_txid}, 0, u32{1});
    EXPECT_TRUE(wire::decode_epitaph(as_message(with_txid)).is_err());

    auto not_epitaph = good;
    wire::store(std::span{not_epitaph}, 8, TWO_WAY.ordinal);
    EXPECT_TRUE(wire::decode_epitaph(as_message(not_epitaph)).is_err());

    auto to_server = good;
    to_server[4] = std::to_underlying(Direction::ToServer);
    EXPECT_TRUE(wire::decode_epitaph(as_message(to_server)).is_err());

    auto longer = good;
    longer.push_back(0);
    EXPECT_TRUE(wire::decode_epitaph(as_message(longer)).is_err());
    EXPECT_TRUE(wire::decode_epitaph(as_message({good.begin(), good.end() - 1})).is_err());

    auto padded = good;
    padded[23] = 1;
    EXPECT_TRUE(wire::decode_epitaph(as_message(padded)).is_err());

    auto unknown = good;
    wire::store(std::span{unknown}, 16, std::to_underlying(Status::MaxCount));
    EXPECT_TRUE(wire::decode_epitaph(as_message(unknown)).is_err());

    std::array const handle{Uandle{0x13}};
    EXPECT_TRUE(wire::decode_epitaph(as_message(good, handle)).is_err());
}

TEST(WireHandle, RoundTripAndRefusals) {
    auto bytes = dirty_buffer();
    std::array<Uandle, 2> handles{};
    auto const span = std::span{bytes}.first(32);

    ASSERT_EQ(wire::encode_handle(span, 16, handles, 0, Uandle{0x23}), Status::Ok);
    ASSERT_EQ(wire::encode_handle(span, 24, handles, 1, Uandle{0x33}), Status::Ok);
    EXPECT_EQ(wire::load<u32>(span, 16), wire::HANDLE_PRESENT);
    EXPECT_EQ(wire::load<u32>(span, 24), wire::HANDLE_PRESENT);

    auto const message = Message{.bytes = span, .handles = handles};
    EXPECT_EQ(wire::decode_handle(message, 16, 0).unwrap(), Uandle{0x23});
    EXPECT_EQ(wire::decode_handle(message, 24, 1).unwrap(), Uandle{0x33});

    // The field must say a handle is there, and the handle must be.
    EXPECT_TRUE(wire::decode_handle(message, 20, 0).is_err());
    EXPECT_TRUE(
        wire::decode_handle(Message{.bytes = span, .handles = std::span{handles}.first(1)}, 24, 1)
            .is_err());
    std::array const invalid{Uandle::Invalid};
    EXPECT_TRUE(wire::decode_handle(Message{.bytes = span, .handles = invalid}, 16, 0).is_err());

    // Every handle field is required, and the handle array must have room.
    EXPECT_EQ(wire::encode_handle(span, 16, handles, 0, Uandle::Invalid), Status::InvArgs);
    EXPECT_EQ(wire::encode_handle(span, 16, handles, 2, Uandle{0x23}), Status::BufferTooSmall);
}
} // namespace
