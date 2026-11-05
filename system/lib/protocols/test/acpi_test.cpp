/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The ours.acpi codec (<prot/acpi.hpp>): each request and reply is
// encoded, decoded back, and then broken on purpose, one malformed form at a
// time, and one method's messages are refused by the other's decoders.
#include <prot/acpi.hpp>

#include <array>
#include <span>
#include <vector>

#include <gtest/gtest.h>
#include <ours/interrupt.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;
using prot::acpi::EcamBaseReply;
using prot::acpi::EcamBaseRequest;
using prot::acpi::IsaIrqReply;
using prot::acpi::IsaIrqRequest;
using prot::acpi::Polarity;
using prot::acpi::Trigger;
using wire::Message;

constexpr auto FILL = u8{0xCC};
constexpr auto TXID = u32{9};
// The root domain the reply carries, by the kernel's name for it.
constexpr auto ROOT_DOMAIN = ours::ROOT_INTERRUPT_DOMAIN;

// Where the fields are, written out here and not taken from the codec, so that
// a layout changed on one side only fails.
constexpr auto REQUEST_SIZE = usize{20};
constexpr auto ISA_REPLY_SIZE = usize{36};
constexpr auto ECAM_REPLY_SIZE = usize{40};

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

    [[nodiscard]]
    auto cut(usize const size) const -> Message {
        return Message{.bytes = std::span{bytes}.first(size), .handles = handles};
    }

    /// A copy with |extra| zero bytes after the message.
    [[nodiscard]]
    auto extended(usize const extra) const -> Owned {
        auto copy = *this;
        copy.bytes.resize(bytes.size() + extra, 0);
        return copy;
    }

    /// A copy that carries a handle.
    [[nodiscard]]
    auto with_handle() const -> Owned {
        auto copy = *this;
        copy.handles.push_back(Uandle{0x1003});
        return copy;
    }

    template <typename T>
    [[nodiscard]]
    auto at(usize const offset) const -> T {
        return wire::load<T>(std::span<u8 const>{bytes}, offset);
    }

    auto set_u32(usize const offset, u32 const value) -> void {
        wire::store(std::span{bytes}, offset, value);
    }

    auto set_u16(usize const offset, u16 const value) -> void {
        wire::store(std::span{bytes}, offset, value);
    }

    auto set_u8(usize const offset, u8 const value) -> void {
        bytes[offset] = value;
    }
};

auto owned(ustl::Result<Message, Status> const &encoded) -> Owned {
    EXPECT_TRUE(encoded.is_ok());
    return Owned(encoded.unwrap());
}

// ------------------------------------------------------------------ requests

TEST(RouteIsaIrq, RoundTrip) {
    for (auto const isa: {u32{0}, u32{1}, u32{15}, u32{0xffff'ffff}}) {
        Buffer buffer;
        auto const message =
            owned(prot::acpi::encode_isa_irq(buffer.bytes, {.txid = TXID, .isa_irq = isa}));
        EXPECT_EQ(message.bytes.size(), REQUEST_SIZE);
        EXPECT_TRUE(message.handles.empty());

        auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
        ASSERT_TRUE(header.is_ok());
        EXPECT_EQ(header->ordinal, prot::acpi::ISA_IRQ.ordinal);

        auto const decoded = prot::acpi::decode_isa_irq(message.view());
        ASSERT_TRUE(decoded.is_ok());
        EXPECT_EQ(decoded->txid, TXID);
        EXPECT_EQ(decoded->isa_irq, isa);
    }
}

TEST(RouteEcamBase, RoundTrip) {
    for (auto const segment: {u16{0}, u16{1}, u16{0xffff}}) {
        Buffer buffer;
        auto const message =
            owned(prot::acpi::encode_ecam_base(buffer.bytes, {.txid = TXID, .segment = segment}));
        EXPECT_EQ(message.bytes.size(), REQUEST_SIZE);

        auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
        ASSERT_TRUE(header.is_ok());
        EXPECT_EQ(header->ordinal, prot::acpi::ECAM_BASE.ordinal);

        auto const decoded = prot::acpi::decode_ecam_base(message.view());
        ASSERT_TRUE(decoded.is_ok());
        EXPECT_EQ(decoded->txid, TXID);
        EXPECT_EQ(decoded->segment, segment);
    }
}

TEST(RouteRequests, MethodsAreTwo) {
    EXPECT_NE(prot::acpi::ISA_IRQ.ordinal, prot::acpi::ECAM_BASE.ordinal);
    EXPECT_EQ(prot::acpi::ISA_IRQ.kind, wire::MethodKind::TwoWay);
    EXPECT_EQ(prot::acpi::ECAM_BASE.kind, wire::MethodKind::TwoWay);
}

TEST(RouteRequests, EncoderRefusals) {
    Buffer buffer;
    for (auto const txid: {u32{0}, wire::MAX_TXID + 1}) {
        auto const isa = prot::acpi::encode_isa_irq(buffer.bytes, {.txid = txid, .isa_irq = 1});
        ASSERT_TRUE(isa.is_err());
        EXPECT_EQ(isa.unwrap_err(), Status::InvArgs);
        auto const ecam = prot::acpi::encode_ecam_base(buffer.bytes, {.txid = txid, .segment = 0});
        ASSERT_TRUE(ecam.is_err());
        EXPECT_EQ(ecam.unwrap_err(), Status::InvArgs);
    }

    auto const room = std::span{buffer.bytes}.first(REQUEST_SIZE - 1);
    auto const isa = prot::acpi::encode_isa_irq(room, {.txid = TXID, .isa_irq = 1});
    ASSERT_TRUE(isa.is_err());
    EXPECT_EQ(isa.unwrap_err(), Status::BufferTooSmall);
    auto const ecam = prot::acpi::encode_ecam_base(room, {.txid = TXID, .segment = 0});
    ASSERT_TRUE(ecam.is_err());
    EXPECT_EQ(ecam.unwrap_err(), Status::BufferTooSmall);
}

TEST(RouteRequests, DecoderRefusals) {
    Buffer buffer;
    auto isa = owned(prot::acpi::encode_isa_irq(buffer.bytes, {.txid = TXID, .isa_irq = 1}));
    auto ecam = owned(prot::acpi::encode_ecam_base(buffer.bytes, {.txid = TXID, .segment = 1}));

    // Each method's decoder refuses the other's message.
    EXPECT_TRUE(prot::acpi::decode_isa_irq(ecam.view()).is_err());
    EXPECT_TRUE(prot::acpi::decode_ecam_base(isa.view()).is_err());

    // A body that is not exactly the request's size, either way.
    EXPECT_TRUE(prot::acpi::decode_isa_irq(isa.cut(REQUEST_SIZE - 1)).is_err());
    EXPECT_TRUE(prot::acpi::decode_isa_irq(isa.cut(wire::HEADER_SIZE)).is_err());
    EXPECT_TRUE(prot::acpi::decode_isa_irq(isa.extended(4).view()).is_err());
    EXPECT_TRUE(prot::acpi::decode_ecam_base(ecam.cut(REQUEST_SIZE - 1)).is_err());
    EXPECT_TRUE(prot::acpi::decode_ecam_base(ecam.extended(4).view()).is_err());

    // A request carries no handle.
    auto isa_with_handle =
        owned(prot::acpi::encode_isa_irq(buffer.bytes, {.txid = TXID, .isa_irq = 1}));
    EXPECT_TRUE(prot::acpi::decode_isa_irq(isa_with_handle.with_handle().view()).is_err());
    auto ecam_with_handle =
        owned(prot::acpi::encode_ecam_base(buffer.bytes, {.txid = TXID, .segment = 1}));
    EXPECT_TRUE(prot::acpi::decode_ecam_base(ecam_with_handle.with_handle().view()).is_err());

    // A message that travels to a client is not a request, whichever method.
    auto reply = wire::start_message(buffer.bytes, REQUEST_SIZE, TXID, wire::Direction::ToClient,
                                     prot::acpi::ISA_IRQ.ordinal);
    ASSERT_TRUE(reply.is_ok());
    EXPECT_TRUE(
        prot::acpi::decode_isa_irq(Message{.bytes = reply.unwrap(), .handles = {}}).is_err());
    auto ecam_reply = wire::start_message(buffer.bytes, REQUEST_SIZE, TXID,
                                          wire::Direction::ToClient, prot::acpi::ECAM_BASE.ordinal);
    ASSERT_TRUE(ecam_reply.is_ok());
    EXPECT_TRUE(prot::acpi::decode_ecam_base(Message{.bytes = ecam_reply.unwrap(), .handles = {}})
                    .is_err());

    // A request carries a txid from 1 to MAX_TXID: a two-way method has a reply
    // to match to it. Broken in the bytes, since the encoder refuses to make
    // one.
    for (auto const txid: {u32{0}, wire::MAX_TXID + 1, u32{0xffff'ffff}}) {
        isa.set_u32(0, txid);
        EXPECT_TRUE(prot::acpi::decode_isa_irq(isa.view()).is_err()) << txid;
        ecam.set_u32(0, txid);
        EXPECT_TRUE(prot::acpi::decode_ecam_base(ecam.view()).is_err()) << txid;
    }
    isa.set_u32(0, TXID);
    ecam.set_u32(0, TXID);

    // A segment is 16 bits: the upper half of its field is zero.
    ecam.set_u32(wire::HEADER_SIZE, 0x1'0000);
    auto const wide = prot::acpi::decode_ecam_base(ecam.view());
    ASSERT_TRUE(wide.is_err());
    EXPECT_EQ(wide.unwrap_err(), Status::InvArgs);
    ecam.set_u32(wire::HEADER_SIZE, 0xffff);
    EXPECT_TRUE(prot::acpi::decode_ecam_base(ecam.view()).is_ok());
}

// The offsets of the fields, read straight out of the bytes, as the comment of
// the header says them and not as the codec holds them.
TEST(RouteLayout, FieldsAreWhereTheHeaderSaysTheyAre) {
    Buffer buffer;
    auto const isa_request =
        owned(prot::acpi::encode_isa_irq(buffer.bytes, {.txid = TXID, .isa_irq = 0x0102'0304}));
    EXPECT_EQ(isa_request.at<u32>(0), TXID);
    EXPECT_EQ(isa_request.at<u32>(16), 0x0102'0304u);

    auto const ecam_request =
        owned(prot::acpi::encode_ecam_base(buffer.bytes, {.txid = TXID, .segment = 0xabcd}));
    EXPECT_EQ(ecam_request.at<u32>(16), 0xabcdu);

    auto const isa_reply =
        owned(prot::acpi::encode_isa_irq_reply(buffer.bytes, TXID,
                                               {.status = Status::Ok,
                                                .gsi = 0x0a0b'0c0d,
                                                .trigger = Trigger::Level,
                                                .polarity = Polarity::ActiveLow}));
    EXPECT_EQ(isa_reply.at<u32>(16), static_cast<u32>(Status::Ok));
    EXPECT_EQ(isa_reply.at<u32>(20), 0u);
    EXPECT_EQ(isa_reply.at<u32>(24), 0x0a0b'0c0du);
    EXPECT_EQ(isa_reply.at<u32>(28), 1u);
    EXPECT_EQ(isa_reply.at<u32>(32), 1u);
    auto const isa_other =
        owned(prot::acpi::encode_isa_irq_reply(buffer.bytes, TXID,
                                               {.status = Status::Ok,
                                                .gsi = 1,
                                                .trigger = Trigger::Edge,
                                                .polarity = Polarity::ActiveLow}));
    EXPECT_EQ(isa_other.at<u32>(28), 0u);
    EXPECT_EQ(isa_other.at<u32>(32), 1u);

    auto const ecam_reply =
        owned(prot::acpi::encode_ecam_base_reply(buffer.bytes, TXID,
                                                 {.status = Status::Ok,
                                                  .address = 0x1122'3344'5566'7788,
                                                  .size = 0x99aa'bbcc'ddee'ff00,
                                                  .first_bus = 0x12,
                                                  .last_bus = 0x34}));
    EXPECT_EQ(ecam_reply.at<u32>(16), static_cast<u32>(Status::Ok));
    EXPECT_EQ(ecam_reply.at<u8>(20), 0x12);
    EXPECT_EQ(ecam_reply.at<u8>(21), 0x34);
    EXPECT_EQ(ecam_reply.at<u16>(22), 0);
    EXPECT_EQ(ecam_reply.at<u64>(24), 0x1122'3344'5566'7788u);
    EXPECT_EQ(ecam_reply.at<u64>(32), 0x99aa'bbcc'ddee'ff00u);
}

// ------------------------------------------------------------------- replies

TEST(RouteIsaIrqReply, RoundTrip) {
    for (auto const trigger: {Trigger::Edge, Trigger::Level}) {
        for (auto const polarity: {Polarity::ActiveHigh, Polarity::ActiveLow}) {
            for (auto const gsi: {u32{0}, u32{2}, u32{0xffff'ffff}}) {
                Buffer buffer;
                auto const message =
                    owned(prot::acpi::encode_isa_irq_reply(buffer.bytes, TXID,
                                                           {.status = Status::Ok,
                                                            .domain = ROOT_DOMAIN,
                                                            .gsi = gsi,
                                                            .trigger = trigger,
                                                            .polarity = polarity}));
                EXPECT_EQ(message.bytes.size(), ISA_REPLY_SIZE);

                auto const decoded = prot::acpi::decode_isa_irq_reply(message.view(), TXID);
                ASSERT_TRUE(decoded.is_ok());
                EXPECT_EQ(decoded->status, Status::Ok);
                EXPECT_EQ(decoded->domain, ROOT_DOMAIN);
                EXPECT_EQ(decoded->gsi, gsi);
                EXPECT_EQ(decoded->trigger, trigger);
                EXPECT_EQ(decoded->polarity, polarity);
            }
        }
    }
}

TEST(RouteIsaIrqReply, FailureIsTheStatusForm) {
    for (auto const status: {Status::NotFound, Status::InvArgs}) {
        Buffer buffer;
        auto const message =
            owned(prot::acpi::encode_isa_irq_reply(buffer.bytes, TXID, {.status = status}));
        EXPECT_EQ(message.bytes.size(), wire::STATUS_FORM_SIZE);
        auto const decoded = prot::acpi::decode_isa_irq_reply(message.view(), TXID);
        ASSERT_TRUE(decoded.is_ok());
        EXPECT_EQ(decoded->status, status);
    }
}

TEST(RouteIsaIrqReply, EncoderRefusals) {
    Buffer buffer;
    // A body next to a failure.
    EXPECT_TRUE(
        prot::acpi::encode_isa_irq_reply(buffer.bytes, TXID, {.status = Status::NotFound, .gsi = 1})
            .is_err());
    EXPECT_TRUE(prot::acpi::encode_isa_irq_reply(
                    buffer.bytes, TXID, {.status = Status::NotFound, .trigger = Trigger::Level})
                    .is_err());
    EXPECT_TRUE(
        prot::acpi::encode_isa_irq_reply(
            buffer.bytes, TXID, {.status = Status::NotFound, .polarity = Polarity::ActiveLow})
            .is_err());
    // A value the enums do not name.
    EXPECT_TRUE(prot::acpi::encode_isa_irq_reply(
                    buffer.bytes, TXID, {.status = Status::Ok, .trigger = static_cast<Trigger>(2)})
                    .is_err());
    EXPECT_TRUE(
        prot::acpi::encode_isa_irq_reply(
            buffer.bytes, TXID, {.status = Status::Ok, .polarity = static_cast<Polarity>(2)})
            .is_err());
    // A txid out of range, and no room.
    EXPECT_TRUE(prot::acpi::encode_isa_irq_reply(buffer.bytes, 0, {.status = Status::Ok}).is_err());
    auto const room = std::span{buffer.bytes}.first(ISA_REPLY_SIZE - 1);
    auto const short_buffer = prot::acpi::encode_isa_irq_reply(room, TXID, {.status = Status::Ok});
    ASSERT_TRUE(short_buffer.is_err());
    EXPECT_EQ(short_buffer.unwrap_err(), Status::BufferTooSmall);
}

TEST(RouteIsaIrqReply, DecoderRefusals) {
    Buffer buffer;
    auto const make = [&] {
        return owned(prot::acpi::encode_isa_irq_reply(buffer.bytes, TXID,
                                                      {.status = Status::Ok,
                                                       .gsi = 2,
                                                       .trigger = Trigger::Level,
                                                       .polarity = Polarity::ActiveLow}));
    };

    // A reply to another request.
    EXPECT_TRUE(prot::acpi::decode_isa_irq_reply(make().view(), TXID + 1).is_err());
    // The other method's reply.
    auto ecam =
        owned(prot::acpi::encode_ecam_base_reply(buffer.bytes, TXID, {.status = Status::Ok}));
    EXPECT_TRUE(prot::acpi::decode_isa_irq_reply(ecam.view(), TXID).is_err());
    // Not the size of an Ok reply: the status form alone, one byte short, one word long.
    auto ok = make();
    EXPECT_TRUE(prot::acpi::decode_isa_irq_reply(ok.cut(wire::STATUS_FORM_SIZE), TXID).is_err());
    EXPECT_TRUE(prot::acpi::decode_isa_irq_reply(ok.cut(ISA_REPLY_SIZE - 1), TXID).is_err());
    EXPECT_TRUE(prot::acpi::decode_isa_irq_reply(ok.extended(4).view(), TXID).is_err());
    // A handle.
    auto with_handle = make();
    EXPECT_TRUE(prot::acpi::decode_isa_irq_reply(with_handle.with_handle().view(), TXID).is_err());
    // Values the enums do not name, including ones whose low byte does: the
    // fields are 32 bits wide and all of them count.
    for (auto const value: {u32{2}, u32{0x100}, u32{0x101}, u32{0x8000'0001}, u32{0xffff'ffff}}) {
        auto bad_trigger = make();
        bad_trigger.set_u32(28, value);
        EXPECT_TRUE(prot::acpi::decode_isa_irq_reply(bad_trigger.view(), TXID).is_err()) << value;
        auto bad_polarity = make();
        bad_polarity.set_u32(32, value);
        EXPECT_TRUE(prot::acpi::decode_isa_irq_reply(bad_polarity.view(), TXID).is_err()) << value;
    }
    // A failure with a body after it is not the status form.
    auto failure =
        owned(prot::acpi::encode_isa_irq_reply(buffer.bytes, TXID, {.status = Status::NotFound}));
    EXPECT_TRUE(prot::acpi::decode_isa_irq_reply(failure.extended(8).view(), TXID).is_err());
}

TEST(RouteEcamBaseReply, RoundTrip) {
    struct Case {
        u64 address;
        u64 size;
        u8 first_bus;
        u8 last_bus;
    };
    for (auto const &c:
         {Case{0xe000'0000, u64{256} << 20, 0, 255}, Case{0x4'0100'0000, u64{16} << 20, 16, 31},
          Case{~u64{0}, ~u64{0}, 255, 255}}) {
        Buffer buffer;
        auto const message = owned(prot::acpi::encode_ecam_base_reply(buffer.bytes, TXID,
                                                                      {.status = Status::Ok,
                                                                       .address = c.address,
                                                                       .size = c.size,
                                                                       .first_bus = c.first_bus,
                                                                       .last_bus = c.last_bus}));
        EXPECT_EQ(message.bytes.size(), ECAM_REPLY_SIZE);

        auto const decoded = prot::acpi::decode_ecam_base_reply(message.view(), TXID);
        ASSERT_TRUE(decoded.is_ok());
        EXPECT_EQ(decoded->status, Status::Ok);
        EXPECT_EQ(decoded->address, c.address);
        EXPECT_EQ(decoded->size, c.size);
        EXPECT_EQ(decoded->first_bus, c.first_bus);
        EXPECT_EQ(decoded->last_bus, c.last_bus);
    }
}

TEST(RouteEcamBaseReply, FailureIsTheStatusForm) {
    for (auto const status: {Status::NotFound, Status::InvArgs}) {
        Buffer buffer;
        auto const message =
            owned(prot::acpi::encode_ecam_base_reply(buffer.bytes, TXID, {.status = status}));
        EXPECT_EQ(message.bytes.size(), wire::STATUS_FORM_SIZE);
        auto const decoded = prot::acpi::decode_ecam_base_reply(message.view(), TXID);
        ASSERT_TRUE(decoded.is_ok());
        EXPECT_EQ(decoded->status, status);
    }
}

TEST(RouteEcamBaseReply, EncoderRefusals) {
    Buffer buffer;
    // A body next to a failure, each field.
    EXPECT_TRUE(prot::acpi::encode_ecam_base_reply(buffer.bytes, TXID,
                                                   {.status = Status::NotFound, .address = 1})
                    .is_err());
    EXPECT_TRUE(prot::acpi::encode_ecam_base_reply(buffer.bytes, TXID,
                                                   {.status = Status::NotFound, .size = 1})
                    .is_err());
    EXPECT_TRUE(prot::acpi::encode_ecam_base_reply(buffer.bytes, TXID,
                                                   {.status = Status::NotFound, .first_bus = 1})
                    .is_err());
    EXPECT_TRUE(prot::acpi::encode_ecam_base_reply(buffer.bytes, TXID,
                                                   {.status = Status::NotFound, .last_bus = 1})
                    .is_err());
    EXPECT_TRUE(
        prot::acpi::encode_ecam_base_reply(buffer.bytes, 0, {.status = Status::Ok}).is_err());
    auto const room = std::span{buffer.bytes}.first(ECAM_REPLY_SIZE - 1);
    auto const short_buffer =
        prot::acpi::encode_ecam_base_reply(room, TXID, {.status = Status::Ok});
    ASSERT_TRUE(short_buffer.is_err());
    EXPECT_EQ(short_buffer.unwrap_err(), Status::BufferTooSmall);
}

TEST(RouteEcamBaseReply, DecoderRefusals) {
    Buffer buffer;
    auto const make = [&] {
        return owned(prot::acpi::encode_ecam_base_reply(buffer.bytes, TXID,
                                                        {.status = Status::Ok,
                                                         .address = 0xe000'0000,
                                                         .size = u64{256} << 20,
                                                         .first_bus = 0,
                                                         .last_bus = 255}));
    };

    EXPECT_TRUE(prot::acpi::decode_ecam_base_reply(make().view(), TXID + 1).is_err());
    auto isa = owned(prot::acpi::encode_isa_irq_reply(buffer.bytes, TXID, {.status = Status::Ok}));
    EXPECT_TRUE(prot::acpi::decode_ecam_base_reply(isa.view(), TXID).is_err());
    auto ok = make();
    EXPECT_TRUE(prot::acpi::decode_ecam_base_reply(ok.cut(wire::STATUS_FORM_SIZE), TXID).is_err());
    EXPECT_TRUE(prot::acpi::decode_ecam_base_reply(ok.cut(ECAM_REPLY_SIZE - 1), TXID).is_err());
    EXPECT_TRUE(prot::acpi::decode_ecam_base_reply(ok.extended(8).view(), TXID).is_err());
    auto with_handle = make();
    EXPECT_TRUE(
        prot::acpi::decode_ecam_base_reply(with_handle.with_handle().view(), TXID).is_err());
    // The two bytes after the buses are zero.
    auto reserved = make();
    reserved.set_u16(22, 1);
    EXPECT_TRUE(prot::acpi::decode_ecam_base_reply(reserved.view(), TXID).is_err());
    auto failure =
        owned(prot::acpi::encode_ecam_base_reply(buffer.bytes, TXID, {.status = Status::NotFound}));
    EXPECT_TRUE(prot::acpi::decode_ecam_base_reply(failure.extended(8).view(), TXID).is_err());
}
} // namespace
