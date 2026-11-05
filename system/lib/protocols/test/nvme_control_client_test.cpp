/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// prot::nvme::Client against a fake control, played in the pump: the
// client's own contract. What the cases pin: an Ok rescan reading Ok, an
// unchanged machine's NotFound reading as itself, the epitaph a closed
// route carries, a garbage reply reading InvArgs, a reply to another
// request's txid refused, and the silent close reading PeerClosed.
#include <prot/nvme_control_client.hpp>

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

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

/// A channel pair, the client end first.
auto channel() -> std::array<Uandle, 2> {
    auto client = Uandle::Invalid;
    auto server = Uandle::Invalid;
    EXPECT_EQ(ours::channel_create(ours::ChannelCreateOptions::None, &client, &server), Status::Ok);
    return {client, server};
}

/// The fixture: a fresh fake kernel, a pair, the pump answering the rescan
/// per the mode. |silent_| closes the provider end without answering, and
/// |garbage_| answers with bytes that are no reply of this method.
class NvmeControlClientTest: public testing::Test {
  protected:
    NvmeControlClientTest() {
        fake::kernel().reset();
        ends_ = channel();
        WAIT_PUMP = [this] { pump(); };
    }

    ~NvmeControlClientTest() override {
        WAIT_PUMP = [] {};
    }

    auto pump() -> void {
        if (silent_) {
            auto _ = ours::handle_close(ends_[1]);
            ends_[1] = Uandle::Invalid;
            return;
        }
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
        auto const header =
            wire::decode_header(std::span{bytes}.first(num_bytes), wire::Direction::ToServer);
        ASSERT_TRUE(header.is_ok());
        if (epitaph_) {
            // A route that leads nowhere closes the connection with the
            // refusal queued before the close: the client reads its status.
            auto const epitaph = wire::encode_epitaph(reply_, epitaph_status_);
            ASSERT_TRUE(epitaph.is_ok());
            EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None,
                                          epitaph->bytes.data(),
                                          static_cast<u32>(epitaph->bytes.size()), nullptr, 0),
                      Status::Ok);
            auto _ = ours::handle_close(ends_[1]);
            ends_[1] = Uandle::Invalid;
            return;
        }
        if (garbage_) {
            auto garbage = std::array<u8, 32>{};
            garbage.fill(0x5A);
            EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None, garbage.data(),
                                          static_cast<u32>(garbage.size()), nullptr, 0),
                      Status::Ok);
            return;
        }
        // A reply to another request's txid: well-formed, so the plumbing
        // hands it over, and the client's decode refuses it.
        auto const txid = stale_txid_ ? header->txid + 1 : header->txid;
        auto const reply =
            prot::nvme::encode_rescan_reply(reply_, txid, {.status = rescan_status_});
        ASSERT_TRUE(reply.is_ok());
        EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None,
                                      reply->bytes.data(), static_cast<u32>(reply->bytes.size()),
                                      nullptr, 0),
                  Status::Ok);
    }

    std::array<Uandle, 2> ends_{};
    Status rescan_status_ = Status::Ok;
    Status epitaph_status_ = Status::AccessDenied;
    bool garbage_ = false;
    bool stale_txid_ = false;
    bool silent_ = false;
    bool epitaph_ = false;
    std::array<u8, wire::STATUS_FORM_SIZE> reply_{};
};

TEST_F(NvmeControlClientTest, RescanAnsweredOk) {
    auto client = prot::nvme::Client{ends_[0]};
    EXPECT_EQ(client.rescan(), Status::Ok);
}

TEST_F(NvmeControlClientTest, UnchangedMachineReadsNotFound) {
    rescan_status_ = Status::NotFound;
    auto client = prot::nvme::Client{ends_[0]};
    EXPECT_EQ(client.rescan(), Status::NotFound);
}

TEST_F(NvmeControlClientTest, GarbageReplyReadsBadState) {
    // Bytes that are no message at all are refused by the plumbing, before
    // any decode of this family runs.
    garbage_ = true;
    auto client = prot::nvme::Client{ends_[0]};
    EXPECT_EQ(client.rescan(), Status::BadState);
}

TEST_F(NvmeControlClientTest, ReplyToAnotherTxidReadsInvArgs) {
    stale_txid_ = true;
    auto client = prot::nvme::Client{ends_[0]};
    EXPECT_EQ(client.rescan(), Status::InvArgs);
}

TEST_F(NvmeControlClientTest, SilentCloseReadsPeerClosed) {
    silent_ = true;
    auto client = prot::nvme::Client{ends_[0]};
    EXPECT_EQ(client.rescan(), Status::PeerClosed);
}

TEST_F(NvmeControlClientTest, EpitaphReadsItsStatus) {
    epitaph_ = true;
    auto client = prot::nvme::Client{ends_[0]};
    EXPECT_EQ(client.rescan(), Status::AccessDenied);
}
} // namespace
