/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <svc/outgoing.hpp>

#include <span>

#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

namespace svc {
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

    /// Writes |message| on |channel|; what channel_write answers. The
    /// message's handles are consumed either way.
    auto write(Uandle const channel, wire::Message const message) -> Status {
        return ours::channel_write(channel, ours::ChannelWriteOptions::None, message.bytes.data(),
                                   static_cast<u32>(message.bytes.size()), message.handles.data(),
                                   static_cast<u32>(message.handles.size()));
    }
} // namespace

Outgoing::Outgoing(Storage &storage, loop::Loop &loop)
    : m_storage(&storage)
    , m_loop(&loop)
    , m_tree(storage.nodes)
    , m_server(m_tree, storage.connections,
               vfs::Syscalls{.handle_duplicate = ours::handle_duplicate})
    // The tree's first two nodes, which Storage sizes room for: neither add
    // can fail, and unwrap() traps if one ever does.
    , m_root(m_tree.add_root().unwrap())
    , m_svc(m_tree.add_directory(m_root, SVC_DIRECTORY).unwrap()) {}

auto Outgoing::add_protocol(std::string_view const name, Handler const handler) -> Status {
    if (handler.connect == nullptr) {
        return Status::InvArgs;
    }
    if (m_protocols == MAX_PROTOCOLS) {
        return Status::NoResource;
    }
    // The service node's value is the handler's index, which handle() takes
    // back. The handler is stored only once the node is in, so a refused
    // name leaves nothing behind.
    auto const added = m_tree.add_service(m_svc, name, m_protocols);
    if (added.is_err()) {
        return added.unwrap_err();
    }
    m_storage->handlers[m_protocols] = handler;
    m_protocols += 1;
    return Status::Ok;
}

auto Outgoing::serve(Uandle const directory_request) -> Status {
    auto const bound = m_server.bind(directory_request, m_root, OUTGOING_RIGHTS);
    if (bound.is_err()) {
        return bound.unwrap_err();
    }
    return watch(directory_request, bound.unwrap());
}

auto Outgoing::watch(Uandle const channel, vfs::ConnectionId const connection) -> Status {
    auto &binding = m_storage->bindings[connection.index];
    binding = Binding{.outgoing = this, .connection = connection};
    auto const added = m_loop->add(channel, CONNECTION_SIGNALS, &Outgoing::on_signals, &binding);
    if (added.is_err()) {
        // The Loop did not take the channel, so the connection must not
        // outlive this answer either.
        m_server.release(connection);
        return added.unwrap_err();
    }
    return Status::Ok;
}

auto Outgoing::on_signals(void *const context, Signals const observed) -> loop::Action {
    auto const *const binding = static_cast<Binding const *>(context);
    return binding->outgoing->handle(binding->connection, observed);
}

auto Outgoing::handle(vfs::ConnectionId const connection, Signals const observed) -> loop::Action {
    auto const channel = m_server.channel(connection);
    if (channel.is_none()) {
        // Nothing releases a connection without closing its slot in the same
        // callback, so this is a slot outliving its connection: close it.
        return loop::Action::Close;
    }
    // The client went away with nothing left to read. The slot's own
    // callback closes its handle, by answering Close (I3).
    if (!(observed & Signals::Readable)) {
        m_server.release(connection);
        return loop::Action::Close;
    }

    // One message per callback, then re-arm (I4).
    auto &storage = *m_storage;
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    if (auto const read = ours::channel_read(
            *channel, ours::ChannelReadOptions::None, storage.request_bytes.data(),
            storage.request_handles.data(), static_cast<u32>(storage.request_bytes.size()),
            static_cast<u32>(storage.request_handles.size()), &num_bytes, &num_handles);
        read != Status::Ok) {
        // The buffers hold the largest message a channel carries, so this is
        // the channel failing, not the message: nothing more can be served
        // on it.
        m_server.release(connection);
        return loop::Action::Close;
    }
    auto const message = wire::Message{
        .bytes = std::span{storage.request_bytes}.first(num_bytes),
        .handles = std::span{storage.request_handles}.first(num_handles),
    };

    auto const outcome = m_server.dispatch(connection, message, storage.reply);
    switch (outcome.action) {
        case vfs::Action::Reply: {
            // A write to a full channel fails that write alone (I1). A reply
            // that cannot be written leaves its client waiting for good, so
            // the connection goes with it.
            if (auto const written = write(*channel, outcome.message); written != Status::Ok) {
                m_server.release(connection);
                return loop::Action::Close;
            }
            return loop::Action::Rearm;
        }
        case vfs::Action::ReplyAndClose: {
            // Node.Close: answered, and the connection is already released.
            // The client may have gone already; it no longer matters.
            auto _ = write(*channel, outcome.message);
            return loop::Action::Close;
        }
        case vfs::Action::Drop: {
            // Malformed, or a method this connection does not serve: the
            // connection is gone, and with it every handle the message
            // brought, which nothing took.
            close_all(message.handles);
            return loop::Action::Close;
        }
        case vfs::Action::Bind: {
            // An Open or Clone of a directory: a connection of its own.
            if (auto const watched = watch(outcome.handle, outcome.connection);
                watched != Status::Ok) {
                // Answered the way any other refused Open is.
                if (auto const epitaph = wire::encode_epitaph(storage.reply.bytes, watched);
                    epitaph.is_ok()) {
                    auto _ = write(outcome.handle, epitaph.unwrap());
                }
                auto _ = ours::handle_close(outcome.handle);
            }
            return loop::Action::Rearm;
        }
        case vfs::Action::Connect: {
            // An Open of svc/<name>: the protocol's handler owns the channel
            // from here. The value is an index add_protocol() gave out, and
            // the tree holds no other service node; one past them would be a
            // bug, and its channel only closes.
            if (outcome.service >= m_protocols) {
                auto _ = ours::handle_close(outcome.handle);
                return loop::Action::Rearm;
            }
            auto const &handler = storage.handlers[outcome.service];
            handler.connect(handler.context, outcome.handle);
            return loop::Action::Rearm;
        }
        case vfs::Action::Refuse: {
            // The epitaph says why the Open or Clone failed: NotFound for a
            // protocol this directory does not serve. The client may be gone
            // already; the handle closes either way.
            auto _ = write(outcome.handle, outcome.message);
            auto _ = ours::handle_close(outcome.handle);
            return loop::Action::Rearm;
        }
    }
    return loop::Action::Rearm;
}
} // namespace svc
