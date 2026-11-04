/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ioservice/loop_server.hpp>

#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

namespace ioservice {
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

auto LoopServer::Delegate::write_failed(Status) -> void {}

auto LoopServer::Delegate::subtree(u64, std::string_view, prot::io::Flags, Uandle const channel)
    -> void {
    // Nobody else holds the channel now: it was the Open's, and the server
    // gave it away. Nothing can be done about a close that fails.
    auto _ = ours::handle_close(channel);
}

auto LoopServer::Delegate::open_refused(NodeId, std::string_view, Status) -> void {}

LoopServer::LoopServer(Tree const &tree, loop::Loop &loop, Syscalls const syscalls,
                       Memory const memory, Delegate *const delegate)
    : m_loop(&loop)
    , m_server(tree, memory.connections, syscalls)
    , m_memory(memory)
    , m_delegate(delegate) {}

auto LoopServer::serve(Uandle const channel, NodeId const directory,
                       prot::io::Operations const rights) -> Status {
    auto const bound = m_server.bind(channel, directory, rights);
    if (bound.is_err()) {
        return bound.unwrap_err();
    }
    return watch(channel, bound.unwrap());
}

auto LoopServer::watch(Uandle const channel, ConnectionId const connection) -> Status {
    // A connection's Binding is the one at its index. A program that lent
    // fewer Bindings than connections has a connection it cannot wait on.
    if (connection.index >= m_memory.bindings.size()) {
        m_server.release(connection);
        return Status::NoResource;
    }
    auto &binding = m_memory.bindings[connection.index];
    binding.m_server = this;
    binding.m_connection = connection;
    auto const added = m_loop->add(channel, CONNECTION_SIGNALS, binding);
    if (added.is_err()) {
        // The Loop did not take the channel, so the connection must not
        // outlive this answer either.
        m_server.release(connection);
        return added.unwrap_err();
    }
    return Status::Ok;
}

auto LoopServer::Binding::on_signals(Signals const observed) -> loop::Action {
    return m_server->handle(m_connection, observed);
}

auto LoopServer::Binding::on_rearm_failed(Status) -> void {
    m_server->rearm_failed(m_connection);
}

auto LoopServer::rearm_failed(ConnectionId const connection) -> void {
    // The cause is the Loop's to report and no one here can act on it; the
    // connection is released as it is when its client goes, so that its entry
    // is handed out again. The Loop closed the channel, so nothing is closed
    // here.
    m_server.release(connection);
}

auto LoopServer::handle(ConnectionId const connection, Signals const observed) -> loop::Action {
    auto const channel = m_server.channel(connection);
    if (channel.is_none()) {
        // Nothing releases a connection without its slot closing too: in the
        // same callback, which answers Close, or, after a re-arm that failed,
        // with the slot already free. So this is a slot outliving its
        // connection: close it.
        return loop::Action::Close;
    }
    // The client went away with nothing left to read. The slot's own
    // callback closes its handle, by answering Close.
    if (!(observed & Signals::Readable)) {
        m_server.release(connection);
        return loop::Action::Close;
    }

    // One message per callback, then re-arm: a client with more queued waits
    // behind everyone else.
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    if (auto const read = ours::channel_read(
            *channel, ours::ChannelReadOptions::None, m_memory.request_bytes.data(),
            m_memory.request_handles.data(), static_cast<u32>(m_memory.request_bytes.size()),
            static_cast<u32>(m_memory.request_handles.size()), &num_bytes, &num_handles);
        read != Status::Ok) {
        // The buffers hold the largest message a channel carries, so this is
        // the channel failing, not the message: nothing more can be served
        // on it.
        m_server.release(connection);
        return loop::Action::Close;
    }
    auto const message = wire::Message{
        .bytes = m_memory.request_bytes.first(num_bytes),
        .handles = m_memory.request_handles.first(num_handles),
    };

    auto const outcome = m_server.dispatch(connection, message, *m_memory.reply);
    switch (outcome.action) {
        case Action::Reply: {
            // A write to a full channel fails that write alone: nothing here
            // waits for room. A reply that cannot be written leaves its
            // client waiting for good, so the connection goes with it.
            if (auto const written = write(*channel, outcome.message); written != Status::Ok) {
                if (m_delegate != nullptr) {
                    m_delegate->write_failed(written);
                }
                m_server.release(connection);
                return loop::Action::Close;
            }
            return loop::Action::Rearm;
        }
        case Action::ReplyAndClose: {
            // Node.Close: answered, and the connection is already released.
            // The client may have gone already; it no longer matters.
            auto _ = write(*channel, outcome.message);
            return loop::Action::Close;
        }
        case Action::Drop: {
            // Malformed, or a method this connection does not serve: the
            // connection is gone, and with it every handle the message
            // brought, which nothing took.
            close_all(message.handles);
            return loop::Action::Close;
        }
        case Action::Bind: {
            // An Open or Clone of a directory: a connection of its own.
            if (auto const watched = watch(outcome.handle, outcome.connection);
                watched != Status::Ok) {
                // Answered the way any other refused Open is.
                if (auto const epitaph = wire::encode_epitaph(m_memory.reply->bytes, watched);
                    epitaph.is_ok()) {
                    auto _ = write(outcome.handle, epitaph.unwrap());
                }
                auto _ = ours::handle_close(outcome.handle);
            }
            return loop::Action::Rearm;
        }
        case Action::Connect: {
            // An Open of a service node: the delegate owns the channel from
            // here. Without one, the channel goes.
            if (m_delegate == nullptr) {
                auto _ = ours::handle_close(outcome.handle);
            } else {
                m_delegate->connect(outcome.service, outcome.handle);
            }
            return loop::Action::Rearm;
        }
        case Action::Subtree: {
            // An Open that walked to a Subtree node: the delegate owns the
            // channel from here, as for a service node, and no connection
            // was made. Without one, the channel goes. The rest views the
            // request buffer, which nothing touches before this returns.
            if (m_delegate == nullptr) {
                auto _ = ours::handle_close(outcome.handle);
            } else {
                m_delegate->subtree(outcome.service, outcome.rest, outcome.flags, outcome.handle);
            }
            return loop::Action::Rearm;
        }
        case Action::Refuse: {
            // The epitaph says why the Open or Clone failed. The client may
            // be gone already; the handle closes either way.
            auto _ = write(outcome.handle, outcome.message);
            auto _ = ours::handle_close(outcome.handle);
            // Reported once the client has its answer, so that a delegate
            // that is slow to say so delays nobody's. The path still views
            // the request buffer, which nothing has touched since the read.
            // A Clone has no path and is not reported; the connection is
            // still the server's, so its directory is there to ask.
            if (m_delegate != nullptr && outcome.path.is_some()) {
                if (auto const directory = m_server.node(connection); directory.is_some()) {
                    m_delegate->open_refused(*directory, *outcome.path, outcome.status);
                }
            }
            return loop::Action::Rearm;
        }
        case Action::Pending: {
            // A NodeHandler answered Pending. A LoopServer gives its Server no
            // handler, so no Open reaches one and nothing answers Pending
            // here; the glue that suspends the slot and resumes it after
            // Server::complete() is the next step. Were one to arrive anyway,
            // the connection could never be ended, so it goes, with the Open's
            // server end it held.
            auto const held = m_server.release(connection);
            if (held != Uandle::Invalid) {
                auto _ = ours::handle_close(held);
            }
            return loop::Action::Close;
        }
        case Action::Forward: {
            // An Open that reached a remote entry: the message is the Open
            // the remote is to serve, the handle it carries the request's
            // server end. A write the remote's channel refuses consumed that
            // handle, and the client sees its channel close with no epitaph,
            // the way an Open routed into a dead provider already ends.
            if (auto const written = write(outcome.forward_channel, outcome.message);
                written != Status::Ok && m_delegate != nullptr) {
                m_delegate->write_failed(written);
            }
            return loop::Action::Rearm;
        }
    }
    return loop::Action::Rearm;
}
} // namespace ioservice
