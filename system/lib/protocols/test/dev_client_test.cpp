/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// prot::dev::Client against a fake registry, played in the pump: the
// client's own contract, the part the registry suite reaches only through
// the real registry. What the cases pin: the granted name round trip into
// the caller's span (NUL-terminated), the decode refusals -- an Ok reply
// naming nothing, a granted name past the bound, the wrong txid -- the
// granted span refusing a name and its NUL that do not fit, and the
// publish's own codec refusal answered before any byte reaches the wire.
#include <prot/dev_client.hpp>

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <cstring>

#include "fake_kernel.hpp"
#include <gtest/gtest.h>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
#include <rpc/rpc.hpp>
#include <wire/wire.hpp>

namespace ours {
// NOLINTNEXTLINE(readability-identifier-naming): gtest's customisation point.
auto PrintTo(Status const status, std::ostream *const out) -> void {
    *out << to_string(status);
}
} // namespace ours

// The wait pumps the machine while it waits: the fake kernel's progress is
// this hook, which the fixture points at the provider's answering logic.
static std::function<void()> WAIT_PUMP = [] {};

// The vDSO declaration carries the asm label; the label rides the
// declarator, not the trailing type.
ours::Status ours_object_wait_one(ours::Uandle handle, ours::Signals signals,
                                  ours::Instant deadline,
                                  ours::Signals *observed) __asm__("ours_object_wait_one");
ours::Status ours_object_wait_one(ours::Uandle handle, ours::Signals signals,
                                  ours::Instant deadline, ours::Signals *observed) {
    for (auto spin = u32{0}; spin < 1'000'000; ++spin) {
        WAIT_PUMP();
        if (fake::kernel().peer_closed(handle)) {
            *observed = ours::Signals::PeerClosed;
            return ours::Status::Ok;
        }
        if (fake::kernel().queued_messages(handle) != 0 &&
            (signals & ours::Signals::Readable) != ours::Signals::None) {
            *observed = ours::Signals::Readable;
            return ours::Status::Ok;
        }
    }
    *observed = ours::Signals::None;
    return ours::Status::TimeOut;
}

namespace {
using ours::Status;
using ours::Uandle;

constexpr auto TXID = u32{7};
constexpr auto DIRECTORY = Uandle{0xDEAD'0000};

/// A channel pair, the client end first.
auto channel() -> std::array<Uandle, 2> {
    auto client = Uandle::Invalid;
    auto server = Uandle::Invalid;
    EXPECT_EQ(ours::channel_create(ours::ChannelCreateOptions::None, &client, &server), Status::Ok);
    return {client, server};
}

/// Builds a Publish reply whose body the caller controls: an Ok reply with
/// the granted name's size and bytes, or a malformed one -- an Ok status
/// naming nothing.
auto publish_reply(std::span<u8> out, u32 const txid, bool const malformed,
                   std::string_view const name) -> usize {
    auto started = wire::start_ok_reply(out, 24 + name.size(), prot::dev::PUBLISH, txid);
    EXPECT_TRUE(started.is_ok());
    if (malformed) {
        wire::store(out.first(24), 20, u32{0});
        return 24;
    }
    wire::store(out.first(24), 20, static_cast<u32>(name.size()));
    std::memcpy(out.data() + 24, name.data(), name.size());
    return 24 + name.size();
}

/// The fixture: a fresh fake kernel, a pair, the pump answering Publish
/// per the mode.
class DevClientTest: public testing::Test {
  protected:
    DevClientTest() {
        fake::kernel().reset();
        ends_ = channel();
        WAIT_PUMP = [this] { pump(); };
    }

    ~DevClientTest() override {
        WAIT_PUMP = [] {};
    }

    auto pump() -> void {
        if (!fake::kernel().is_open(ends_[1]) || fake::kernel().queued_messages(ends_[1]) == 0) {
            return;
        }
        auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
        auto handles = std::array<Uandle, 4>{};
        auto num_bytes = u32{0};
        auto num_handles = u32{0};
        EXPECT_EQ(ours::channel_read(ends_[1], ours::ChannelReadOptions::None, bytes.data(),
                                     handles.data(), static_cast<u32>(bytes.size()),
                                     static_cast<u32>(handles.size()), &num_bytes, &num_handles),
                  Status::Ok);
        // The publish's directory handle arrived with the request: the
        // pump is its last owner now.
        for (auto index = u32{0}; index < num_handles; ++index) {
            auto _ = ours::handle_close(handles[index]);
        }
        auto const header =
            wire::decode_header(std::span{bytes}.first(num_bytes), wire::Direction::ToServer);
        ASSERT_TRUE(header.is_ok());
        // The un-publish's reply is the status form: Ok from this provider.
        if (header->ordinal == prot::dev::UNPUBLISH.ordinal) {
            if (garbage_unpublish_) {
                auto garbage = std::array<u8, 32>{};
                garbage.fill(0x5A);
                EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None,
                                              garbage.data(), static_cast<u32>(garbage.size()),
                                              nullptr, 0),
                          Status::Ok);
                return;
            }
            auto const reply = prot::dev::encode_unpublish_reply(reply_, header->txid,
                                                                 {.status = unpublish_status_});
            ASSERT_TRUE(reply.is_ok());
            EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None,
                                          reply->bytes.data(),
                                          static_cast<u32>(reply->bytes.size()), nullptr, 0),
                      Status::Ok);
            return;
        }
        auto const length = publish_status_ == Status::Ok
                                ? publish_reply(reply_, header->txid, malformed_, granted_name_)
                                : (wire::encode_status_reply(reply_, prot::dev::PUBLISH,
                                                             header->txid, publish_status_)
                                       .unwrap()
                                       .bytes.size());
        EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None, reply_.data(),
                                      static_cast<u32>(length), nullptr, 0),
                  Status::Ok);
    }

    std::array<Uandle, 2> ends_{};
    bool malformed_ = false;
    bool garbage_unpublish_ = false;
    Status publish_status_ = Status::Ok;
    Status unpublish_status_ = Status::Ok;
    std::string_view granted_name_ = "000";
    std::array<u8, wire::MAX_MESSAGE_BYTES> reply_{};
};

} // namespace

/// The granted name lands in the caller's span, NUL-terminated.
TEST_F(DevClientTest, GrantedNameLandsInSpan) {
    auto client = prot::dev::Client{ends_[0]};
    auto granted = std::array<char, 8>{};
    EXPECT_EQ(client.publish("block", "000", "ours.block.Block", DIRECTORY, granted), Status::Ok);
    EXPECT_STREQ(granted.data(), "000");
}

/// An Ok reply naming nothing is malformed: InvArgs.
TEST_F(DevClientTest, OkReplyWithoutNameReadsInvArgs) {
    malformed_ = true;
    auto client = prot::dev::Client{ends_[0]};
    auto granted = std::array<char, 8>{};
    EXPECT_EQ(client.publish("block", "000", "ours.block.Block", DIRECTORY, granted),
              Status::InvArgs);
}

/// A granted name past the bound is malformed: the provider could only
/// send it by breaking its own codec, and the reply is built by hand here
/// to prove the decode refuses it.
TEST_F(DevClientTest, OverlongGrantedNameReadsInvArgs) {
    auto over = std::string(prot::dev::MAX_NODE_NAME + 1, 'x');
    auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
    auto started = wire::start_ok_reply(bytes, 24 + over.size(), prot::dev::PUBLISH, TXID);
    ASSERT_TRUE(started.is_ok());
    wire::store(std::span{bytes}.first(24), 20, static_cast<u32>(over.size()));
    std::memcpy(bytes.data() + 24, over.data(), over.size());
    EXPECT_EQ(
        (prot::dev::decode_publish_reply(
             wire::Message{.bytes = std::span{bytes}.first(24 + over.size()), .handles = {}}, TXID)
             .unwrap_err()),
        Status::InvArgs);
}

/// A granted name whose span cannot hold it and its NUL: BufferTooSmall.
TEST_F(DevClientTest, TightSpanReadsBufferTooSmall) {
    auto client = prot::dev::Client{ends_[0]};
    auto granted = std::array<char, 3>{};
    EXPECT_EQ(client.publish("block", "000", "ours.block.Block", DIRECTORY, granted),
              Status::BufferTooSmall);
}

/// The publish's own codec refusal: an oversized protocol name is refused
/// before any byte reaches the wire.
TEST_F(DevClientTest, OversizedProtocolRefusedBeforeWire) {
    auto client = prot::dev::Client{ends_[0]};
    auto granted = std::array<char, 8>{};
    auto protocol = std::string(prot::dev::MAX_PROTOCOL_NAME + 1, 'p');
    EXPECT_EQ(client.publish("block", "000", protocol, DIRECTORY, granted), Status::InvArgs);
    EXPECT_EQ(fake::kernel().queued_messages(ends_[1]), 0U);
}

/// The typed un-publish: Ok, NotFound, and the dead connection's
/// PeerClosed, through the same channel the publish used.
TEST_F(DevClientTest, UnpublishRoundTrip) {
    auto client = prot::dev::Client{ends_[0]};
    EXPECT_EQ(client.unpublish("block", "000"), Status::Ok);
}

TEST_F(DevClientTest, UnpublishUnknownNameReadsNotFound) {
    unpublish_status_ = Status::NotFound;
    auto client = prot::dev::Client{ends_[0]};
    EXPECT_EQ(client.unpublish("block", "042"), Status::NotFound);
}

TEST_F(DevClientTest, UnpublishOnDeadConnectionReadsPeerClosed) {
    auto client = prot::dev::Client{ends_[0]};
    // The provider goes away between two calls: the second reads the peer
    // gone, whatever the first answered.
    EXPECT_EQ(client.unpublish("block", "000"), Status::Ok);
    EXPECT_EQ(ours::handle_close(ends_[1]), Status::Ok);
    EXPECT_EQ(client.unpublish("block", "000"), Status::PeerClosed);
}

/// Publish through a provider that answers a failure: the status arrives
/// as itself.
TEST_F(DevClientTest, PublishFailureStatusArrivesAsItself) {
    publish_status_ = Status::NoResource;
    auto client = prot::dev::Client{ends_[0]};
    auto granted = std::array<char, 8>{};
    EXPECT_EQ(client.publish("block", "000", "ours.block.Block", DIRECTORY, granted),
              Status::NoResource);
}

/// Publish through a provider that went away between two calls: the dead
/// connection's PeerClosed.
TEST_F(DevClientTest, PublishOnDeadConnectionReadsPeerClosed) {
    auto client = prot::dev::Client{ends_[0]};
    auto granted = std::array<char, 8>{};
    EXPECT_EQ(client.publish("block", "000", "ours.block.Block", DIRECTORY, granted), Status::Ok);
    EXPECT_EQ(ours::handle_close(ends_[1]), Status::Ok);
    EXPECT_EQ(client.publish("block", "001", "ours.block.Block", DIRECTORY, granted),
              Status::PeerClosed);
}

/// The un-publish's own encode refusal: a kind that names a path is
/// refused before any byte reaches the wire.
TEST_F(DevClientTest, UnpublishPathNamingKindRefusedBeforeWire) {
    auto client = prot::dev::Client{ends_[0]};
    EXPECT_EQ(client.unpublish("blo/ck", "000"), Status::InvArgs);
    EXPECT_EQ(fake::kernel().queued_messages(ends_[1]), 0U);
}

/// The un-publish's reply that is not a message at all: the pipeline's own
/// header check refuses it before the codec runs, and the client reads the
/// pipeline's BadState.
TEST_F(DevClientTest, UnpublishGarbageReplyReadsBadState) {
    garbage_unpublish_ = true;
    auto client = prot::dev::Client{ends_[0]};
    EXPECT_EQ(client.unpublish("block", "000"), Status::BadState);
}
