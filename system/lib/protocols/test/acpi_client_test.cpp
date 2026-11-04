/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// prot::acpi's client against a fake Route provider: the typed client's
// calls on a connection the caller keeps, and what each failure reads as.
// The test plays the provider inside the fixture's pump -- when the
// client's wait asks the machine to progress, the pump reads the request
// off the server end, decodes it with the codec's own decoders, and answers
// per the mode with the codec's reply encoders. What the cases pin: both
// round trips (the ISA line and the segment arriving as sent, the answer's
// fields coming back), the epitaph a provider closes with standing in for
// the failure, an Ok epitaph reading as the connection closing, a reply
// carrying a handle refused with its handle closed, a reply to the other
// method caught as InvArgs, and the peer gone without a word reading
// PeerClosed. The /svc overloads are the namespace's to serve and have no
// host; the connection-taking ones carry all of the conduct.
#include <prot/acpi_client.hpp>

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

// The /svc overloads connect through the process's namespace, which does
// not exist on the host; no case here calls one. The definition keeps the
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

/// The question each round trip sends, and the answer the provider gives:
/// the facts the test and the pump share.
constexpr auto ISA_LINE = u32{4};
constexpr auto ISA_DOMAIN = u32{0};
constexpr auto ISA_GSI = u32{21};
constexpr auto ECAM_SEGMENT = u16{1};
constexpr auto ECAM_ADDRESS = u64{0xE000'0000};
constexpr auto ECAM_FIRST_BUS = u8{0};
constexpr auto ECAM_LAST_BUS = u8{31};
constexpr auto ECAM_SIZE = u64{0x0200'0000};

/// One test's provider: the pump reads the request off the server end and
/// answers per the mode. IsaIrq and EcamBase answer the question; StrayHandle
/// rides a handle on the reply, which this family never carries; IsaIrqAsEcam
/// answers the IsaIrq question with the other method's reply; Epitaph closes
/// with the epitaph after reading the request; OkEpitaph closes saying
/// nothing; Silent closes without answering.
enum class Mode {
    IsaIrq,
    EcamBase,
    StrayHandle,
    IsaIrqAsEcam,
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
class OursAcpiClient: public testing::Test {
  protected:
    OursAcpiClient() {
        fake::kernel().reset();
        ends_ = channel();
        WAIT_PUMP = [this] { pump(); };
    }

    ~OursAcpiClient() override {
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
            case Mode::IsaIrq: {
                auto const decoded = prot::acpi::decode_isa_irq(message);
                ASSERT_TRUE(decoded.is_ok());
                EXPECT_EQ(decoded->isa_irq, ISA_LINE);
                auto const reply = prot::acpi::encode_isa_irq_reply(
                    reply_scratch_, txid,
                    {.status = Status::Ok,
                     .domain = ISA_DOMAIN,
                     .gsi = ISA_GSI,
                     .trigger = prot::acpi::Trigger::Edge,
                     .polarity = prot::acpi::Polarity::ActiveHigh});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply);
                return;
            }
            case Mode::EcamBase: {
                auto const decoded = prot::acpi::decode_ecam_base(message);
                ASSERT_TRUE(decoded.is_ok());
                EXPECT_EQ(decoded->segment, ECAM_SEGMENT);
                auto const reply = prot::acpi::encode_ecam_base_reply(reply_scratch_, txid,
                                                                      {.status = Status::Ok,
                                                                       .address = ECAM_ADDRESS,
                                                                       .size = ECAM_SIZE,
                                                                       .first_bus = ECAM_FIRST_BUS,
                                                                       .last_bus = ECAM_LAST_BUS});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply);
                return;
            }
            case Mode::StrayHandle: {
                // A reply with a handle on it, which this family never
                // carries: the client's to close, and the answer reads as
                // InvArgs, not as the reply.
                auto const stray = channel();
                stray_ = stray[0];
                auto const reply = prot::acpi::encode_isa_irq_reply(
                    reply_scratch_, txid, {.status = Status::Ok, .gsi = ISA_GSI});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply, stray[0]);
                return;
            }
            case Mode::IsaIrqAsEcam: {
                // A well-formed reply, but to the other method: the
                // ordinal is what tells them apart, and the mismatch is
                // what the client names.
                auto const reply = prot::acpi::encode_ecam_base_reply(
                    reply_scratch_, txid,
                    {.status = Status::Ok, .address = ECAM_ADDRESS, .size = ECAM_SIZE});
                ASSERT_TRUE(reply.is_ok());
                write_reply(*reply);
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
    /// with |handle| riding it when the mode brings one.
    auto write_reply(wire::Message const &reply, Uandle const handle = Uandle::Invalid) -> void {
        auto riding = std::array<Uandle, 1>{handle};
        if (handle == Uandle::Invalid) {
            EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None,
                                          reply.bytes.data(), static_cast<u32>(reply.bytes.size()),
                                          nullptr, 0),
                      Status::Ok);
            return;
        }
        EXPECT_EQ(ours::channel_write(ends_[1], ours::ChannelWriteOptions::None, reply.bytes.data(),
                                      static_cast<u32>(reply.bytes.size()), riding.data(), 1),
                  Status::Ok);
    }

    std::array<Uandle, 2> ends_{};
    Mode mode_ = Mode::IsaIrq;
    bool answered_ = false;
    Uandle stray_ = Uandle::Invalid;
    std::array<u8, wire::MAX_MESSAGE_BYTES> reply_scratch_{};
};

} // namespace

/// The ISA round trip: the line arrives as sent, and the answer's GSI and
/// signalling come back in the caller's Result.
TEST_F(OursAcpiClient, IsaIrqRoundTrip) {
    auto const routed = prot::acpi::isa_irq(ends_[0], ISA_LINE);
    ASSERT_TRUE(routed.is_ok());
    EXPECT_EQ(routed->domain, ISA_DOMAIN);
    EXPECT_EQ(routed->gsi, ISA_GSI);
    EXPECT_EQ(routed->trigger, prot::acpi::Trigger::Edge);
    EXPECT_EQ(routed->polarity, prot::acpi::Polarity::ActiveHigh);
}

/// The ECAM round trip: the segment arrives as sent, and the window's
/// address, size and buses come back.
TEST_F(OursAcpiClient, EcamBaseRoundTrip) {
    mode_ = Mode::EcamBase;
    auto const window = prot::acpi::ecam_base(ends_[0], ECAM_SEGMENT);
    ASSERT_TRUE(window.is_ok());
    EXPECT_EQ(window->address, ECAM_ADDRESS);
    EXPECT_EQ(window->size, ECAM_SIZE);
    EXPECT_EQ(window->first_bus, ECAM_FIRST_BUS);
    EXPECT_EQ(window->last_bus, ECAM_LAST_BUS);
}

/// A reply carrying a handle, which this family never does: the handle is
/// closed, and the answer reads InvArgs rather than the reply.
TEST_F(OursAcpiClient, StrayHandleReadsInvArgsAndIsClosed) {
    mode_ = Mode::StrayHandle;
    auto const routed = prot::acpi::isa_irq(ends_[0], ISA_LINE);
    ASSERT_TRUE(routed.is_err());
    EXPECT_EQ(routed.unwrap_err(), Status::InvArgs);
    EXPECT_FALSE(fake::kernel().is_open(stray_));
}

/// A well-formed reply to the other method: the ordinal mismatch is caught
/// and named InvArgs, not read as the answer.
TEST_F(OursAcpiClient, OtherMethodsReplyReadsInvArgs) {
    mode_ = Mode::IsaIrqAsEcam;
    auto const routed = prot::acpi::isa_irq(ends_[0], ISA_LINE);
    ASSERT_TRUE(routed.is_err());
    EXPECT_EQ(routed.unwrap_err(), Status::InvArgs);
}

/// The epitaph a provider closes with stands in for the failure.
TEST_F(OursAcpiClient, EpitaphBecomesTheFailure) {
    mode_ = Mode::Epitaph;
    auto const routed = prot::acpi::isa_irq(ends_[0], ISA_LINE);
    ASSERT_TRUE(routed.is_err());
    EXPECT_EQ(routed.unwrap_err(), Status::NotFound);
}

/// An Ok epitaph says nothing about why: it reads as the connection closing.
TEST_F(OursAcpiClient, OkEpitaphReadsPeerClosed) {
    mode_ = Mode::OkEpitaph;
    auto const routed = prot::acpi::ecam_base(ends_[0], ECAM_SEGMENT);
    ASSERT_TRUE(routed.is_err());
    EXPECT_EQ(routed.unwrap_err(), Status::PeerClosed);
}

/// A peer gone without a word reads PeerClosed.
TEST_F(OursAcpiClient, SilentCloseReadsPeerClosed) {
    mode_ = Mode::Silent;
    auto const routed = prot::acpi::ecam_base(ends_[0], ECAM_SEGMENT);
    ASSERT_TRUE(routed.is_err());
    EXPECT_EQ(routed.unwrap_err(), Status::PeerClosed);
}
