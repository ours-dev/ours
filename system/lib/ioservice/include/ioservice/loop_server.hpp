/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef IOSERVICE_LOOP_SERVER_HPP
#define IOSERVICE_LOOP_SERVER_HPP 1

#include <ioservice/server.hpp>
#include <ioservice/tree.hpp>

#include <span>
#include <string_view>

#include <loop/loop.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours_io/ours_io.hpp>
#include <wire/wire.hpp>

/// A Server served on a Loop: the glue between the core, which answers one
/// message and touches no channel, and the channels and the Loop a program
/// serves it on. The root's /boot and /svc and every provider's outgoing
/// directory are served this way, so the glue is written here once.
///
/// The objects:
///   - the Server and its connection table;
///   - one Loop slot per connection, which waits on the connection's Binding,
///     whose on_signals() reads one message from the connection's channel,
///     hands it to the Server and does what the Outcome says;
///   - the Delegate, the program's object that says what an Open of a service
///     node comes to.
///
/// The events: serve() binds a directory to a channel and waits on it. When
/// a message arrives the slot fires. A reply is written back, a malformed
/// message ends the connection, and an Open of a directory makes a
/// connection of its own, which gets a slot too. An Open of a service node
/// goes to the Delegate's connect(), which owns its channel from then on. An
/// Open that is refused is answered with its epitaph, and then reported to
/// the Delegate's open_refused(), so that a program can say why a client
/// found nothing. A client that goes away ends its connection and frees its
/// slot. So does a
/// slot the Loop cannot wait on again: it frees the slot and tells the
/// Binding, which ends the connection.
///
/// One thread, as the Loop it runs on: nothing is locked, and an on_signals()
/// runs to completion before the next one starts.
namespace ioservice {
/// One Server served on a Loop.
class LoopServer final {
  public:
    /// What the program supplies beyond the tree: the object a LoopServer
    /// calls back into, which is the program's own server class (Realm,
    /// Outgoing) in every use. Each function runs inside a Binding's
    /// on_signals(), so it may add() slots but must not wait.
    ///
    /// Nothing is deleted through a Delegate, so its destructor is not
    /// virtual: a virtual one would add a deleting destructor, which a
    /// program with no heap has no use for.
    class Delegate {
      public:
        /// Where an Open of a service node goes: |service| is the value the
        /// node was added with and |channel| the request's, which the
        /// function owns from the call on. It serves the channel, forwards
        /// it, or closes it.
        virtual auto connect(u64 service, ours::Uandle channel) -> void = 0;

        /// A reply could not be written, with what channel_write answered:
        /// called just before the connection is ended, so that the program
        /// can say so. It does nothing unless overridden.
        virtual auto write_failed(ours::Status status) -> void;

        /// An Open was refused, and its epitaph has been written: |directory|
        /// is the directory the Open was made on, |path| what it asked for
        /// and |status| the epitaph's. It does nothing unless overridden, so
        /// a program that does not listen pays one virtual call per refused
        /// Open.
        ///
        /// |path| is a view of the request, valid for the call only: a
        /// program that keeps it copies it. It is whatever the client sent,
        /// a path that was not valid included. A refused Clone has no path
        /// and is not reported. This runs for every refused Open of every
        /// directory the server serves, so a program that serves a tree
        /// where a refusal is ordinary (a missing file) tells the
        /// directories apart by |directory|.
        virtual auto open_refused(NodeId directory, std::string_view path, ours::Status status)
            -> void;

      protected:
        Delegate() = default;
        Delegate(Delegate const &) = default;
        auto operator=(Delegate const &) -> Delegate & = default;
        ~Delegate() = default;
    };

    /// What a connection's Loop slot waits on: the Loop's Waiter for one
    /// connection. It holds the server and the connection the slot serves, and
    /// when the slot's wait fires on_signals() hands the connection to the
    /// server. A connection has one slot and one behaviour, so its Binding is
    /// the object the Loop calls, and no function has to cast a `void *` back
    /// to it.
    ///
    /// A program declares an array of them for a LoopServer to use; only the
    /// LoopServer reads or writes what is inside. The Loop keeps a Binding's
    /// address (see loop::Waiter), which holds because the array is the
    /// program's storage, outlives the LoopServer and is never moved, and
    /// because a connection's Binding is registered again only after its
    /// connection was released and the Loop has freed its slot. A connection
    /// is released by handle(), which answers Close, so that the Loop frees
    /// the slot before any other callback runs, and by on_rearm_failed(),
    /// which the Loop calls after it has freed the slot. The constructor is
    /// constexpr, so that an array of them can be constant-initialised.
    class Binding final: public loop::Waiter {
      public:
        constexpr Binding() = default;

        /// What the Loop calls when the connection's channel has a message or
        /// its peer is gone: LoopServer::handle() for this connection.
        auto on_signals(ours::Signals observed) -> loop::Action override;

        /// What the Loop calls when it could not arm the connection's wait
        /// again, after it has closed the channel and freed the slot:
        /// LoopServer::rearm_failed() for this connection, which releases it.
        auto on_rearm_failed(ours::Status cause) -> void override;

      private:
        friend class LoopServer;

        LoopServer *m_server = nullptr;
        ConnectionId m_connection = {};
    };

    /// The storage a program lends the server, sized by how many connections
    /// it allows. A program declares it constinit, outside its stack: the
    /// two buffers are as large as a channel message.
    struct Memory {
        /// The Server's connection table.
        std::span<Connection> connections;
        /// A connection's Binding is the one at its index, so there must be
        /// as many as |connections|. With fewer, a connection past them
        /// cannot be waited on and is refused.
        std::span<Binding> bindings;
        /// Where a Binding's on_signals() reads the one message it handles.
        /// They must hold the largest message a channel carries
        /// (wire::MAX_MESSAGE_BYTES bytes and wire::MAX_MESSAGE_HANDLES
        /// handles): a smaller buffer makes every read of a large message
        /// fail, and the connection it came on ends.
        std::span<u8> request_bytes;
        std::span<ours::Uandle> request_handles;
        /// Where the Server builds its answer. One serves every Binding,
        /// since one on_signals() runs at a time.
        ReplyBuffer *reply = nullptr;
    };

    /// A server for |tree|, whose connections wait on |loop|. |tree|, |loop|,
    /// |delegate| and everything |memory| refers to must outlive it; the tree
    /// may still grow while it is served. |syscalls| is the Server's.
    /// |delegate| may be null for now: see set_delegate().
    LoopServer(Tree const &tree, loop::Loop &loop, Syscalls syscalls, Memory memory,
               Delegate *delegate = nullptr);

    /// The Server and its slots keep pointers to this object.
    LoopServer(LoopServer const &) = delete;
    auto operator=(LoopServer const &) -> LoopServer & = delete;

    /// Sets where an Open of a service node, and a failed reply, go. Until a
    /// delegate is set, such an Open's channel is closed.
    auto set_delegate(Delegate *delegate) -> void {
        m_delegate = delegate;
    }

    /// Serves the directory |directory| on |channel|, with |rights|, as if a
    /// client had opened it, and waits on it. On Ok the channel is the
    /// Loop's, its slot's to close. On failure nothing changed and the
    /// channel is still the caller's: what Server::bind() refuses with
    /// (InvArgs, NotDir), or NoResource when every connection is in use, a
    /// connection has no Binding, or the Loop is full.
    [[nodiscard]]
    auto serve(ours::Uandle channel, NodeId directory, ours_io::Operations rights) -> ours::Status;

  private:
    /// Handles what |observed| says happened on |connection|: one message,
    /// or its client gone. It is what the connection's Binding does when the
    /// Loop calls it.
    [[nodiscard]]
    auto handle(ConnectionId connection, ours::Signals observed) -> loop::Action;

    /// Releases |connection|, whose slot the Loop has freed because it could
    /// not arm its wait again; the Loop has closed the channel as well, so
    /// the Server's entry is all that is left to free. It is what the
    /// connection's Binding does when the Loop calls its on_rearm_failed().
    auto rearm_failed(ConnectionId connection) -> void;

    /// Waits on |channel|, the channel of the new connection |connection|.
    /// On failure the connection is released and the channel left to the
    /// caller.
    [[nodiscard]]
    auto watch(ours::Uandle channel, ConnectionId connection) -> ours::Status;

    loop::Loop *m_loop;
    Server m_server;
    Memory m_memory;
    Delegate *m_delegate;
};
} // namespace ioservice

#endif // #ifndef IOSERVICE_LOOP_SERVER_HPP
