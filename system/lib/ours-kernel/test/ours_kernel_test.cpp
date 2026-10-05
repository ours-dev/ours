/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The ours.kernel.DebugResource codec (<ours_kernel/ours_kernel.hpp>):
// Get and its reply are encoded, decoded back, and then broken on purpose,
// one malformed form at a time.
#include <ours_kernel/ours_kernel.hpp>

#include <array>
#include <span>
#include <vector>

#include <gtest/gtest.h>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;
using ours_kernel::DebugResourceGetReply;
using ours_kernel::DebugResourceGetRequest;
using wire::Message;

constexpr auto FILL = u8{0xCC};
constexpr auto RESOURCE = Uandle{0x1003};

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

TEST(OursKernelDebugResourceGet, RoundTrip) {
    Buffer buffer;
    auto const message = owned(
        ours_kernel::encode_debug_resource_get(buffer.bytes, DebugResourceGetRequest{.txid = 9}));
    EXPECT_EQ(message.bytes.size(), wire::HEADER_SIZE);
    EXPECT_TRUE(message.handles.empty());

    auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header->ordinal, ours_kernel::DEBUG_RESOURCE_GET.ordinal);

    auto const decoded = ours_kernel::decode_debug_resource_get(message.view());
    ASSERT_TRUE(decoded.is_ok());
    EXPECT_EQ(decoded->txid, 9u);
}

TEST(OursKernelDebugResourceGet, Refusals) {
    Buffer buffer;
    EXPECT_EQ(
        ours_kernel::encode_debug_resource_get(buffer.bytes, DebugResourceGetRequest{.txid = 0})
            .unwrap_err(),
        Status::InvArgs);
    EXPECT_EQ(ours_kernel::encode_debug_resource_get(std::span{buffer.bytes}.first(15),
                                                     DebugResourceGetRequest{.txid = 9})
                  .unwrap_err(),
              Status::BufferTooSmall);

    auto const good = owned(
        ours_kernel::encode_debug_resource_get(buffer.bytes, DebugResourceGetRequest{.txid = 9}));
    auto refused = [](Message const message) {
        auto const decoded = ours_kernel::decode_debug_resource_get(message);
        return decoded.is_err() && decoded.unwrap_err() == Status::InvArgs;
    };
    // Exactly the header, no handles, a txid, this method, toward the server.
    auto longer = good;
    longer.bytes.push_back(0);
    EXPECT_TRUE(refused(longer.view()));
    EXPECT_TRUE(refused(good.cut(15)));
    auto with_handle = good;
    with_handle.handles.push_back(RESOURCE);
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

TEST(OursKernelDebugResourceGetReply, RoundTrip) {
    Buffer buffer;
    auto const ok = owned(ours_kernel::encode_debug_resource_get_reply(
        buffer.bytes, buffer.handles, 9,
        DebugResourceGetReply{.status = Status::Ok, .resource = RESOURCE}));
    EXPECT_EQ(ok.bytes.size(), wire::STATUS_FORM_SIZE);
    ASSERT_EQ(ok.handles.size(), 1u);
    EXPECT_EQ(wire::load<u32>(ok.bytes, 20), wire::HANDLE_PRESENT);
    auto const decoded_ok = ours_kernel::decode_debug_resource_get_reply(ok.view(), 9);
    ASSERT_TRUE(decoded_ok.is_ok());
    EXPECT_EQ(decoded_ok->status, Status::Ok);
    EXPECT_EQ(decoded_ok->resource, RESOURCE);

    Buffer failed_buffer;
    auto const failed = owned(ours_kernel::encode_debug_resource_get_reply(
        failed_buffer.bytes, failed_buffer.handles, 9,
        DebugResourceGetReply{.status = Status::NoResource}));
    EXPECT_TRUE(failed.handles.empty());
    auto const decoded_failed = ours_kernel::decode_debug_resource_get_reply(failed.view(), 9);
    ASSERT_TRUE(decoded_failed.is_ok());
    EXPECT_EQ(decoded_failed->status, Status::NoResource);
    EXPECT_EQ(decoded_failed->resource, Uandle::Invalid);
}

TEST(OursKernelDebugResourceGetReply, Refusals) {
    Buffer buffer;
    EXPECT_EQ(ours_kernel::encode_debug_resource_get_reply(
                  buffer.bytes, buffer.handles, 9,
                  DebugResourceGetReply{.status = Status::NoResource, .resource = RESOURCE})
                  .unwrap_err(),
              Status::InvArgs);
    EXPECT_EQ(ours_kernel::encode_debug_resource_get_reply(
                  buffer.bytes, buffer.handles, 9, DebugResourceGetReply{.status = Status::Ok})
                  .unwrap_err(),
              Status::InvArgs);

    auto const good = owned(ours_kernel::encode_debug_resource_get_reply(
        buffer.bytes, buffer.handles, 9,
        DebugResourceGetReply{.status = Status::Ok, .resource = RESOURCE}));
    auto refused = [](Message const message, u32 const txid) {
        auto const decoded = ours_kernel::decode_debug_resource_get_reply(message, txid);
        return decoded.is_err() && decoded.unwrap_err() == Status::InvArgs;
    };
    EXPECT_TRUE(refused(good.view(), 8));
    auto no_handle = good;
    no_handle.handles.clear();
    EXPECT_TRUE(refused(no_handle.view(), 9));
    auto absent = good;
    absent.set_u32(20, 0);
    EXPECT_TRUE(refused(absent.view(), 9));
    auto invalid = good;
    invalid.handles[0] = Uandle::Invalid;
    EXPECT_TRUE(refused(invalid.view(), 9));
    EXPECT_TRUE(refused(good.cut(23), 9));
    auto failure_with_resource = good;
    failure_with_resource.set_u32(16, static_cast<u32>(Status::NoResource));
    EXPECT_TRUE(refused(failure_with_resource.view(), 9));
}

TEST(OursKernel, Method) {
    EXPECT_EQ(ours_kernel::DEBUG_RESOURCE_GET.kind, wire::MethodKind::TwoWay);
    EXPECT_EQ(ours_kernel::DEBUG_RESOURCE_NAME, "ours.kernel.DebugResource");
}
} // namespace
