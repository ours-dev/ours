/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <prot/virtio_control.hpp>

namespace prot::virtio {
using ours::Status;
using wire::Message;

namespace {
    using Encoded = ustl::Result<Message, Status>;
} // namespace

auto encode_rescan(std::span<u8> const bytes, RescanRequest const &request) -> Encoded {
    auto started = wire::start_request(bytes, wire::HEADER_SIZE, CONTROL_RESCAN, request.txid);
    if (started.is_err()) {
        return ustl::err(started.unwrap_err());
    }
    return ustl::ok(Message{.bytes = started.unwrap(), .handles = {}});
}

auto decode_rescan(Message const message) -> ustl::Result<RescanRequest, Status> {
    auto header = wire::decode_request_header(message, CONTROL_RESCAN);
    if (header.is_err()) {
        return ustl::err(header.unwrap_err());
    }
    // Empty by contract: a payload byte or a handle on a rescan is not one.
    if (message.bytes.size() != wire::HEADER_SIZE || !message.handles.empty()) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(RescanRequest{.txid = header->txid});
}

auto encode_rescan_reply(std::span<u8> const bytes, u32 const txid, RescanReply const &reply)
    -> Encoded {
    return wire::encode_status_reply(bytes, CONTROL_RESCAN, txid, reply.status);
}

auto decode_rescan_reply(Message const message, u32 const txid)
    -> ustl::Result<RescanReply, Status> {
    auto status = wire::decode_reply_status(message, CONTROL_RESCAN, txid);
    if (status.is_err()) {
        return ustl::err(status.unwrap_err());
    }
    return ustl::ok(RescanReply{.status = status.unwrap()});
}
} // namespace prot::virtio
