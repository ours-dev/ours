/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "debug_resource_server.hpp"

#include <ours/rights.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

#include <span>

#include <fuchsia_kernel/fuchsia_kernel.hpp>
#include <ort/debug.hpp>

namespace init {
namespace {
    using ours::Signals;
    using ours::Status;
    using ours::Uandle;

    /// What a connection's slot waits for: a message, or the client gone.
    constexpr auto CONNECTION_SIGNALS = Signals::Readable | Signals::PeerClosed;

    /// Closes every handle of |handles|: the ones a dropped message carried,
    /// which nothing took.
    auto close_all(std::span<Uandle const> const handles) -> void {
        for (auto const handle: handles) {
            // Each was this process's; nothing can be done about a close
            // that fails.
            auto _ = ours::handle_close(handle);
        }
    }
} // namespace

DebugResourceServer::DebugResourceServer(Storage &storage, loop::Loop &loop, Uandle const resource)
    : m_storage(&storage), m_loop(&loop), m_resource(resource) {}

auto DebugResourceServer::bind(Uandle const channel) -> Status {
    for (auto &connection: m_storage->connections) {
        if (connection.m_channel != Uandle::Invalid) {
            continue;
        }
        connection.m_server = this;
        connection.m_channel = channel;
        auto const added =
            m_loop->add(channel, CONNECTION_SIGNALS, &DebugResourceServer::on_signals, &connection);
        if (added.is_err()) {
            connection.m_channel = Uandle::Invalid;
            return added.unwrap_err();
        }
        return Status::Ok;
    }
    return Status::NoResource;
}

auto DebugResourceServer::on_signals(void *const context, Signals const observed) -> loop::Action {
    auto &connection = *static_cast<DebugResourceConnection *>(context);
    return connection.m_server->handle(connection, observed);
}

auto DebugResourceServer::handle(DebugResourceConnection &connection, Signals const observed)
    -> loop::Action {
    // The entry is free again once its slot closes; the slot closes the
    // channel (I3).
    auto const release = [&connection] {
        connection.m_channel = Uandle::Invalid;
        return loop::Action::Close;
    };
    if (!(observed & Signals::Readable)) {
        return release();
    }

    // One message per callback, then re-arm (I4).
    auto &storage = *m_storage;
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    if (auto const read = ours::channel_read(
            connection.m_channel, ours::ChannelReadOptions::None, storage.request_bytes.data(),
            storage.request_handles.data(), static_cast<u32>(storage.request_bytes.size()),
            static_cast<u32>(storage.request_handles.size()), &num_bytes, &num_handles);
        read != Status::Ok) {
        return release();
    }
    auto const message = wire::Message{
        .bytes = std::span{storage.request_bytes}.first(num_bytes),
        .handles = std::span{storage.request_handles}.first(num_handles),
    };

    // DebugResource is a closed protocol with one method: anything but a
    // well-formed Get -- a reply, another ordinal, a Get with handles --
    // closes the connection without an epitaph, and every handle it brought.
    auto const request = fuchsia_kernel::decode_debug_resource_get(message);
    if (request.is_err()) {
        close_all(message.handles);
        return release();
    }

    // A duplicate of the root's resource, as component_manager's builtin
    // duplicates its own for every Get. When it cannot -- the root was
    // started without one, say -- the reply's status says why, which
    // Fuchsia's reply, the resource alone, has no room for.
    auto resource = Uandle::Invalid;
    auto reply = fuchsia_kernel::DebugResourceGetReply{.status = Status::Ok};
    if (auto const status = ours::handle_duplicate(m_resource, ours::Rights::SameRights, &resource);
        status != Status::Ok) {
        reply.status = status;
    } else {
        reply.resource = resource;
    }
    auto const encoded = fuchsia_kernel::encode_debug_resource_get_reply(
        storage.reply_bytes, storage.reply_handles, request->txid, reply);
    if (encoded.is_err()) {
        // Cannot happen: the txid passed the decoder, and the buffers hold
        // either form. Were it to, the client would wait for good.
        if (resource != Uandle::Invalid) {
            auto _ = ours::handle_close(resource);
        }
        return release();
    }
    auto const answer = encoded.unwrap();
    // The resource goes with the reply, whatever the write answers.
    if (auto const written =
            ours::channel_write(connection.m_channel, ours::ChannelWriteOptions::None,
                                answer.bytes.data(), static_cast<u32>(answer.bytes.size()),
                                answer.handles.data(), static_cast<u32>(answer.handles.size()));
        written != Status::Ok) {
        // I1: the client would wait for an answer that never comes.
        auto _ = ort::debug_line(
            {"init: DebugResource cannot answer a client: ", ours::to_string(written)});
        return release();
    }
    return loop::Action::Rearm;
}
} // namespace init
