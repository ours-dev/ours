/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// prot::fshost::Client against a fake Admin provider: the typed client's
// one call and what each failure reads as. The test plays the provider
// inside the fixture's pump -- when the client's wait asks the machine to
// progress, the pump reads the Mount off the server end, decodes it with
// the codec's own decoder, and answers per the mode. What the cases pin:
// the mount answered Ok with the name arriving as the client sent it and
// the directory handle riding the message, a taken name answering
// AlreadyExists as itself, a provider's failure status arriving as itself,
// the epitaph a provider closes with standing in for the failure, and the
// peer gone without one reading PeerClosed. The directory handle is one
// per mount -- a transferred handle cannot be sent twice -- so each case
// mounts through its own.
#include <prot/fshost_client.hpp>

#include <array>
#include <functional>
#include <span>
#include <string_view>

#include "fake_kernel.hpp"
#include <gtest/gtest.h>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
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

/// The mount point's name: what the client sends and the pump expects.
constexpr auto NAME = std::string_view{"data"};

/// One test's provider: the pump reads the Mount off the server end and
/// answers the status per the mode. Epitaph closes with the epitaph after
/// reading the request; Silent closes without answering.
enum class Mode {
    Ok,
    AlreadyExists,
    NoResource,
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

/// The fixture: a fresh fake kernel, a pair, the pump pointed at the mode.
class OursFshostClient: public testing::Test {
  protected:
    OursFshostClient() {
        fake::kernel().reset();
        m_ends = channel();
        WAIT_PUMP = [this] { pump(); };
    }

    ~OursFshostClient() override {
        WAIT_PUMP = [] {};
    }

    /// One pump step: answer whatever is queued on the server end, per the
    /// mode. The client's call spins this while it waits.
    auto pump() -> void {
        if (m_answered) {
            return;
        }
        if (fake::kernel().queued_messages(m_ends[1]) == 0) {
            return;
        }
        m_answered = true;
        auto bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
        auto handles = std::array<Uandle, wire::MAX_MESSAGE_HANDLES>{};
        auto num_bytes = u32{0};
        auto num_handles = u32{0};
        EXPECT_EQ(ours::channel_read(m_ends[1], ours::ChannelReadOptions::None, bytes.data(),
                                     handles.data(), static_cast<u32>(bytes.size()),
                                     static_cast<u32>(handles.size()), &num_bytes, &num_handles),
                  Status::Ok);
        auto const message = wire::Message{.bytes = std::span{bytes}.first(num_bytes),
                                           .handles = std::span{handles}.first(num_handles)};
        auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
        ASSERT_TRUE(header.is_ok());
        auto const txid = header->txid;

        switch (m_mode) {
            case Mode::Ok:
            case Mode::AlreadyExists:
            case Mode::NoResource: {
                // The Mount decoded as sent: the name, with the directory
                // handle riding the message's one handle.
                auto const decoded = prot::fshost::decode_mount(message);
                ASSERT_TRUE(decoded.is_ok());
                EXPECT_EQ(decoded->name, NAME);
                ASSERT_EQ(message.handles.size(), usize{1});
                EXPECT_EQ(message.handles[0], m_directory);
                auto answered = Status::NoResource;
                if (m_mode == Mode::Ok) {
                    answered = Status::Ok;
                } else if (m_mode == Mode::AlreadyExists) {
                    answered = Status::AlreadyExists;
                }
                auto const reply =
                    prot::fshost::encode_mount_reply(m_reply_scratch, txid, answered);
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply);
                return;
            }
            case Mode::Epitaph: {
                auto epitaph = wire::encode_epitaph(m_reply_scratch, Status::NotFound);
                ASSERT_TRUE(epitaph.is_ok());
                EXPECT_EQ(ours::channel_write(m_ends[1], ours::ChannelWriteOptions::None,
                                              epitaph->bytes.data(),
                                              static_cast<u32>(epitaph->bytes.size()), nullptr, 0),
                          Status::Ok);
                EXPECT_EQ(ours::handle_close(m_ends[1]), Status::Ok);
                return;
            }
            case Mode::Silent:
                // No answer: just the peer's death.
                EXPECT_EQ(ours::handle_close(m_ends[1]), Status::Ok);
                return;
        }
    }

    /// Writes an encoded reply onto the server end, back to the client.
    auto write_reply(wire::Message const &reply) -> void {
        EXPECT_EQ(ours::channel_write(m_ends[1], ours::ChannelWriteOptions::None,
                                      reply.bytes.data(), static_cast<u32>(reply.bytes.size()),
                                      nullptr, 0),
                  Status::Ok);
    }

    std::array<Uandle, 2> m_ends{};
    Mode m_mode = Mode::Ok;
    bool m_answered = false;
    Uandle m_directory = Uandle::Invalid;
    std::array<u8, wire::MAX_MESSAGE_BYTES> m_reply_scratch{};
};

} // namespace

/// The mount answered Ok: the provider decoded the name the client sent,
/// with the directory handle riding the message.
TEST_F(OursFshostClient, MountAnswersOk) {
    m_mode = Mode::Ok;
    auto client = prot::fshost::Client{m_ends[0]};
    auto const directory = channel();
    m_directory = directory[0];
    EXPECT_EQ(client.mount(directory[0], NAME), Status::Ok);
}

/// A taken name answers AlreadyExists, as itself.
TEST_F(OursFshostClient, TakenNameReadsAlreadyExists) {
    m_mode = Mode::AlreadyExists;
    auto client = prot::fshost::Client{m_ends[0]};
    auto const directory = channel();
    m_directory = directory[0];
    EXPECT_EQ(client.mount(directory[0], NAME), Status::AlreadyExists);
}

/// A provider's failure status arrives as itself.
TEST_F(OursFshostClient, FailureStatusArrivesAsItself) {
    m_mode = Mode::NoResource;
    auto client = prot::fshost::Client{m_ends[0]};
    auto const directory = channel();
    m_directory = directory[0];
    EXPECT_EQ(client.mount(directory[0], NAME), Status::NoResource);
}

/// The epitaph a provider closes with stands in for the failure.
TEST_F(OursFshostClient, EpitaphBecomesTheFailure) {
    m_mode = Mode::Epitaph;
    auto client = prot::fshost::Client{m_ends[0]};
    auto const directory = channel();
    m_directory = directory[0];
    EXPECT_EQ(client.mount(directory[0], NAME), Status::NotFound);
}

/// A peer gone without an epitaph reads PeerClosed.
TEST_F(OursFshostClient, SilentCloseReadsPeerClosed) {
    m_mode = Mode::Silent;
    auto client = prot::fshost::Client{m_ends[0]};
    auto const directory = channel();
    m_directory = directory[0];
    EXPECT_EQ(client.mount(directory[0], NAME), Status::PeerClosed);
}
