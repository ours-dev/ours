/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The ours.resource codec (<ours_resource/ours_resource.hpp>): each
// protocol's Get and its reply are encoded, decoded back, and then broken on
// purpose, one malformed form at a time, and one protocol's messages are
// refused by another's decoders.
#include <ours_resource/ours_resource.hpp>

#include <array>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;
using ours_resource::GetReply;
using ours_resource::GetRequest;
using wire::Message;

constexpr auto FILL = u8{0xCC};
constexpr auto RESOURCE = Uandle{0x1003};

/// Every protocol of the library: each test runs on all of them.
constexpr auto PROTOCOLS = std::array{
    ours_resource::DEBUG_PROTOCOL, ours_resource::IRQ_PROTOCOL,   ours_resource::IOPORT_PROTOCOL,
    ours_resource::MMIO_PROTOCOL,  ours_resource::IOMMU_PROTOCOL,
};

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

TEST(OursResourceGet, RoundTrip) {
    for (auto const &protocol: PROTOCOLS) {
        SCOPED_TRACE(protocol.name);
        Buffer buffer;
        auto const message =
            owned(ours_resource::encode_get(buffer.bytes, protocol, GetRequest{.txid = 9}));
        EXPECT_EQ(message.bytes.size(), wire::HEADER_SIZE);
        EXPECT_TRUE(message.handles.empty());

        auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
        ASSERT_TRUE(header.is_ok());
        EXPECT_EQ(header->ordinal, protocol.get.ordinal);

        auto const decoded = ours_resource::decode_get(message.view(), protocol);
        ASSERT_TRUE(decoded.is_ok());
        EXPECT_EQ(decoded->txid, 9u);
    }
}

TEST(OursResourceGet, Refusals) {
    for (auto const &protocol: PROTOCOLS) {
        SCOPED_TRACE(protocol.name);
        Buffer buffer;
        EXPECT_EQ(
            ours_resource::encode_get(buffer.bytes, protocol, GetRequest{.txid = 0}).unwrap_err(),
            Status::InvArgs);
        EXPECT_EQ(ours_resource::encode_get(std::span{buffer.bytes}.first(15), protocol,
                                            GetRequest{.txid = 9})
                      .unwrap_err(),
                  Status::BufferTooSmall);

        auto const good =
            owned(ours_resource::encode_get(buffer.bytes, protocol, GetRequest{.txid = 9}));
        auto refused = [&protocol](Message const message) {
            auto const decoded = ours_resource::decode_get(message, protocol);
            return decoded.is_err() && decoded.unwrap_err() == Status::InvArgs;
        };
        // Exactly the header, no handles, a txid, this method, toward the
        // server.
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
}

TEST(OursResourceGetReply, RoundTrip) {
    for (auto const &protocol: PROTOCOLS) {
        SCOPED_TRACE(protocol.name);
        Buffer buffer;
        auto const ok = owned(
            ours_resource::encode_get_reply(buffer.bytes, buffer.handles, protocol, 9,
                                            GetReply{.status = Status::Ok, .resource = RESOURCE}));
        EXPECT_EQ(ok.bytes.size(), wire::STATUS_FORM_SIZE);
        ASSERT_EQ(ok.handles.size(), 1u);
        EXPECT_EQ(wire::load<u32>(ok.bytes, 20), wire::HANDLE_PRESENT);
        auto const decoded_ok = ours_resource::decode_get_reply(ok.view(), protocol, 9);
        ASSERT_TRUE(decoded_ok.is_ok());
        EXPECT_EQ(decoded_ok->status, Status::Ok);
        EXPECT_EQ(decoded_ok->resource, RESOURCE);

        Buffer failed_buffer;
        auto const failed = owned(
            ours_resource::encode_get_reply(failed_buffer.bytes, failed_buffer.handles, protocol, 9,
                                            GetReply{.status = Status::BadHandle}));
        EXPECT_TRUE(failed.handles.empty());
        auto const decoded_failed = ours_resource::decode_get_reply(failed.view(), protocol, 9);
        ASSERT_TRUE(decoded_failed.is_ok());
        EXPECT_EQ(decoded_failed->status, Status::BadHandle);
        EXPECT_EQ(decoded_failed->resource, Uandle::Invalid);
    }
}

TEST(OursResourceGetReply, Refusals) {
    for (auto const &protocol: PROTOCOLS) {
        SCOPED_TRACE(protocol.name);
        Buffer buffer;
        EXPECT_EQ(ours_resource::encode_get_reply(
                      buffer.bytes, buffer.handles, protocol, 9,
                      GetReply{.status = Status::NoResource, .resource = RESOURCE})
                      .unwrap_err(),
                  Status::InvArgs);
        EXPECT_EQ(ours_resource::encode_get_reply(buffer.bytes, buffer.handles, protocol, 9,
                                                  GetReply{.status = Status::Ok})
                      .unwrap_err(),
                  Status::InvArgs);

        auto const good = owned(
            ours_resource::encode_get_reply(buffer.bytes, buffer.handles, protocol, 9,
                                            GetReply{.status = Status::Ok, .resource = RESOURCE}));
        auto refused = [&protocol](Message const message, u32 const txid) {
            auto const decoded = ours_resource::decode_get_reply(message, protocol, txid);
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
}

// The three Gets are the same bytes but for the ordinal, so a connection
// speaks exactly one protocol: a server refuses another protocol's request,
// and a client another protocol's reply. Without this, a root that bound a
// connection to the wrong server would hand out the wrong resource.
TEST(OursResource, ProtocolsRefuseEachOther) {
    for (auto const &sent: PROTOCOLS) {
        for (auto const &expected: PROTOCOLS) {
            // Every pair of two protocols, told apart by name: two that
            // were declared with one ordinal would accept each other's Get,
            // which is the mistake this test is here to catch, so the
            // ordinal cannot be what a pair is skipped by.
            if (sent.name == expected.name) {
                continue;
            }
            SCOPED_TRACE(expected.name);
            SCOPED_TRACE(sent.name);
            Buffer request_buffer;
            auto const request =
                owned(ours_resource::encode_get(request_buffer.bytes, sent, GetRequest{.txid = 9}));
            auto const decoded = ours_resource::decode_get(request.view(), expected);
            ASSERT_TRUE(decoded.is_err());
            EXPECT_EQ(decoded.unwrap_err(), Status::InvArgs);

            Buffer reply_buffer;
            auto const reply = owned(ours_resource::encode_get_reply(
                reply_buffer.bytes, reply_buffer.handles, sent, 9,
                GetReply{.status = Status::Ok, .resource = RESOURCE}));
            auto const decoded_reply = ours_resource::decode_get_reply(reply.view(), expected, 9);
            ASSERT_TRUE(decoded_reply.is_err());
            EXPECT_EQ(decoded_reply.unwrap_err(), Status::InvArgs);
        }
    }
}

TEST(OursResource, Protocols) {
    // Each protocol's name, the one a component finds it under in its /svc,
    // and the selector its Get is hashed from spell the same library and
    // protocol.
    struct Expected {
        std::string_view name;
        std::string_view selector;
    };
    constexpr auto EXPECTED = std::array{
        Expected{.name = "ours.resource.Debug", .selector = "ours.resource/Debug.Get"},
        Expected{.name = "ours.resource.Irq", .selector = "ours.resource/Irq.Get"},
        Expected{.name = "ours.resource.Ioport", .selector = "ours.resource/Ioport.Get"},
        Expected{.name = "ours.resource.Mmio", .selector = "ours.resource/Mmio.Get"},
        Expected{.name = "ours.resource.Iommu", .selector = "ours.resource/Iommu.Get"},
    };
    static_assert(EXPECTED.size() == PROTOCOLS.size());
    for (auto index = usize{0}; index < PROTOCOLS.size(); ++index) {
        auto const &protocol = PROTOCOLS[index];
        EXPECT_EQ(protocol.name, EXPECTED[index].name);
        EXPECT_EQ(protocol.get.selector, EXPECTED[index].selector);
        EXPECT_EQ(protocol.get.kind, wire::MethodKind::TwoWay);
    }
}
} // namespace
