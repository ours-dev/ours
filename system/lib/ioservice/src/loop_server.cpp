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
                       Memory const memory, Delegate *const delegate, NodeHandler *const handler)
    : m_loop(&loop)
    , m_server(tree, memory.connections, syscalls, handler)
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

auto LoopServer::complete(Ticket const ticket, OpenResult const &result) -> Status {
    return finish(ticket, m_server.complete(ticket, result, *m_memory.reply));
}

auto LoopServer::complete(Ticket const ticket, ReadResult const &result) -> Status {
    return finish(ticket, m_server.complete(ticket, result, *m_memory.reply));
}

auto LoopServer::complete(Ticket const ticket, EntriesResult const &result) -> Status {
    return finish(ticket, m_server.complete(ticket, result, *m_memory.reply));
}

auto LoopServer::complete(Ticket const ticket, VmoResult const &result) -> Status {
    return finish(ticket, m_server.complete(ticket, result, *m_memory.reply));
}

auto LoopServer::finish(Ticket const ticket, ustl::Result<Outcome, Status> const &completed)
    -> Status {
    if (completed.is_err()) {
        // Refused: NotFound or InvArgs, and nothing was written or taken.
        return completed.unwrap_err();
    }
    auto const outcome = completed.unwrap();
    // The Server accepted the ticket, so its connection is alive and was made
    // through watch(): its channel is there, and so are its Binding and the key
    // of its slot. Both are read before anything is carried out, which can run
    // the handler (a release) but never frees this connection or its Binding.
    auto const connection = ticket.connection;
    auto const channel = *m_server.channel(connection);
    auto const key = m_memory.bindings[connection.index].m_key;

    // complete() answers one of three: the reply, the connection of a granted
    // Open, or the epitaph of a refused one. Never a Drop or a ReplyAndClose,
    // which would release the connection from a callback that is not its own.
    if (outcome.action == Action::Reply) {
        // A write to a client that went away fails alone: nothing here waits.
        // The connection stays. Its callback releases it once it is resumed
        // and sees the client gone, and not here, where its slot is still in
        // use and an index freed would be taken by a new connection before the
        // old slot's packet came.
        if (auto const written = write(channel, outcome.message);
            written != Status::Ok && m_delegate != nullptr) {
            m_delegate->write_failed(written);
        }
    } else if (outcome.action == Action::Bind) {
        bind(outcome);
    } else if (outcome.action == Action::Refuse) {
        // There is no path to report to open_refused(): the Open's bytes are
        // gone by now.
        refuse(outcome);
    }

    // After the answer, never before: the slot is read again from here, and
    // the next request must find the last one answered.
    if (auto const resumed = m_loop->resume(key);
        resumed == Status::BadState || resumed == Status::NotFound) {
        // The slot was not suspended, or is not the one this connection had.
        // Any other answer is the kernel refusing the arm, which freed the slot
        // and had the Binding release the connection already.
        return resumed;
    }
    return Status::Ok;
}

auto LoopServer::watch(Uandle const channel, ConnectionId const connection) -> Status {
    // A connection's Binding is the one at its index. A program that lent
    // fewer Bindings than connections has a connection it cannot wait on.
    if (connection.index >= m_memory.bindings.size()) {
        release(connection);
        return Status::NoResource;
    }
    auto &binding = m_memory.bindings[connection.index];
    binding.m_server = this;
    binding.m_connection = connection;
    auto const added = m_loop->add(channel, CONNECTION_SIGNALS, binding);
    if (added.is_err()) {
        // The Loop did not take the channel, so the connection must not
        // outlive this answer either.
        release(connection);
        return added.unwrap_err();
    }
    // Kept for complete(), which resumes the slot with it.
    binding.m_key = added.unwrap();
    return Status::Ok;
}

auto LoopServer::release(ConnectionId const connection) -> void {
    if (auto const held = m_server.release(connection); held != Uandle::Invalid) {
        // An Open that was still pending on the connection: nothing will
        // answer it now, and the client would wait for ever on its channel.
        // Nothing can be done about a close that fails.
        auto _ = ours::handle_close(held);
    }
}

auto LoopServer::bind(Outcome const &outcome) -> void {
    if (auto const watched = watch(outcome.handle, outcome.connection); watched != Status::Ok) {
        // Answered the way any other refused Open is.
        if (auto const epitaph = wire::encode_epitaph(m_memory.reply->bytes, watched);
            epitaph.is_ok()) {
            auto _ = write(outcome.handle, epitaph.unwrap());
        }
        auto _ = ours::handle_close(outcome.handle);
    }
}

auto LoopServer::refuse(Outcome const &outcome) -> void {
    // The epitaph says why the Open or Clone failed. The client may be gone
    // already; the handle closes either way.
    auto _ = write(outcome.handle, outcome.message);
    auto _ = ours::handle_close(outcome.handle);
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
    release(connection);
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
        release(connection);
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
        release(connection);
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
                release(connection);
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
            // brought, which nothing took, and the Open's server end that it
            // still held, when the message came for a connection that waited.
            close_all(message.handles);
            if (outcome.handle != Uandle::Invalid) {
                auto _ = ours::handle_close(outcome.handle);
            }
            return loop::Action::Close;
        }
        case Action::Bind: {
            // An Open or Clone of a directory: a connection of its own.
            bind(outcome);
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
            refuse(outcome);
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
            // The NodeHandler will answer later, through complete(). Nothing is
            // written now, and the connection must not be read before then:
            // Suspend leaves its slot in use with nothing armed, so the
            // client's next message waits in the channel, in order, and a
            // client that goes away is seen when complete() resumes the slot.
            // complete() never runs inside this callback: the Server accepts a
            // ticket only once the handler's call has returned Pending, and
            // nothing between that and this return calls the handler.
            return loop::Action::Suspend;
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
