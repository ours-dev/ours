/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours_acpi/client.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

#include <fsio/fsio.hpp>
#include <ours/signals.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
#include <wire/wire.hpp>

namespace ours_acpi {
namespace {
    using ours::Status;
    using ours::Uandle;

    // The one request on a connection, so any txid will do.
    constexpr auto TXID = u32{1};

    // Room for the largest request and reply, with some to spare: 20 and 40
    // bytes.
    constexpr auto REQUEST_ROOM = usize{32};
    constexpr auto REPLY_ROOM = usize{48};

    /// Writes |request| on |connection| and reads the one message that answers
    /// it into |reply|: the message's length, or why not. A message that is an
    /// epitaph is its status.
    auto exchange(Uandle const connection, wire::Message const request, std::span<u8> const reply)
        -> ustl::Result<usize, Status> {
        // A write that finds the connection closed is not the answer: the
        // epitaph the route closed it with is, and it is still there to read.
        if (auto const written = ours::channel_write(
                connection, ours::ChannelWriteOptions::None, request.bytes.data(),
                static_cast<u32>(request.bytes.size()), nullptr, 0);
            written != Status::Ok && written != Status::PeerClosed) {
            return ustl::err(written);
        }
        auto observed = ours::Signals::None;
        if (auto const status = ours::object_wait_one(
                connection, ours::Signals::Readable | ours::Signals::PeerClosed,
                ours::INSTANT_INFINITE, &observed);
            status != Status::Ok) {
            return ustl::err(status);
        }
        if (!(observed & ours::Signals::Readable)) {
            return ustl::err(Status::PeerClosed);
        }

        // A reply carries no handle. Room for one all the same, so that a
        // message that brings one is read, and its handle closed, and then
        // refused.
        std::array<Uandle, 1> handles{};
        auto num_bytes = u32{0};
        auto num_handles = u32{0};
        if (auto const status =
                ours::channel_read(connection, ours::ChannelReadOptions::None, reply.data(),
                                   handles.data(), static_cast<u32>(reply.size()),
                                   static_cast<u32>(handles.size()), &num_bytes, &num_handles);
            status != Status::Ok) {
            return ustl::err(status);
        }
        for (auto index = u32{0}; index < num_handles; ++index) {
            auto _ = ours::handle_close(handles[index]);
        }
        if (num_handles != 0) {
            return ustl::err(Status::InvArgs);
        }

        auto const message = wire::Message{.bytes = reply.first(num_bytes), .handles = {}};
        if (auto const header = wire::decode_header(message.bytes, wire::Direction::ToClient);
            header.is_ok() && wire::is_epitaph(header.unwrap())) {
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
        return ustl::ok(usize{num_bytes});
    }

    /// One call: a connection of its own to the route service, |request|
    /// written to it, and the answer read into |reply|; the connection closes
    /// after.
    auto call(wire::Message const request, std::span<u8> const reply)
        -> ustl::Result<usize, Status> {
        // Every protocol use lives in /svc, under the use's name.
        constexpr auto SVC = std::string_view{"/svc/"};
        std::array<char, 64> path_bytes{};
        if (SVC.size() + ROUTE_NAME.size() > path_bytes.size()) {
            return ustl::err(Status::InvArgs);
        }
        std::ranges::copy(ROUTE_NAME, std::ranges::copy(SVC, path_bytes.begin()).out);
        auto const path = std::string_view{path_bytes.data(), SVC.size() + ROUTE_NAME.size()};

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
        auto const length = exchange(client, request, reply);
        auto _ = ours::handle_close(client);
        return length;
    }
} // namespace

auto isa_irq(u32 const isa_irq) -> ustl::Result<IsaIrqReply, Status> {
    std::array<u8, REQUEST_ROOM> request_bytes{};
    auto const request = encode_isa_irq(request_bytes, {.txid = TXID, .isa_irq = isa_irq});
    if (request.is_err()) {
        return ustl::err(request.unwrap_err());
    }
    std::array<u8, REPLY_ROOM> reply_bytes{};
    auto const length = call(request.unwrap(), reply_bytes);
    if (length.is_err()) {
        return ustl::err(length.unwrap_err());
    }
    auto const reply = decode_isa_irq_reply(
        wire::Message{.bytes = std::span{reply_bytes}.first(length.unwrap()), .handles = {}}, TXID);
    if (reply.is_err()) {
        return ustl::err(Status::InvArgs);
    }
    if (reply->status != Status::Ok) {
        return ustl::err(reply->status);
    }
    return ustl::ok(reply.unwrap());
}

auto ecam_base(u16 const segment) -> ustl::Result<EcamBaseReply, Status> {
    std::array<u8, REQUEST_ROOM> request_bytes{};
    auto const request = encode_ecam_base(request_bytes, {.txid = TXID, .segment = segment});
    if (request.is_err()) {
        return ustl::err(request.unwrap_err());
    }
    std::array<u8, REPLY_ROOM> reply_bytes{};
    auto const length = call(request.unwrap(), reply_bytes);
    if (length.is_err()) {
        return ustl::err(length.unwrap_err());
    }
    auto const reply = decode_ecam_base_reply(
        wire::Message{.bytes = std::span{reply_bytes}.first(length.unwrap()), .handles = {}}, TXID);
    if (reply.is_err()) {
        return ustl::err(Status::InvArgs);
    }
    if (reply->status != Status::Ok) {
        return ustl::err(reply->status);
    }
    return ustl::ok(reply.unwrap());
}
} // namespace ours_acpi
