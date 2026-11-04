/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <prot/resource_client.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

#include <fsio/fsio.hpp>
#include <ours/signals.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
#include <rpc/rpc.hpp>
#include <wire/wire.hpp>

namespace prot::resource {
using ours::Status;
using ours::Uandle;

auto get(Uandle const connection, Protocol const &protocol) -> ustl::Result<Uandle, Status> {
    // The one request on this connection, so any txid will do.
    constexpr auto TXID = u32{1};
    std::array<u8, wire::STATUS_FORM_SIZE> request_bytes{};
    // The request always fits the buffer and the txid is fixed: the encode
    // cannot refuse.
    auto const request = encode_get(request_bytes, protocol, {.txid = TXID});
    // The pipeline, handles intact: the resource is the reply's payload.
    // The reply lands in this frame's buffers -- a view of the pipeline's
    // own stack would die at its return.
    auto reply_bytes = std::array<u8, wire::MAX_MESSAGE_BYTES>{};
    auto reply_handles = std::array<Uandle, wire::MAX_MESSAGE_HANDLES>{};
    auto const message = rpc::exchange(connection, request.unwrap(), reply_bytes, reply_handles);
    if (message.is_err()) {
        return ustl::err(message.unwrap_err());
    }
    auto const close_handles = [&message] {
        for (auto const handle: message->handles) {
            auto _ = ours::handle_close(handle);
        }
    };
    auto const header = wire::decode_header(message->bytes, wire::Direction::ToClient);
    if (header.is_err()) {
        close_handles();
        return ustl::err(Status::InvArgs);
    }
    // An epitaph names the failure: an Ok one says nothing about why, and
    // reads as the connection closing, as fsio reads one.
    if (wire::is_epitaph(header.unwrap())) {
        close_handles();
        auto const epitaph = wire::decode_epitaph(*message);
        if (epitaph.is_err()) {
            return ustl::err(Status::InvArgs);
        }
        return ustl::err(epitaph.unwrap() == Status::Ok ? Status::PeerClosed : epitaph.unwrap());
    }
    auto const decoded = decode_get_reply(*message, protocol, TXID);
    if (decoded.is_err()) {
        close_handles();
        return ustl::err(Status::InvArgs);
    }
    if (decoded->status != Status::Ok) {
        return ustl::err(decoded->status);
    }
    return ustl::ok(decoded->resource);
}

auto get(Protocol const &protocol) -> ustl::Result<Uandle, Status> {
    // Every protocol use lives in /svc, under the use's name.
    constexpr auto SVC = std::string_view{"/svc/"};
    std::array<char, 128> path_bytes{};
    if (SVC.size() + protocol.name.size() > path_bytes.size()) {
        return ustl::err(Status::InvArgs);
    }
    std::ranges::copy(protocol.name, std::ranges::copy(SVC, path_bytes.begin()).out);
    auto const path = std::string_view{path_bytes.data(), SVC.size() + protocol.name.size()};

    auto client = Uandle::Invalid;
    auto server = Uandle::Invalid;
    if (auto const status =
            ours::channel_create(ours::ChannelCreateOptions::None, &client, &server);
        status != Status::Ok) {
        return ustl::err(status);
    }
    if (auto const status = fsio::connect(path, server); status != Status::Ok) {
        auto _ = ours::handle_close(client);
        return ustl::err(status);
    }
    auto const resource = get(client, protocol);
    auto _ = ours::handle_close(client);
    return resource;
}
} // namespace prot::resource
