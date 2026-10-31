/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours_acpi/ours_acpi.hpp>

namespace ours_acpi {
namespace {
    using ours::Status;
    using wire::Message;

    using Encoded = ustl::Result<Message, Status>;

    // The request of both methods: one u32 after the header.
    constexpr auto REQUEST_FIELD_OFFSET = usize{16};
    constexpr auto REQUEST_SIZE = usize{20};

    // IsaIrq's Ok reply: the status, then three u32.
    constexpr auto ISA_GSI_OFFSET = usize{20};
    constexpr auto ISA_TRIGGER_OFFSET = usize{24};
    constexpr auto ISA_POLARITY_OFFSET = usize{28};
    constexpr auto ISA_REPLY_SIZE = usize{32};

    // EcamBase's Ok reply: the status, the buses and a zero word, then two u64.
    constexpr auto ECAM_FIRST_BUS_OFFSET = usize{20};
    constexpr auto ECAM_LAST_BUS_OFFSET = usize{21};
    constexpr auto ECAM_RESERVED_OFFSET = usize{22};
    constexpr auto ECAM_ADDRESS_OFFSET = usize{24};
    constexpr auto ECAM_SIZE_OFFSET = usize{32};
    constexpr auto ECAM_REPLY_SIZE = usize{40};

    static_assert(wire::HEADER_SIZE == REQUEST_FIELD_OFFSET);
    static_assert(wire::STATUS_OFFSET + sizeof(u32) == ISA_GSI_OFFSET);
    static_assert(ISA_POLARITY_OFFSET + sizeof(u32) == ISA_REPLY_SIZE);
    static_assert(ECAM_SIZE_OFFSET + sizeof(u64) == ECAM_REPLY_SIZE);
    static_assert(ISA_REPLY_SIZE <= wire::MAX_MESSAGE_BYTES);
    static_assert(ECAM_REPLY_SIZE <= wire::MAX_MESSAGE_BYTES);

    /// Whether the enums hold a value they name.
    constexpr auto names(Trigger const trigger) -> bool {
        return trigger == Trigger::Edge || trigger == Trigger::Level;
    }

    constexpr auto names(Polarity const polarity) -> bool {
        return polarity == Polarity::ActiveHigh || polarity == Polarity::ActiveLow;
    }

    /// A request of one u32 field, checked as far as both methods go: the
    /// header, no handles, exactly REQUEST_SIZE bytes. The txid, or the error.
    auto decode_request(Message const message, wire::Method const &method)
        -> ustl::Result<u32, Status> {
        auto header = wire::decode_request_header(message, method);
        if (header.is_err()) {
            return ustl::err(header.unwrap_err());
        }
        if (!message.handles.empty() || message.bytes.size() != REQUEST_SIZE) {
            return ustl::err(Status::InvArgs);
        }
        return ustl::ok(header->txid);
    }

    auto encode_request(std::span<u8> const bytes, wire::Method const &method, u32 const txid,
                        u32 const field) -> Encoded {
        auto started = wire::start_request(bytes, REQUEST_SIZE, method, txid);
        if (started.is_err()) {
            return ustl::err(started.unwrap_err());
        }
        auto const message = started.unwrap();
        wire::store(message, REQUEST_FIELD_OFFSET, field);
        return ustl::ok(Message{.bytes = message, .handles = {}});
    }

    /// The Ok reply of |size| bytes to the request |txid| of |method|, or the
    /// error start_ok_reply() gave.
    auto start_ok(std::span<u8> const bytes, wire::Method const &method, u32 const txid,
                  usize const size) -> ustl::Result<std::span<u8>, Status> {
        return wire::start_ok_reply(bytes, size, method, txid);
    }

    /// The status of a reply of |method| to |txid| and whether it is Ok with a
    /// body of exactly |size| bytes and no handles to read. An Err is a
    /// malformed reply. For a failure, the status form that
    /// decode_reply_status() already held it to.
    struct Reply {
        Status status;
        bool has_body;
    };

    auto decode_reply(Message const message, wire::Method const &method, u32 const txid,
                      usize const size) -> ustl::Result<Reply, Status> {
        auto status = wire::decode_reply_status(message, method, txid);
        if (status.is_err()) {
            return ustl::err(status.unwrap_err());
        }
        if (status.unwrap() != Status::Ok) {
            return ustl::ok(Reply{.status = status.unwrap(), .has_body = false});
        }
        if (!message.handles.empty() || message.bytes.size() != size) {
            return ustl::err(Status::InvArgs);
        }
        return ustl::ok(Reply{.status = Status::Ok, .has_body = true});
    }
} // namespace

auto encode_isa_irq(std::span<u8> const bytes, IsaIrqRequest const &request) -> Encoded {
    return encode_request(bytes, ISA_IRQ, request.txid, request.isa_irq);
}

auto decode_isa_irq(Message const message) -> ustl::Result<IsaIrqRequest, Status> {
    auto txid = decode_request(message, ISA_IRQ);
    if (txid.is_err()) {
        return ustl::err(txid.unwrap_err());
    }
    return ustl::ok(IsaIrqRequest{
        .txid = txid.unwrap(),
        .isa_irq = wire::load<u32>(message.bytes, REQUEST_FIELD_OFFSET),
    });
}

auto encode_ecam_base(std::span<u8> const bytes, EcamBaseRequest const &request) -> Encoded {
    return encode_request(bytes, ECAM_BASE, request.txid, request.segment);
}

auto decode_ecam_base(Message const message) -> ustl::Result<EcamBaseRequest, Status> {
    auto txid = decode_request(message, ECAM_BASE);
    if (txid.is_err()) {
        return ustl::err(txid.unwrap_err());
    }
    // A segment is 16 bits in a 32 bit field; the upper half is zero.
    auto const segment = wire::load<u32>(message.bytes, REQUEST_FIELD_OFFSET);
    if (segment > 0xffff) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(EcamBaseRequest{.txid = txid.unwrap(), .segment = static_cast<u16>(segment)});
}

auto encode_isa_irq_reply(std::span<u8> const bytes, u32 const txid, IsaIrqReply const &reply)
    -> Encoded {
    auto const has_body =
        reply.gsi != 0 || reply.trigger != Trigger::Edge || reply.polarity != Polarity::ActiveHigh;
    if (reply.status != Status::Ok) {
        if (has_body) {
            return ustl::err(Status::InvArgs);
        }
        return wire::encode_status_reply(bytes, ISA_IRQ, txid, reply.status);
    }
    if (!names(reply.trigger) || !names(reply.polarity)) {
        return ustl::err(Status::InvArgs);
    }
    auto started = start_ok(bytes, ISA_IRQ, txid, ISA_REPLY_SIZE);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    wire::store(message, ISA_GSI_OFFSET, reply.gsi);
    wire::store(message, ISA_TRIGGER_OFFSET, static_cast<u32>(reply.trigger));
    wire::store(message, ISA_POLARITY_OFFSET, static_cast<u32>(reply.polarity));
    return ustl::ok(Message{.bytes = message, .handles = {}});
}

auto decode_isa_irq_reply(Message const message, u32 const txid)
    -> ustl::Result<IsaIrqReply, Status> {
    auto reply = decode_reply(message, ISA_IRQ, txid, ISA_REPLY_SIZE);
    if (reply.is_err()) {
        return ustl::err(reply.unwrap_err());
    }
    if (!reply->has_body) {
        return ustl::ok(IsaIrqReply{.status = reply->status});
    }
    auto const trigger = wire::load<u32>(message.bytes, ISA_TRIGGER_OFFSET);
    auto const polarity = wire::load<u32>(message.bytes, ISA_POLARITY_OFFSET);
    if (trigger > static_cast<u32>(Trigger::Level) ||
        polarity > static_cast<u32>(Polarity::ActiveLow)) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(IsaIrqReply{
        .status = Status::Ok,
        .gsi = wire::load<u32>(message.bytes, ISA_GSI_OFFSET),
        .trigger = static_cast<Trigger>(trigger),
        .polarity = static_cast<Polarity>(polarity),
    });
}

auto encode_ecam_base_reply(std::span<u8> const bytes, u32 const txid, EcamBaseReply const &reply)
    -> Encoded {
    if (reply.status != Status::Ok) {
        if (reply.address != 0 || reply.size != 0 || reply.first_bus != 0 || reply.last_bus != 0) {
            return ustl::err(Status::InvArgs);
        }
        return wire::encode_status_reply(bytes, ECAM_BASE, txid, reply.status);
    }
    auto started = start_ok(bytes, ECAM_BASE, txid, ECAM_REPLY_SIZE);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    wire::store(message, ECAM_FIRST_BUS_OFFSET, reply.first_bus);
    wire::store(message, ECAM_LAST_BUS_OFFSET, reply.last_bus);
    wire::store(message, ECAM_ADDRESS_OFFSET, reply.address);
    wire::store(message, ECAM_SIZE_OFFSET, reply.size);
    return ustl::ok(Message{.bytes = message, .handles = {}});
}

auto decode_ecam_base_reply(Message const message, u32 const txid)
    -> ustl::Result<EcamBaseReply, Status> {
    auto reply = decode_reply(message, ECAM_BASE, txid, ECAM_REPLY_SIZE);
    if (reply.is_err()) {
        return ustl::err(reply.unwrap_err());
    }
    if (!reply->has_body) {
        return ustl::ok(EcamBaseReply{.status = reply->status});
    }
    if (wire::load<u16>(message.bytes, ECAM_RESERVED_OFFSET) != 0) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(EcamBaseReply{
        .status = Status::Ok,
        .address = wire::load<u64>(message.bytes, ECAM_ADDRESS_OFFSET),
        .size = wire::load<u64>(message.bytes, ECAM_SIZE_OFFSET),
        .first_bus = wire::load<u8>(message.bytes, ECAM_FIRST_BUS_OFFSET),
        .last_bus = wire::load<u8>(message.bytes, ECAM_LAST_BUS_OFFSET),
    });
}
} // namespace ours_acpi
