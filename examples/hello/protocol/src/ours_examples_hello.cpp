/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours_examples_hello/ours_examples_hello.hpp>

#include <cstring>

namespace ours_examples_hello {
namespace {
    using ours::Status;
    using wire::Message;

    using Encoded = ustl::Result<Message, Status>;

    // Greet's request: the size, then the string.
    constexpr auto REQUEST_SIZE_OFFSET = usize{16};
    constexpr auto REQUEST_STRING_OFFSET = usize{20};

    // Its Ok reply: the status, the size, then the string.
    constexpr auto REPLY_SIZE_OFFSET = usize{20};
    constexpr auto REPLY_STRING_OFFSET = usize{24};

    static_assert(wire::HEADER_SIZE == REQUEST_SIZE_OFFSET);
    static_assert(wire::STATUS_OFFSET + sizeof(u32) == REPLY_SIZE_OFFSET);
    static_assert(REPLY_STRING_OFFSET + MAX_STRING_LENGTH <= wire::MAX_MESSAGE_BYTES);

    /// Writes |text|, at most MAX_STRING_LENGTH bytes, as a size at
    /// |size_offset| and the bytes right after it, into |message|, which
    /// start_request() or start_ok_reply() sized for them.
    auto store_string(std::span<u8> const message, usize const size_offset,
                      std::string_view const text) -> void {
        wire::store(message, size_offset, static_cast<u32>(text.size()));
        // An empty view may have no data pointer, which memcpy must not be
        // given even for zero bytes.
        if (!text.empty()) {
            std::memcpy(message.data() + size_offset + sizeof(u32), text.data(), text.size());
        }
    }

    /// The string a message of exactly the right size holds after its size
    /// field at |size_offset|: the size must be at most MAX_STRING_LENGTH
    /// and account for every byte after the field, so that nothing trails
    /// it. InvArgs otherwise.
    auto load_string(std::span<u8 const> const bytes, usize const size_offset)
        -> ustl::Result<std::string_view, Status> {
        auto const start = size_offset + sizeof(u32);
        if (bytes.size() < start) {
            return ustl::err(Status::InvArgs);
        }
        auto const size = usize{wire::load<u32>(bytes, size_offset)};
        if (size > MAX_STRING_LENGTH || bytes.size() != start + size) {
            return ustl::err(Status::InvArgs);
        }
        return ustl::ok(
            std::string_view{reinterpret_cast<char const *>(bytes.data() + start), size});
    }
} // namespace

auto encode_greet(std::span<u8> const bytes, GreetRequest const &request) -> Encoded {
    if (request.value.size() > MAX_STRING_LENGTH) {
        return ustl::err(Status::InvArgs);
    }
    auto started = wire::start_request(bytes, REQUEST_STRING_OFFSET + request.value.size(), GREET,
                                       request.txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    store_string(message, REQUEST_SIZE_OFFSET, request.value);
    return ustl::ok(Message{.bytes = message, .handles = {}});
}

auto decode_greet(Message const message) -> ustl::Result<GreetRequest, Status> {
    auto header = wire::decode_request_header(message, GREET);
    if (header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (!message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    auto value = load_string(message.bytes, REQUEST_SIZE_OFFSET);
    if (value.is_err()) {
        return ustl::err(value.unwrap_err());
    }
    return ustl::ok(GreetRequest{.txid = header->txid, .value = value.unwrap()});
}

auto encode_stop(std::span<u8> const bytes, StopRequest const &request) -> Encoded {
    auto started = wire::start_request(bytes, wire::HEADER_SIZE, STOP, request.txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    return ustl::ok(Message{.bytes = started.unwrap(), .handles = {}});
}

auto decode_stop(Message const message) -> ustl::Result<StopRequest, Status> {
    auto header = wire::decode_request_header(message, STOP);
    if (header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() != wire::HEADER_SIZE || !message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(StopRequest{.txid = header->txid});
}

auto encode_greet_reply(std::span<u8> const bytes, u32 const txid, GreetReply const &reply)
    -> Encoded {
    if (reply.status != Status::Ok) {
        if (!reply.response.empty()) {
            return ustl::err(Status::InvArgs);
        }
        return wire::encode_status_reply(bytes, GREET, txid, reply.status);
    }
    if (reply.response.size() > MAX_STRING_LENGTH) {
        return ustl::err(Status::InvArgs);
    }
    auto started =
        wire::start_ok_reply(bytes, REPLY_STRING_OFFSET + reply.response.size(), GREET, txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    store_string(message, REPLY_SIZE_OFFSET, reply.response);
    return ustl::ok(Message{.bytes = message, .handles = {}});
}

auto decode_greet_reply(Message const message, u32 const txid) -> ustl::Result<GreetReply, Status> {
    auto status = wire::decode_reply_status(message, GREET, txid);
    if (status.is_err()) {
        return ustl::err(status.unwrap_err());
    }
    if (status.unwrap() != Status::Ok) {
        return ustl::ok(GreetReply{.status = status.unwrap()});
    }
    if (!message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    auto response = load_string(message.bytes, REPLY_SIZE_OFFSET);
    if (response.is_err()) {
        return ustl::err(response.unwrap_err());
    }
    return ustl::ok(GreetReply{.status = Status::Ok, .response = response.unwrap()});
}

auto encode_stop_reply(std::span<u8> const bytes, u32 const txid, Status const status) -> Encoded {
    return wire::encode_status_reply(bytes, STOP, txid, status);
}

auto decode_stop_reply(Message const message, u32 const txid) -> ustl::Result<Status, Status> {
    auto status = wire::decode_reply_status(message, STOP, txid);
    if (status.is_err()) {
        return ustl::err(status.unwrap_err());
    }
    // decode_reply_status() holds anything but Ok to the status form; an Ok
    // reply of a method that returns nothing is that form too, zero in its
    // last four bytes included.
    if (message.bytes.size() != wire::STATUS_FORM_SIZE || !message.handles.empty() ||
        wire::load<u32>(message.bytes, REPLY_SIZE_OFFSET) != 0) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(status.unwrap());
}
} // namespace ours_examples_hello
