/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The ours.examples.echo codec (<ours_examples_echo/ours_examples_echo.hpp>):
// EchoString and Stop and their replies are encoded, decoded back, and then
// broken on purpose, one malformed form at a time.
#include <ours_examples_echo/ours_examples_echo.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;
using ours_examples_echo::EchoStringReply;
using ours_examples_echo::EchoStringRequest;
using ours_examples_echo::StopRequest;
using wire::Message;

constexpr auto FILL = u8{0xCC};
constexpr auto STRAY = Uandle{0x1003};

/// Room for one message, starting dirty so that a byte the encoder forgot
/// shows.
struct Buffer {
    std::vector<u8> bytes = std::vector<u8>(wire::MAX_MESSAGE_BYTES, FILL);
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

    auto set_u32(usize const offset, u32 const value) -> void {
        wire::store(std::span{bytes}, offset, value);
    }
};

auto owned(ustl::Result<Message, Status> const &encoded) -> Owned {
    EXPECT_TRUE(encoded.is_ok());
    return Owned(encoded.unwrap());
}

/// The longest string the protocol carries, and one byte more.
auto const LONGEST = std::string(ours_examples_echo::MAX_STRING_LENGTH, 'e');
auto const TOO_LONG = std::string(ours_examples_echo::MAX_STRING_LENGTH + 1, 'e');

TEST(OursExamplesEchoEchoString, RoundTrip) {
    for (auto const value:
         {std::string_view{}, std::string_view{"hello"}, std::string_view{LONGEST}}) {
        Buffer buffer;
        auto const message = owned(ours_examples_echo::encode_echo_string(
            buffer.bytes, EchoStringRequest{.txid = 7, .value = value}));
        EXPECT_EQ(message.bytes.size(), 20 + value.size());
        EXPECT_EQ(wire::load<u32>(message.bytes, 16), value.size());
        auto const decoded = ours_examples_echo::decode_echo_string(message.view());
        ASSERT_TRUE(decoded.is_ok());
        EXPECT_EQ(decoded->txid, 7u);
        EXPECT_EQ(decoded->value, value);
    }
}

TEST(OursExamplesEchoEchoString, Refusals) {
    Buffer buffer;
    EXPECT_EQ(ours_examples_echo::encode_echo_string(buffer.bytes,
                                                     EchoStringRequest{.txid = 0, .value = "x"})
                  .unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(ours_examples_echo::encode_echo_string(
                  buffer.bytes, EchoStringRequest{.txid = 7, .value = TOO_LONG})
                  .unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(ours_examples_echo::encode_echo_string(std::span{buffer.bytes}.first(24),
                                                     EchoStringRequest{.txid = 7, .value = "hello"})
                  .unwrap_err(),
              Status::BufferTooSmall);

    auto const good = owned(ours_examples_echo::encode_echo_string(
        buffer.bytes, EchoStringRequest{.txid = 7, .value = "hello"}));
    auto refused = [](Message const message) {
        auto const decoded = ours_examples_echo::decode_echo_string(message);
        return decoded.is_err() && decoded.unwrap_err() == Status::InvArgs;
    };
    // The size must account for every byte after it, and no more.
    auto trailing = good;
    trailing.bytes.push_back('!');
    EXPECT_TRUE(refused(trailing.view()));
    auto short_of_size = good;
    short_of_size.set_u32(16, 6);
    EXPECT_TRUE(refused(short_of_size.view()));
    auto no_size = good;
    no_size.bytes.resize(18);
    EXPECT_TRUE(refused(no_size.view()));
    // Longer than MAX_STRING_LENGTH, even when the bytes are all there.
    auto long_bytes = owned(ours_examples_echo::encode_echo_string(
        buffer.bytes, EchoStringRequest{.txid = 7, .value = LONGEST}));
    long_bytes.bytes.push_back('e');
    long_bytes.set_u32(16, static_cast<u32>(TOO_LONG.size()));
    EXPECT_TRUE(refused(long_bytes.view()));
    auto with_handle = good;
    with_handle.handles.push_back(STRAY);
    EXPECT_TRUE(refused(with_handle.view()));
    auto one_way = good;
    one_way.set_u32(0, 0);
    EXPECT_TRUE(refused(one_way.view()));
    auto stop = good;
    wire::store(std::span{stop.bytes}, 8, ours_examples_echo::STOP.ordinal);
    EXPECT_TRUE(refused(stop.view()));
}

TEST(OursExamplesEchoEchoStringReply, RoundTrip) {
    Buffer buffer;
    auto const ok = owned(ours_examples_echo::encode_echo_string_reply(
        buffer.bytes, 7, EchoStringReply{.status = Status::Ok, .response = "hello"}));
    EXPECT_EQ(ok.bytes.size(), 24u + 5u);
    EXPECT_TRUE(ok.handles.empty());
    auto const decoded_ok = ours_examples_echo::decode_echo_string_reply(ok.view(), 7);
    ASSERT_TRUE(decoded_ok.is_ok());
    EXPECT_EQ(decoded_ok->status, Status::Ok);
    EXPECT_EQ(decoded_ok->response, "hello");

    Buffer failed_buffer;
    auto const failed = owned(ours_examples_echo::encode_echo_string_reply(
        failed_buffer.bytes, 7, EchoStringReply{.status = Status::NoResource}));
    EXPECT_EQ(failed.bytes.size(), wire::STATUS_FORM_SIZE);
    auto const decoded_failed = ours_examples_echo::decode_echo_string_reply(failed.view(), 7);
    ASSERT_TRUE(decoded_failed.is_ok());
    EXPECT_EQ(decoded_failed->status, Status::NoResource);
    EXPECT_TRUE(decoded_failed->response.empty());
}

TEST(OursExamplesEchoEchoStringReply, Refusals) {
    Buffer buffer;
    EXPECT_EQ(ours_examples_echo::encode_echo_string_reply(
                  buffer.bytes, 7, EchoStringReply{.status = Status::NoResource, .response = "x"})
                  .unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(ours_examples_echo::encode_echo_string_reply(
                  buffer.bytes, 7, EchoStringReply{.status = Status::Ok, .response = TOO_LONG})
                  .unwrap_err(),
              Status::InvArgs);

    auto const good = owned(ours_examples_echo::encode_echo_string_reply(
        buffer.bytes, 7, EchoStringReply{.status = Status::Ok, .response = "hello"}));
    auto refused = [](Message const message, u32 const txid) {
        auto const decoded = ours_examples_echo::decode_echo_string_reply(message, txid);
        return decoded.is_err() && decoded.unwrap_err() == Status::InvArgs;
    };
    EXPECT_TRUE(refused(good.view(), 8));
    auto trailing = good;
    trailing.bytes.push_back('!');
    EXPECT_TRUE(refused(trailing.view(), 7));
    auto with_handle = good;
    with_handle.handles.push_back(STRAY);
    EXPECT_TRUE(refused(with_handle.view(), 7));
    auto request = good;
    request.bytes[4] = static_cast<u8>(wire::Direction::ToServer);
    EXPECT_TRUE(refused(request.view(), 7));
}

TEST(OursExamplesEchoStop, RoundTrip) {
    Buffer buffer;
    auto const message =
        owned(ours_examples_echo::encode_stop(buffer.bytes, StopRequest{.txid = 3}));
    EXPECT_EQ(message.bytes.size(), wire::HEADER_SIZE);
    auto const decoded = ours_examples_echo::decode_stop(message.view());
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->txid, 3u);

    Buffer reply_buffer;
    auto const reply =
        owned(ours_examples_echo::encode_stop_reply(reply_buffer.bytes, 3, Status::Ok));
    EXPECT_EQ(reply.bytes.size(), wire::STATUS_FORM_SIZE);
    auto const decoded_reply = ours_examples_echo::decode_stop_reply(reply.view(), 3);
    ASSERT_TRUE(decoded_reply.is_ok());
    EXPECT_EQ(decoded_reply.unwrap(), Status::Ok);
}

TEST(OursExamplesEchoStop, Refusals) {
    Buffer buffer;
    auto const good = owned(ours_examples_echo::encode_stop(buffer.bytes, StopRequest{.txid = 3}));
    auto longer = good;
    longer.bytes.push_back(0);
    EXPECT_EQ(ours_examples_echo::decode_stop(longer.view()).unwrap_err(), Status::InvArgs);
    auto echo = good;
    wire::store(std::span{echo.bytes}, 8, ours_examples_echo::ECHO_STRING.ordinal);
    EXPECT_EQ(ours_examples_echo::decode_stop(echo.view()).unwrap_err(), Status::InvArgs);

    Buffer reply_buffer;
    auto const reply =
        owned(ours_examples_echo::encode_stop_reply(reply_buffer.bytes, 3, Status::Ok));
    auto dirty = reply;
    dirty.set_u32(20, 1);
    EXPECT_EQ(ours_examples_echo::decode_stop_reply(dirty.view(), 3).unwrap_err(), Status::InvArgs);
    auto with_handle = reply;
    with_handle.handles.push_back(STRAY);
    EXPECT_EQ(ours_examples_echo::decode_stop_reply(with_handle.view(), 3).unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(ours_examples_echo::decode_stop_reply(reply.view(), 4).unwrap_err(), Status::InvArgs);
}

TEST(OursExamplesEcho, Methods) {
    EXPECT_EQ(ours_examples_echo::ECHO_STRING.kind, wire::MethodKind::TwoWay);
    EXPECT_EQ(ours_examples_echo::STOP.kind, wire::MethodKind::TwoWay);
    EXPECT_NE(ours_examples_echo::ECHO_STRING.ordinal, ours_examples_echo::STOP.ordinal);
    EXPECT_EQ(ours_examples_echo::ECHO_NAME, "ours.examples.echo.Echo");
}
} // namespace
