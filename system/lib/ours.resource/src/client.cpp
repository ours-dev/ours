/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours_resource/client.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

#include <fsio/fsio.hpp>
#include <ours/signals.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
#include <wire/wire.hpp>

namespace ours_resource {
using ours::Status;
using ours::Uandle;

auto get(Uandle const connection, Protocol const &protocol) -> ustl::Result<Uandle, Status> {
    // The one request on this connection, so any txid will do.
    constexpr auto TXID = u32{1};
    std::array<u8, wire::STATUS_FORM_SIZE> request_bytes{};
    auto const request = encode_get(request_bytes, protocol, {.txid = TXID});
    if (request.is_err()) {
        return ustl::err(request.unwrap_err());
    }
    // A write that finds the connection closed is not the answer: the
    // epitaph the root closed it with is, and it is still there to read.
    if (auto const written =
            ours::channel_write(connection, ours::ChannelWriteOptions::None, request->bytes.data(),
                                static_cast<u32>(request->bytes.size()), nullptr, 0);
        written != Status::Ok && written != Status::PeerClosed) {
        return ustl::err(written);
    }
    auto observed = ours::Signals::None;
    if (auto const status =
            ours::object_wait_one(connection, ours::Signals::Readable | ours::Signals::PeerClosed,
                                  ours::INSTANT_INFINITE, &observed);
        status != Status::Ok) {
        return ustl::err(status);
    }
    if (!(observed & ours::Signals::Readable)) {
        return ustl::err(Status::PeerClosed);
    }

    // Either answer, an epitaph or the reply, is the 24-byte status form or
    // as long, with at most the resource in it.
    std::array<u8, wire::STATUS_FORM_SIZE> bytes{};
    std::array<Uandle, 1> handles{};
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    if (auto const status = ours::channel_read(connection, ours::ChannelReadOptions::None,
                                               bytes.data(), handles.data(), bytes.size(),
                                               handles.size(), &num_bytes, &num_handles);
        status != Status::Ok) {
        return ustl::err(status);
    }
    auto const message = wire::Message{
        .bytes = std::span{bytes}.first(num_bytes),
        .handles = std::span{handles}.first(num_handles),
    };
    auto const close_handles = [&message] {
        for (auto const handle: message.handles) {
            auto _ = ours::handle_close(handle);
        }
    };
    if (auto const header = wire::decode_header(message.bytes, wire::Direction::ToClient);
        header.is_ok() && wire::is_epitaph(header.unwrap())) {
        close_handles();
        auto const epitaph = wire::decode_epitaph(message);
        if (epitaph.is_err()) {
            return ustl::err(Status::InvArgs);
        }
        // An Ok epitaph says nothing about why; it reads as the connection
        // closing, as fsio reads one.
        if (epitaph.unwrap() == Status::Ok) {
            return ustl::err(Status::PeerClosed);
        }
        return ustl::err(epitaph.unwrap());
    }
    auto const decoded = decode_get_reply(message, protocol, TXID);
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
} // namespace ours_resource
