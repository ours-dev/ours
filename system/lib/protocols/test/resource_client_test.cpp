/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// prot::resource's client against a fake resource provider: the Get on a
// connection the caller keeps, and what each answer reads as. The test
// plays the provider inside the fixture's pump -- when the client's wait
// asks the machine to progress, the pump reads the request off the server
// end, decodes it with the codec's own decoders, and answers per the mode.
// What the cases pin: the resource handle coming back alive -- a fresh
// pair's end, still open for the caller -- the reply's failure status
// arriving as itself, the epitaph a provider closes with standing in for
// the failure (Ok reading as the connection closing), a malformed reply
// refused InvArgs with its stray handle closed, a reply to another
// protocol's Get caught the same way, and the peer gone without a word
// reading PeerClosed. The /svc overload is the namespace's to serve and has
// no host; the connection-taking get carries all of the conduct.
#include <prot/resource_client.hpp>

#include <array>
#include <functional>
#include <span>
#include <string_view>

#include "fake_kernel.hpp"
#include <fsio/fsio.hpp>
#include <gtest/gtest.h>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
#include <wire/wire.hpp>

// The /svc overload connects through the process's namespace, which does
// not exist on the host; no case here calls it. The definition keeps the
// link whole and answers what a namespace without the use would.
auto fsio::connect(std::string_view const, ours::Uandle const) -> ours::Status {
    return ours::Status::NotFound;
}

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

/// The protocol the cases ask their Get with, and the one whose reply the
/// mismatch case answers with: the facts the test and the pump share.
constexpr auto &ASKED = prot::resource::DEBUG_PROTOCOL;
constexpr auto &OTHER = prot::resource::IRQ_PROTOCOL;

/// One test's provider: the pump reads the request off the server end and
/// answers per the mode. Reply hands out one handle -- a fresh pair's end;
/// Failure answers the status form; StrayHandle rides a handle on a reply
/// the client cannot read; WrongProtocol answers with the other protocol's
/// Get, stray handle riding; Epitaph closes with the epitaph after reading
/// the request; Silent closes without answering.
enum class Mode {
    Reply,
    Failure,
    StrayHandle,
    WrongProtocol,
    Epitaph,
    OkEpitaph,
    Silent,
};

/// A channel pair, the client end first.
auto channel() -> std::array<Uandle, 2> {
    auto client = Uandle::Invalid;
    auto server = Uandle::Invalid;
    EXPECT_EQ(ours::channel_create(ours::ChannelCreateOptions::None, &client, &server), Status::Ok);
    return {client, server};
}

/// The fixture: a fresh fake kernel, a pair, the pump pointed at the mode.
class ResourceClient: public testing::Test {
  protected:
    ResourceClient() {
        fake::kernel().reset();
        ends_ = channel();
        WAIT_PUMP = [this] { pump(); };
    }

    ~ResourceClient() override {
        WAIT_PUMP = [] {};
    }

    /// One pump step: answer whatever is queued on the server end, per the
    /// mode. The client's calls spin this while they wait.
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
        auto const message = wire::Message{.bytes = std::span{bytes}.first(num_bytes),
                                           .handles = std::span{handles}.first(num_handles)};
        auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
        ASSERT_TRUE(header.is_ok());
        auto const txid = header->txid;

        switch (mode_) {
            case Mode::Reply: {
                // The resource is a fresh pair's end: the client's to keep.
                // The pair's server end stays here; the client's copy of the
                // end stays open past the Get, which is the point.
                auto const resource = channel();
                resource_ = resource[0];
                auto riding = std::array{resource[0]};
                auto const reply = prot::resource::encode_get_reply(
                    reply_scratch_, riding, ASKED, txid,
                    {.status = Status::Ok, .resource = resource[0]});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply);
                return;
            }
            case Mode::Failure: {
                // A root that does not hold the resource says so: the
                // status form, no handle.
                auto const reply = prot::resource::encode_get_reply(reply_scratch_, {}, ASKED, txid,
                                                                    {.status = Status::NotFound});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply);
                return;
            }
            case Mode::StrayHandle:
            case Mode::WrongProtocol: {
                // A reply the client cannot take: bytes it refuses, with a
                // stray handle riding for it to close. StrayHandle rides it
                // on garbage (the header's magic is wrong); WrongProtocol on
                // the other protocol's well-formed Get, whose ordinal is
                // what the client's decoder refuses.
                auto const stray = channel();
                stray_ = stray[0];
                auto riding = std::array{stray[0]};
                if (mode_ == Mode::WrongProtocol) {
                    auto const reply = prot::resource::encode_get_reply(
                        reply_scratch_, riding, OTHER, txid,
                        {.status = Status::Ok, .resource = stray[0]});
                    ASSERT_TRUE(reply.is_ok());
                    write_reply(*reply);
                    return;
                }
                auto garbage = std::array<u8, 32>{};
                garbage.fill(0x5A);
                EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None,
                                              garbage.data(), static_cast<u32>(garbage.size()),
                                              riding.data(), 1),
                          Status::Ok);
                return;
            }
            case Mode::Epitaph: {
                auto epitaph = wire::encode_epitaph(reply_scratch_, Status::NotFound);
                ASSERT_TRUE(epitaph.is_ok());
                EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None,
                                              epitaph->bytes.data(),
                                              static_cast<u32>(epitaph->bytes.size()), nullptr, 0),
                          Status::Ok);
                EXPECT_EQ(ours::handle_close(ends_[1]), Status::Ok);
                return;
            }
            case Mode::OkEpitaph: {
                auto epitaph = wire::encode_epitaph(reply_scratch_, Status::Ok);
                ASSERT_TRUE(epitaph.is_ok());
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

    /// Writes an encoded reply onto the server end, back to the client,
    /// handles riding it as its codec attached them.
    auto write_reply(wire::Message const &reply) -> void {
        if (reply.handles.empty()) {
            EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None,
                                          reply.bytes.data(), static_cast<u32>(reply.bytes.size()),
                                          nullptr, 0),
                      Status::Ok);
            return;
        }
        auto riding = std::array<Uandle, 1>{reply.handles.front()};
        EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None, reply.bytes.data(),
                                      static_cast<u32>(reply.bytes.size()), riding.data(), 1),
                  Status::Ok);
    }

    std::array<Uandle, 2> ends_{};
    Mode mode_ = Mode::Reply;
    bool answered_ = false;
    Uandle resource_ = Uandle::Invalid;
    Uandle stray_ = Uandle::Invalid;
    std::array<u8, wire::MAX_MESSAGE_BYTES> reply_scratch_{};
};

} // namespace

/// The round trip: the Get answered, and the resource handle coming back
/// alive -- it survived the call for the caller.
TEST_F(ResourceClient, GetHandsTheResourceOver) {
    auto const gotten = prot::resource::get(ends_[0], ASKED);
    ASSERT_TRUE(gotten.is_ok());
    EXPECT_EQ(gotten.unwrap(), resource_);
    EXPECT_TRUE(fake::kernel().is_open(resource_));
}

/// A reply whose status is a failure arrives as itself, no resource with it.
TEST_F(ResourceClient, FailureStatusArrivesAsItself) {
    mode_ = Mode::Failure;
    auto const gotten = prot::resource::get(ends_[0], ASKED);
    ASSERT_TRUE(gotten.is_err());
    EXPECT_EQ(gotten.unwrap_err(), Status::NotFound);
}

/// A reply that is not a message, a stray handle riding it: the handle is
/// closed, and the answer reads InvArgs rather than the reply.
TEST_F(ResourceClient, GarbageReplyReadsInvArgsAndClosesItsHandle) {
    mode_ = Mode::StrayHandle;
    auto const gotten = prot::resource::get(ends_[0], ASKED);
    ASSERT_TRUE(gotten.is_err());
    EXPECT_EQ(gotten.unwrap_err(), Status::InvArgs);
    EXPECT_FALSE(fake::kernel().is_open(stray_));
}

/// The other protocol's well-formed Get, stray handle riding: the ordinal
/// mismatch is what the client's decoder refuses, the handle closed with it.
TEST_F(ResourceClient, OtherProtocolsReplyReadsInvArgsAndClosesItsHandle) {
    mode_ = Mode::WrongProtocol;
    auto const gotten = prot::resource::get(ends_[0], ASKED);
    ASSERT_TRUE(gotten.is_err());
    EXPECT_EQ(gotten.unwrap_err(), Status::InvArgs);
    EXPECT_FALSE(fake::kernel().is_open(stray_));
}

/// The epitaph a provider closes with stands in for the failure.
TEST_F(ResourceClient, EpitaphBecomesTheFailure) {
    mode_ = Mode::Epitaph;
    auto const gotten = prot::resource::get(ends_[0], ASKED);
    ASSERT_TRUE(gotten.is_err());
    EXPECT_EQ(gotten.unwrap_err(), Status::NotFound);
}

/// An Ok epitaph says nothing about why: it reads as the connection closing.
TEST_F(ResourceClient, OkEpitaphReadsPeerClosed) {
    mode_ = Mode::OkEpitaph;
    auto const gotten = prot::resource::get(ends_[0], ASKED);
    ASSERT_TRUE(gotten.is_err());
    EXPECT_EQ(gotten.unwrap_err(), Status::PeerClosed);
}

/// A peer gone without a word reads PeerClosed.
TEST_F(ResourceClient, SilentCloseReadsPeerClosed) {
    mode_ = Mode::Silent;
    auto const gotten = prot::resource::get(ends_[0], ASKED);
    ASSERT_TRUE(gotten.is_err());
    EXPECT_EQ(gotten.unwrap_err(), Status::PeerClosed);
}
