/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef IOSERVICE_LOOP_SERVER_HPP
#define IOSERVICE_LOOP_SERVER_HPP 1

#include <ioservice/server.hpp>
#include <ioservice/tree.hpp>

#include <span>

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
///   - one Loop slot per connection, whose callback reads one message from
///     the connection's channel, hands it to the Server and does what the
///     Outcome says;
///   - the Handlers, where the program says what an Open of a service node
///     comes to.
///
/// The events: serve() binds a directory to a channel and waits on it. When
/// a message arrives the slot fires. A reply is written back, a malformed
/// message ends the connection, and an Open of a directory makes a
/// connection of its own, which gets a slot too. An Open of a service node
/// goes to the Handlers' connect function, which owns its channel from then
/// on. A client that goes away ends its connection and frees its slot.
///
/// One thread, as the Loop it runs on: nothing is locked, and a callback runs
/// to completion before the next one starts.
namespace ioservice {
/// What the program supplies beyond the tree. Both functions run inside a
/// Loop callback, so they may add() slots but must not wait.
struct Handlers {
    /// Where an Open of a service node goes: |service| is the value the node
    /// was added with and |channel| the request's, which the function owns
    /// from the call on. It serves the channel, forwards it, or closes it.
    /// Without one, such an Open's channel is closed.
    auto (*connect)(void *context, u64 service, ours::Uandle channel) -> void = nullptr;

    /// Optional. A reply could not be written, with what channel_write
    /// answered: called just before the connection is ended, so that the
    /// program can say so.
    auto (*write_failed)(void *context, ours::Status status) -> void = nullptr;

    /// The first argument of both functions.
    void *context = nullptr;
};

/// One Server served on a Loop.
class LoopServer final {
  public:
    /// What a connection's Loop slot is called with: the server, and the
    /// connection the slot serves. A program declares an array of them for a
    /// LoopServer to use; only the LoopServer reads or writes what is inside.
    class Binding final {
      public:
        constexpr Binding() = default;

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
        /// Where a callback reads the one message it handles. They must hold
        /// the largest message a channel carries (wire::MAX_MESSAGE_BYTES
        /// bytes and wire::MAX_MESSAGE_HANDLES handles): a smaller buffer
        /// makes every read of a large message fail, and the connection it
        /// came on ends.
        std::span<u8> request_bytes;
        std::span<ours::Uandle> request_handles;
        /// Where the Server builds its answer. One serves every callback,
        /// since one callback runs at a time.
        ReplyBuffer *reply = nullptr;
    };

    /// A server for |tree|, whose connections wait on |loop|. |tree|, |loop|
    /// and everything |memory| refers to must outlive it; the tree may still
    /// grow while it is served. |syscalls| is the Server's.
    LoopServer(Tree const &tree, loop::Loop &loop, Syscalls syscalls, Memory memory,
               Handlers handlers = {});

    /// The Server and its slots keep pointers to this object.
    LoopServer(LoopServer const &) = delete;
    auto operator=(LoopServer const &) -> LoopServer & = delete;

    /// Sets what an Open of a service node, and a failed reply, come to.
    /// Until a connect function is set, such an Open's channel is closed.
    auto set_handlers(Handlers handlers) -> void {
        m_handlers = handlers;
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
    /// A connection slot's callback: |context| is its Binding.
    static auto on_signals(void *context, ours::Signals observed) -> loop::Action;

    /// Handles what |observed| says happened on |connection|: one message,
    /// or its client gone.
    [[nodiscard]]
    auto handle(ConnectionId connection, ours::Signals observed) -> loop::Action;

    /// Waits on |channel|, the channel of the new connection |connection|.
    /// On failure the connection is released and the channel left to the
    /// caller.
    [[nodiscard]]
    auto watch(ours::Uandle channel, ConnectionId connection) -> ours::Status;

    loop::Loop *m_loop;
    Server m_server;
    Memory m_memory;
    Handlers m_handlers;
};
} // namespace ioservice

#endif // #ifndef IOSERVICE_LOOP_SERVER_HPP
