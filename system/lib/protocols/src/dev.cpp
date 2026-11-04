/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <prot/dev.hpp>

#include <cstring>

namespace prot::dev {
namespace {
    using ours::Status;
    using ours::Uandle;
    using wire::Message;

    using Encoded = ustl::Result<Message, Status>;

    // Registry.Publish: the three string sizes, the directory handle's
    // presence marker, then the three strings; the handle rides in
    // handles[0].
    constexpr auto CLASS_SIZE_OFFSET = usize{20};
    constexpr auto NAME_SIZE_OFFSET = usize{24};
    constexpr auto PROTOCOL_SIZE_OFFSET = usize{28};
    constexpr auto HANDLE_OFFSET = usize{32};
    constexpr auto STRINGS_OFFSET = usize{36};
    // The Ok reply: the status at 16, the granted name's size, then the name.
    constexpr auto REPLY_NAME_SIZE_OFFSET = usize{20};
    constexpr auto REPLY_NAME_OFFSET = usize{24};

    /// The message's bytes as a string, for the class and the name.
    auto chars(std::span<u8 const> const bytes) -> std::string_view {
        return {reinterpret_cast<char const *>(bytes.data()), bytes.size()};
    }

    /// A string that may not carry a separator or a NUL: the registry mounts
    /// at class/<class>/<name>, so a class or name naming a path is refused,
    /// and the protocol becomes a path segment in the forward, so it may not
    /// carry one either.
    [[nodiscard]]
    auto well_formed(std::string_view const text, usize const bound) -> bool {
        if (text.empty() || text.size() > bound) {
            return false;
        }
        for (auto const c: text) {
            if (c == '/' || c == '\0') {
                return false;
            }
        }
        return true;
    }
} // namespace

auto encode_publish(std::span<u8> const bytes, std::span<Uandle> const handles,
                    PublishRequest const &request) -> Encoded {
    if (!well_formed(request.klass, MAX_CLASS_LENGTH) ||
        !well_formed(request.name, MAX_NODE_NAME) ||
        !well_formed(request.protocol, MAX_PROTOCOL_NAME)) {
        return ustl::err(Status::InvArgs);
    }
    auto started = wire::start_request(bytes,
                                       STRINGS_OFFSET + request.klass.size() + request.name.size() +
                                           request.protocol.size(),
                                       PUBLISH, request.txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    if (auto const status =
            wire::encode_handle(message, HANDLE_OFFSET, handles, 0, request.directory);
        status != Status::Ok) {
        return ustl::err(status);
    }
    wire::store(message, CLASS_SIZE_OFFSET, static_cast<u32>(request.klass.size()));
    wire::store(message, NAME_SIZE_OFFSET, static_cast<u32>(request.name.size()));
    wire::store(message, PROTOCOL_SIZE_OFFSET, static_cast<u32>(request.protocol.size()));
    auto at = STRINGS_OFFSET;
    for (auto const text: {request.klass, request.name, request.protocol}) {
        if (!text.empty()) {
            std::memcpy(message.data() + at, text.data(), text.size());
        }
        at += text.size();
    }
    return ustl::ok(Message{.bytes = message, .handles = handles.first(1)});
}

auto decode_publish(Message const message) -> ustl::Result<PublishRequest, Status> {
    auto header = wire::decode_request_header(message, PUBLISH);
    if (header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() < STRINGS_OFFSET || message.handles.size() != 1) {
        return ustl::err(Status::InvArgs);
    }
    auto const class_size = wire::load<u32>(message.bytes, CLASS_SIZE_OFFSET);
    auto const name_size = wire::load<u32>(message.bytes, NAME_SIZE_OFFSET);
    auto const protocol_size = wire::load<u32>(message.bytes, PROTOCOL_SIZE_OFFSET);
    if (class_size > MAX_CLASS_LENGTH || name_size > MAX_NODE_NAME ||
        protocol_size > MAX_PROTOCOL_NAME ||
        message.bytes.size() != STRINGS_OFFSET + class_size + name_size + protocol_size) {
        return ustl::err(Status::InvArgs);
    }
    auto klass = chars(message.bytes.subspan(STRINGS_OFFSET, class_size));
    auto name = chars(message.bytes.subspan(STRINGS_OFFSET + class_size, name_size));
    auto protocol =
        chars(message.bytes.subspan(STRINGS_OFFSET + class_size + name_size, protocol_size));
    if (!well_formed(klass, MAX_CLASS_LENGTH) || !well_formed(name, MAX_NODE_NAME) ||
        !well_formed(protocol, MAX_PROTOCOL_NAME)) {
        return ustl::err(Status::InvArgs);
    }
    auto directory = wire::decode_handle(message, HANDLE_OFFSET, 0);
    if (directory.is_err()) {
        return ustl::err(Status::InvArgs);
    }

    return ustl::ok(PublishRequest{
        .txid = header->txid,
        .klass = klass,
        .name = name,
        .protocol = protocol,
        .directory = directory.unwrap(),
    });
}

auto encode_publish_reply(std::span<u8> const bytes, u32 const txid, PublishReply const &reply)
    -> Encoded {
    if (reply.status != Status::Ok) {
        // A failure is the bare status form: no name follows a refusal.
        return wire::encode_status_reply(bytes, PUBLISH, txid, reply.status);
    }
    if (reply.name.empty() || reply.name.size() > MAX_NODE_NAME) {
        return ustl::err(Status::InvArgs);
    }
    auto started = wire::start_ok_reply(
        bytes, REPLY_NAME_SIZE_OFFSET + sizeof(u32) + reply.name.size(), PUBLISH, txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    wire::store(message, REPLY_NAME_SIZE_OFFSET, static_cast<u32>(reply.name.size()));
    std::memcpy(message.data() + REPLY_NAME_OFFSET, reply.name.data(), reply.name.size());
    return ustl::ok(Message{.bytes = message, .handles = {}});
}

auto decode_publish_reply(Message const message, u32 const txid)
    -> ustl::Result<PublishReply, Status> {
    auto status = wire::decode_reply_status(message, PUBLISH, txid);
    if (status.is_err()) {
        return ustl::err(status.unwrap_err());
    }
    if (status.unwrap() != Status::Ok) {
        // The bare status form: a refusal names nothing.
        return ustl::ok(PublishReply{.status = status.unwrap(), .name = {}});
    }
    // The Ok reply carries the name and nothing else: no handle rides with
    // it, and one that does is not this protocol's reply.
    if (message.bytes.size() < REPLY_NAME_OFFSET || !message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    auto const name_size = wire::load<u32>(message.bytes, REPLY_NAME_SIZE_OFFSET);
    if (name_size == 0 || name_size > MAX_NODE_NAME ||
        message.bytes.size() != REPLY_NAME_OFFSET + name_size) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(PublishReply{
        .status = Status::Ok,
        .name = chars(message.bytes.subspan(REPLY_NAME_OFFSET, name_size)),
    });
}
} // namespace prot::dev
