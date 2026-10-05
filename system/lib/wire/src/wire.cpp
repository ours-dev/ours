/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <wire/wire.hpp>

#include <utility>

namespace wire {
namespace {
    using ours::Status;
    using ours::Uandle;

    /// The u32 after the status in the status form.
    constexpr auto STATUS_FORM_PAD_OFFSET = usize{20};

    /// Whether |value| is a status ours::Status names. A peer's status is a
    /// plain u32 on the wire, so it can hold any value; ours reads it into an
    /// enum that code switches over, so a value past the last one is refused
    /// rather than carried along.
    auto names_a_status(u32 const value) -> bool {
        return value < std::to_underlying(Status::MaxCount);
    }

    /// The txid rule of |kind|.
    auto txid_fits(MethodKind const kind, u32 const txid) -> bool {
        if (kind == MethodKind::OneWay) {
            return txid == 0;
        }
        return txid != 0 && txid <= MAX_TXID;
    }

    /// The status form into |out|, ToClient: a reply's when |txid| and
    /// |ordinal| are a request's, an epitaph's when they are 0 and
    /// EPITAPH_ORDINAL.
    auto encode_status_form(std::span<u8> const out, u32 const txid, u64 const ordinal,
                            Status const status) -> ustl::Result<Message, Status> {
        if (!names_a_status(std::to_underlying(status))) {
            return ustl::err(Status::InvArgs);
        }
        auto started = start_message(out, STATUS_FORM_SIZE, txid, Direction::ToClient, ordinal);
        if (started.is_err()) {
            return ustl::err(started.unwrap_err());
        }
        auto const bytes = started.unwrap();
        store(bytes, STATUS_OFFSET, std::to_underlying(status));
        return ustl::ok(Message{.bytes = bytes, .handles = {}});
    }
} // namespace

auto decode_header(std::span<u8 const> const bytes, Direction const direction)
    -> ustl::Result<Header, Status> {
    if (bytes.size() < HEADER_SIZE || bytes.size() > MAX_MESSAGE_BYTES) {
        return ustl::err(Status::InvArgs);
    }
    auto const header = load<Header>(bytes, 0);
    if (header.magic != MAGIC || header.reserved[0] != 0 || header.reserved[1] != 0 ||
        header.direction != direction) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(header);
}

auto decode_request_header(Message const message, Method const &method)
    -> ustl::Result<Header, Status> {
    auto decoded = decode_header(message.bytes, Direction::ToServer);
    if (decoded.is_err()) {
        return decoded;
    }
    auto const header = decoded.unwrap();
    if (header.ordinal != method.ordinal || !txid_fits(method.kind, header.txid)) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(header);
}

auto decode_reply_status(Message const message, Method const &method, u32 const txid)
    -> ustl::Result<Status, Status> {
    if (method.kind != MethodKind::TwoWay || !txid_fits(MethodKind::TwoWay, txid)) {
        return ustl::err(Status::InvArgs);
    }
    auto decoded = decode_header(message.bytes, Direction::ToClient);
    if (decoded.is_err()) {
        return ustl::err(decoded.unwrap_err());
    }
    auto const header = decoded.unwrap();
    if (header.ordinal != method.ordinal || header.txid != txid ||
        message.bytes.size() < STATUS_FORM_PAD_OFFSET) {
        return ustl::err(Status::InvArgs);
    }
    auto const status = load<u32>(message.bytes, STATUS_OFFSET);
    if (!names_a_status(status)) {
        return ustl::err(Status::InvArgs);
    }
    if (static_cast<Status>(status) != Status::Ok) {
        // Any failure is the status form, whatever the method returns on Ok:
        // a server never sends half an answer next to an error.
        if (message.bytes.size() != STATUS_FORM_SIZE ||
            load<u32>(message.bytes, STATUS_FORM_PAD_OFFSET) != 0 || !message.handles.empty()) {
            return ustl::err(Status::InvArgs);
        }
    }
    return ustl::ok(static_cast<Status>(status));
}

auto start_message(std::span<u8> const out, usize const size, u32 const txid,
                   Direction const direction, u64 const ordinal)
    -> ustl::Result<std::span<u8>, Status> {
    if (size < HEADER_SIZE || size > MAX_MESSAGE_BYTES) {
        return ustl::err(Status::InvArgs);
    }
    if (out.size() < size) {
        return ustl::err(Status::BufferTooSmall);
    }
    auto const bytes = out.first(size);
    std::memset(bytes.data(), 0, bytes.size());
    store(bytes, 0,
          Header{
              .txid = txid,
              .direction = direction,
              .reserved = {},
              .magic = MAGIC,
              .ordinal = ordinal,
          });
    return ustl::ok(bytes);
}

auto start_request(std::span<u8> const out, usize const size, Method const &method, u32 const txid)
    -> ustl::Result<std::span<u8>, Status> {
    if (!txid_fits(method.kind, txid)) {
        return ustl::err(Status::InvArgs);
    }
    return start_message(out, size, txid, Direction::ToServer, method.ordinal);
}

auto start_ok_reply(std::span<u8> const out, usize const size, Method const &method, u32 const txid)
    -> ustl::Result<std::span<u8>, Status> {
    if (method.kind != MethodKind::TwoWay || !txid_fits(MethodKind::TwoWay, txid) ||
        size < STATUS_FORM_SIZE) {
        return ustl::err(Status::InvArgs);
    }
    // start_message() zeroes the bytes, and Ok is zero: the status is in place.
    static_assert(std::to_underlying(Status::Ok) == 0);
    return start_message(out, size, txid, Direction::ToClient, method.ordinal);
}

auto encode_status_reply(std::span<u8> const out, Method const &method, u32 const txid,
                         Status const status) -> ustl::Result<Message, Status> {
    if (method.kind != MethodKind::TwoWay || !txid_fits(MethodKind::TwoWay, txid)) {
        return ustl::err(Status::InvArgs);
    }
    return encode_status_form(out, txid, method.ordinal, status);
}

auto encode_epitaph(std::span<u8> const out, Status const status) -> ustl::Result<Message, Status> {
    return encode_status_form(out, 0, EPITAPH_ORDINAL, status);
}

auto decode_epitaph(Message const message) -> ustl::Result<Status, Status> {
    auto decoded = decode_header(message.bytes, Direction::ToClient);
    if (decoded.is_err()) {
        return ustl::err(decoded.unwrap_err());
    }
    auto const header = decoded.unwrap();
    if (header.txid != 0 || header.ordinal != EPITAPH_ORDINAL ||
        message.bytes.size() != STATUS_FORM_SIZE || !message.handles.empty() ||
        load<u32>(message.bytes, STATUS_FORM_PAD_OFFSET) != 0) {
        return ustl::err(Status::InvArgs);
    }
    auto const status = load<u32>(message.bytes, STATUS_OFFSET);
    if (!names_a_status(status)) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(static_cast<Status>(status));
}

auto decode_handle(Message const message, usize const offset, usize const index)
    -> ustl::Result<Uandle, Status> {
    if (load<u32>(message.bytes, offset) != HANDLE_PRESENT || index >= message.handles.size() ||
        message.handles[index] == Uandle::Invalid) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(message.handles[index]);
}

auto encode_handle(std::span<u8> const bytes, usize const offset, std::span<Uandle> const handles,
                   usize const index, Uandle const handle) -> Status {
    if (handle == Uandle::Invalid) {
        return Status::InvArgs;
    }
    if (index >= handles.size()) {
        return Status::BufferTooSmall;
    }
    store(bytes, offset, HANDLE_PRESENT);
    handles[index] = handle;
    return Status::Ok;
}
} // namespace wire
