/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// rpc::Channel against the shared fake kernel: the call pipeline's own
// behaviors. The test plays the provider inside the fixture's pump -- when
// the client's wait asks the machine to progress, the pump reads the
// request off the server end and answers per the mode. What the cases pin:
// the round trip (the txid the caller sent comes back, the reply's bytes
// are the caller's to read), the epitaph a provider closes with standing
// in for the failure, the peer gone without an epitaph reading PeerClosed,
// the txid wrapping inside the protocol's bound, and a reply's stray
// handle closed by the pipeline rather than leaked.
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "fake_kernel.hpp"
#include <gtest/gtest.h>
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
// declarator, not the trailing type (the extern "C" spelling would land a
// platform underscore the label's reference never carries).
ours::Status ours_object_wait_one(ours::Uandle handle, ours::Signals signals,
                                  ours::Instant deadline,
                                  ours::Signals *observed) __asm__("ours_object_wait_one");
ours::Status ours_object_wait_one(ours::Uandle handle, ours::Signals signals,
                                  ours::Instant deadline, ours::Signals *observed) {
    for (auto spin = u32{0}; spin < 1'000'000; ++spin) {
        WAIT_PUMP();
        // A queued message wins over the peer's death: a survivor holding a
        // last message reads it first, the epitaph case lives on this order.
        if (fake::kernel().queued_messages(handle) != 0 &&
            (signals & ours::Signals::Readable) != ours::Signals::None) {
            *observed = ours::Signals::Readable;
            return ours::Status::Ok;
        }
        if (fake::kernel().peer_closed(handle)) {
            *observed = ours::Signals::PeerClosed;
            return ours::Status::Ok;
        }
    }
    *observed = ours::Signals::None;
    return ours::Status::TimeOut;
}

namespace {
using ours::Status;
using ours::Uandle;

/// One test's provider: the pump reads the request off the server end and
/// answers per the mode. Reply carries a stray handle the client should
/// close; Epitaph closes the connection with the epitaph after answering;
/// Silent closes without one.
enum class Mode {
    Reply,
    ReplyWithStrayHandle,
    Epitaph,
    Silent,
};

/// A channel pair, the client end first.
auto channel() -> std::array<Uandle, 2> {
    auto client = Uandle::Invalid;
    auto server = Uandle::Invalid;
    EXPECT_EQ(ours::channel_create(ours::ChannelCreateOptions::None, &client, &server), Status::Ok);
    return {client, server};
}

/// The request a provider reads: its txid, from the wire header.
auto request_txid(std::span<u8 const> const bytes) -> u32 {
    auto const header = wire::decode_header(bytes, wire::Direction::ToServer);
    EXPECT_TRUE(header.is_ok());
    return header->txid;
}

/// Builds a status-form reply to |txid|.
auto reply_bytes(u32 const txid) -> std::array<u8, wire::STATUS_FORM_SIZE> {
    auto bytes = std::array<u8, wire::STATUS_FORM_SIZE>{};
    wire::store(bytes, 0,
                wire::Header{.txid = txid,
                             .direction = wire::Direction::ToClient,
                             .reserved = {},
                             .magic = wire::MAGIC,
                             .ordinal = 7 /* any non-epitaph ordinal */});
    wire::store(bytes, wire::STATUS_OFFSET, Status::Ok);
    return bytes;
}

/// The fixture: a fresh fake kernel, a pair, the pump pointed at the mode.
class RpcTest: public testing::Test {
  protected:
    RpcTest() {
        fake::kernel().reset();
        ends_ = channel();
        WAIT_PUMP = [this] { pump(); };
    }

    ~RpcTest() override {
        WAIT_PUMP = [] {};
    }

    /// One pump step: answer whatever is queued on the server end, per the
    /// mode. The client's call() spins this while it waits.
    auto pump() -> void {
        if (answered_) {
            return;
        }
        if (fake::kernel().queued_messages(ends_[1]) == 0) {
            return;
        }
        answered_ = true;
        auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
        auto handles = std::array<Uandle, wire::MAX_MESSAGE_HANDLES>{};
        auto num_bytes = u32{0};
        auto num_handles = u32{0};
        EXPECT_EQ(ours::channel_read(ends_[1], ours::ChannelReadOptions::None, bytes.data(),
                                     handles.data(), static_cast<u32>(bytes.size()),
                                     static_cast<u32>(handles.size()), &num_bytes, &num_handles),
                  Status::Ok);
        auto const message =
            wire::Message{.bytes = std::span{bytes}.first(num_bytes), .handles = {}};
        auto const txid = request_txid(message.bytes);

        switch (mode_) {
            case Mode::Reply:
            case Mode::ReplyWithStrayHandle: {
                auto const reply = reply_bytes(txid);
                if (mode_ == Mode::ReplyWithStrayHandle) {
                    // A stray handle rides the reply: the pipeline's to close.
                    // The message carries the client end; the server end stays
                    // here, its peer gone once the client closes its copy.
                    auto const stray = channel();
                    stray_ = stray[0];
                    auto stray_span = std::array{stray[0]};
                    auto const written =
                        ours::channel_write(ends_[1], ours::ChannelWriteOptions::None, reply.data(),
                                            static_cast<u32>(reply.size()), stray_span.data(), 1);
                    EXPECT_EQ(written, Status::Ok);
                    return;
                }
                EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None,
                                              reply.data(), static_cast<u32>(reply.size()), nullptr,
                                              0),
                          Status::Ok);
                return;
            }
            case Mode::Epitaph: {
                auto epitaph = wire::encode_epitaph(reply_scratch_, Status::NotFound);
                EXPECT_TRUE(epitaph.is_ok());
                EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None,
                                              epitaph->bytes.data(),
                                              static_cast<u32>(epitaph->bytes.size()), nullptr, 0),
                          Status::Ok);
                EXPECT_EQ(ours::handle_close(ends_[1]), Status::Ok);
                return;
            }
            case Mode::Silent:
                // No answer: just the peer's death.
                EXPECT_EQ(ours::handle_close(ends_[1]), Status::Ok);
                return;
        }
    }

    auto run_call(rpc::Channel &client, wire::Message const request)
        -> ustl::Result<wire::Message, Status> {
        return client.call(request);
    }

    std::array<Uandle, 2> ends_{};
    Mode mode_ = Mode::Reply;
    bool answered_ = false;
    Uandle stray_ = Uandle::Invalid;
    std::array<u8, wire::MAX_MESSAGE_BYTES> reply_scratch_{};
};

} // namespace

/// The round trip: the reply the caller reads is the provider's answer to
/// the request's own txid.
TEST_F(RpcTest, RoundTrip) {
    auto client = rpc::Channel{ends_[0]};
    auto const txid = client.next_txid();
    auto bytes = std::array<u8, wire::STATUS_FORM_SIZE>{};
    wire::store(bytes, 0,
                wire::Header{.txid = txid,
                             .direction = wire::Direction::ToServer,
                             .reserved = {},
                             .magic = wire::MAGIC,
                             .ordinal = 7});
    auto const answered = client.call(wire::Message{.bytes = bytes, .handles = {}});
    ASSERT_TRUE(answered.is_ok());
    auto const header = wire::decode_header(answered->bytes, wire::Direction::ToClient);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header->txid, txid);
}

/// The epitaph a provider closes with stands in for the failure.
TEST_F(RpcTest, EpitaphBecomesTheFailure) {
    mode_ = Mode::Epitaph;
    auto client = rpc::Channel{ends_[0]};
    auto bytes = std::array<u8, wire::STATUS_FORM_SIZE>{};
    wire::store(bytes, 0,
                wire::Header{.txid = client.next_txid(),
                             .direction = wire::Direction::ToServer,
                             .reserved = {},
                             .magic = wire::MAGIC,
                             .ordinal = 7});
    EXPECT_EQ(client.call(wire::Message{.bytes = bytes, .handles = {}}).unwrap_err(),
              Status::NotFound);
}

/// A peer gone without an epitaph reads PeerClosed.
TEST_F(RpcTest, SilentPeerReadsPeerClosed) {
    mode_ = Mode::Silent;
    auto client = rpc::Channel{ends_[0]};
    auto bytes = std::array<u8, wire::STATUS_FORM_SIZE>{};
    wire::store(bytes, 0,
                wire::Header{.txid = client.next_txid(),
                             .direction = wire::Direction::ToServer,
                             .reserved = {},
                             .magic = wire::MAGIC,
                             .ordinal = 7});
    EXPECT_EQ(client.call(wire::Message{.bytes = bytes, .handles = {}}).unwrap_err(),
              Status::PeerClosed);
}

/// The txid wraps inside the protocol's bound instead of reaching zero.
TEST_F(RpcTest, TxidWraps) {
    auto client = rpc::Channel{ends_[0]};
    for (auto index = u32{0}; index < wire::MAX_TXID; ++index) {
        client.next_txid();
    }
    // MAX_TXID calls spent the bound: the next is 1 again, never zero.
    EXPECT_EQ(client.next_txid(), 1u);
}

/// A reply's stray handle is closed by the pipeline: the caller never sees
/// it, and the kernel's copy is gone.
TEST_F(RpcTest, StrayHandleClosed) {
    mode_ = Mode::ReplyWithStrayHandle;
    auto client = rpc::Channel{ends_[0]};
    auto bytes = std::array<u8, wire::STATUS_FORM_SIZE>{};
    wire::store(bytes, 0,
                wire::Header{.txid = client.next_txid(),
                             .direction = wire::Direction::ToServer,
                             .reserved = {},
                             .magic = wire::MAGIC,
                             .ordinal = 7});
    auto const answered = client.call(wire::Message{.bytes = bytes, .handles = {}});
    ASSERT_TRUE(answered.is_ok());
    EXPECT_FALSE(fake::kernel().is_open(stray_));
}
