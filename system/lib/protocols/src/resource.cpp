/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <prot/resource.hpp>

namespace prot::resource {
namespace {
    using ours::Status;
    using ours::Uandle;
    using wire::Message;

    using Encoded = ustl::Result<Message, Status>;

    // The Ok reply: the resource after the status.
    constexpr auto REPLY_RESOURCE_OFFSET = usize{20};

    static_assert(wire::STATUS_OFFSET + sizeof(u32) == REPLY_RESOURCE_OFFSET);
} // namespace

auto encode_get(std::span<u8> const bytes, Protocol const &protocol, GetRequest const &request)
    -> Encoded {
    auto started = wire::start_request(bytes, wire::HEADER_SIZE, protocol.get, request.txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    return ustl::ok(Message{.bytes = started.unwrap(), .handles = {}});
}

auto decode_get(Message const message, Protocol const &protocol)
    -> ustl::Result<GetRequest, Status> {
    // The ordinal is the protocol's own, so another protocol's Get, which is
    // the same bytes with another ordinal, is refused here.
    auto header = wire::decode_request_header(message, protocol.get);
    if (header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    if (message.bytes.size() != wire::HEADER_SIZE || !message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(GetRequest{.txid = header->txid});
}

auto encode_get_reply(std::span<u8> const bytes, std::span<Uandle> const handles,
                      Protocol const &protocol, u32 const txid, GetReply const &reply) -> Encoded {
    if (reply.status != Status::Ok) {
        if (reply.resource != Uandle::Invalid) {
            return ustl::err(Status::InvArgs);
        }
        return wire::encode_status_reply(bytes, protocol.get, txid, reply.status);
    }
    auto started = wire::start_ok_reply(bytes, wire::STATUS_FORM_SIZE, protocol.get, txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    auto const message = started.unwrap();
    if (auto const status =
            wire::encode_handle(message, REPLY_RESOURCE_OFFSET, handles, 0, reply.resource);
        status != Status::Ok) {
        return ustl::err(status);
    }
    return ustl::ok(Message{.bytes = message, .handles = handles.first(1)});
}

auto decode_get_reply(Message const message, Protocol const &protocol, u32 const txid)
    -> ustl::Result<GetReply, Status> {
    auto status = wire::decode_reply_status(message, protocol.get, txid);
    if (status.is_err()) {
        return ustl::err(status.unwrap_err());
    }
    if (status.unwrap() != Status::Ok) {
        return ustl::ok(GetReply{.status = status.unwrap(), .resource = Uandle::Invalid});
    }
    if (message.bytes.size() != wire::STATUS_FORM_SIZE || message.handles.size() != 1) {
        return ustl::err(Status::InvArgs);
    }
    auto resource = wire::decode_handle(message, REPLY_RESOURCE_OFFSET, 0);
    if (resource.is_err()) {
        return ustl::err(resource.unwrap_err());
    }
    return ustl::ok(GetReply{.status = Status::Ok, .resource = resource.unwrap()});
}
} // namespace prot::resource
