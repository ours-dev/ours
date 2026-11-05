/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <prot/acpi_client.hpp>

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

namespace prot::acpi {
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
    /// it into |reply|: the message's length, or why not. The pipeline is
    /// rpc::exchange's -- handles intact, epitaphs untranslated -- and this
    /// family's contract goes on top: a reply carries no handle, and an
    /// epitaph is its status.
    auto exchange(Uandle const connection, wire::Message const request, std::span<u8> const reply)
        -> ustl::Result<usize, Status> {
        // Room for one handle all the same, so that a message that brings
        // one is read, and its handle closed, and then refused.
        auto handles = std::array<Uandle, 1>{};
        auto const message = rpc::exchange(connection, request, reply, handles);
        if (message.is_err()) {
            return ustl::err(message.unwrap_err());
        }
        for (auto const handle: message->handles) {
            auto _ = ours::handle_close(handle);
        }
        if (!message->handles.empty()) {
            return ustl::err(Status::InvArgs);
        }

        auto const header = wire::decode_header(message->bytes, wire::Direction::ToClient);
        if (header.is_ok() && wire::is_epitaph(header.unwrap())) {
            auto const epitaph = wire::decode_epitaph(*message);
            if (epitaph.is_err()) {
                return ustl::err(Status::InvArgs);
            }
            // An Ok epitaph says nothing about why; it reads as the connection
            // closing, as fsio reads one.
            return ustl::err(epitaph.unwrap() == Status::Ok ? Status::PeerClosed
                                                            : epitaph.unwrap());
        }
        return ustl::ok(message->bytes.size());
    }

    /// A connection of this process's own to the route service, from /svc:
    /// the client end, or why not. Every protocol use lives in /svc, under
    /// the use's name.
    auto connect_route() -> ustl::Result<Uandle, Status> {
        constexpr auto SVC = std::string_view{"/svc/"};
        static_assert(SVC.size() + ROUTE_NAME.size() <= 64, "the route path always fits");
        std::array<char, 64> path_bytes{};
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
        return ustl::ok(client);
    }
} // namespace

auto isa_irq(Uandle const connection, u32 const isa_irq) -> ustl::Result<IsaIrqReply, Status> {
    std::array<u8, REQUEST_ROOM> request_bytes{};
    // The request always fits and the txid is fixed: the encode cannot refuse.
    auto const request = encode_isa_irq(request_bytes, {.txid = TXID, .isa_irq = isa_irq});
    std::array<u8, REPLY_ROOM> reply_bytes{};
    auto const length = exchange(connection, request.unwrap(), reply_bytes);
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

auto ecam_base(Uandle const connection, u16 const segment) -> ustl::Result<EcamBaseReply, Status> {
    std::array<u8, REQUEST_ROOM> request_bytes{};
    // The request always fits and the txid is fixed: the encode cannot refuse.
    auto const request = encode_ecam_base(request_bytes, {.txid = TXID, .segment = segment});
    std::array<u8, REPLY_ROOM> reply_bytes{};
    auto const length = exchange(connection, request.unwrap(), reply_bytes);
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

auto isa_irq(u32 const isa_irq) -> ustl::Result<IsaIrqReply, Status> {
    auto const connection = connect_route();
    if (connection.is_err()) {
        return ustl::err(connection.unwrap_err());
    }
    // The parameter shares the function's name, so the call on the fresh
    // connection is spelled qualified.
    auto const reply = prot::acpi::isa_irq(connection.unwrap(), isa_irq);
    auto _ = ours::handle_close(connection.unwrap());
    return reply;
}

auto ecam_base(u16 const segment) -> ustl::Result<EcamBaseReply, Status> {
    auto const connection = connect_route();
    if (connection.is_err()) {
        return ustl::err(connection.unwrap_err());
    }
    auto const reply = ecam_base(connection.unwrap(), segment);
    auto _ = ours::handle_close(connection.unwrap());
    return reply;
}
} // namespace prot::acpi
