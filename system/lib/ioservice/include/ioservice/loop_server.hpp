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
#include <prot/io.hpp>
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
///     node comes to;
///   - the NodeHandler, the program's object that answers the Handler nodes of
///     the tree (<ioservice/server.hpp>), now or later.
///
/// The events: serve() binds a directory to a channel and waits on it. When
/// a message arrives the slot fires. A reply is written back, a malformed
/// message ends the connection, and an Open of a directory makes a
/// connection of its own, which gets a slot too. An Open of a service node
/// goes to the Delegate's connect(), which owns its channel from then on. So
/// does an Open that walks to a Subtree node, to the Delegate's subtree(),
/// with the rest of its path and its flags: no connection is made, and the
/// program serves the channel as it likes. An
/// Open that is refused is answered with its epitaph, and then reported to
/// the Delegate's open_refused(), so that a program can say why a client
/// found nothing. A client that goes away ends its connection and frees its
/// slot. So does a
/// slot the Loop cannot wait on again: it frees the slot and tells the
/// Binding, which ends the connection.
///
/// A request the NodeHandler answers Pending is not answered now: the Binding
/// answers Suspend, so that the Loop leaves the connection's slot in use and
/// unarmed, and the next message of the client waits in its channel, in
/// order, while this one is open. The handler ends the request later with
/// complete(), from one of its own Loop callbacks: the glue writes the reply
/// on the connection's channel, and then resumes the slot, in that order, so
/// that the next request is never started before the last one is answered.
/// The connection is not released by complete(), whatever the client did
/// meanwhile: a client that went away is seen by the connection's own
/// callback, once the slot is resumed (see complete()).
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

        /// Where an Open that walked to a Subtree node goes: |value| is what
        /// the node was added with, |rest| the part of the Open's path after
        /// the node ("." when it names the node itself), |flags| the Open's
        /// flags exactly as the client sent them, and |channel| the Open's,
        /// which the function owns from the call on, as connect()'s. It
        /// serves the channel, forwards it, answers it with an epitaph, or
        /// closes it. The Open's own checks have run on the connection it
        /// came on, so |flags| ask for no more than that connection holds,
        /// and the program holds what it makes to the same: rights only shrink.
        ///
        /// |rest| is a view of the request, valid for the call only: a
        /// program that parks the Open to answer it later copies it.
        ///
        /// Unless overridden it closes |channel|, since nothing else would: a
        /// program that adds no Subtree node never reaches it, and one that
        /// does is told by its client seeing the channel close.
        virtual auto subtree(u64 value, std::string_view rest, prot::io::Flags flags,
                             ours::Uandle channel) -> void;

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
        /// and is not reported, and neither is an Open that a NodeHandler
        /// answered Pending and that was refused when complete() ended it:
        /// its request is gone by then, so there is no path to say. This runs
        /// for every refused Open of every
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
    /// which the Loop calls after it has freed the slot. Nothing else
    /// releases one, complete() least of all: it runs in another slot's
    /// callback, while this connection's slot is still in use, so an index it
    /// freed could be taken by a new connection that this slot's late packet
    /// would then be handled as. The constructor is constexpr, so that an
    /// array of them can be constant-initialised.
    ///
    /// A Binding keeps the key the Loop gave its slot, which complete() needs
    /// to resume the slot. It is the key of the slot's current life only: it is
    /// set again whenever the connection's slot is.
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
        /// The key add() answered for the connection's slot, valid from the
        /// connection's watch() until the slot is freed. Before the first
        /// watch() it names slot 0 in its first life, which nothing reads: a
        /// Binding is looked up for a ticket only through a connection that
        /// is alive, and a live connection was watched.
        loop::Key m_key = loop::Key{0, 0};
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
    /// |delegate|, |handler| and everything |memory| refers to must outlive it;
    /// the tree may still grow while it is served. |syscalls| is the Server's.
    /// |delegate| may be null for now: see set_delegate(). |handler| answers
    /// the tree's Handler nodes (NodeKind::Handler) and is the Server's, as its
    /// constructor says; without one, an Open that walks to a Handler node is
    /// refused with NotSupported, and nothing is ever pending.
    LoopServer(Tree const &tree, loop::Loop &loop, Syscalls syscalls, Memory memory,
               Delegate *delegate = nullptr, NodeHandler *handler = nullptr);

    /// The Server and its slots keep pointers to this object.
    LoopServer(LoopServer const &) = delete;
    auto operator=(LoopServer const &) -> LoopServer & = delete;

    /// Sets where an Open of a service node or of a Subtree node, and a
    /// failed reply, go. Until a delegate is set, such an Open's channel is
    /// closed.
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
    auto serve(ours::Uandle channel, NodeId directory, prot::io::Operations rights) -> ours::Status;

    /// Ends the request the NodeHandler answered Pending with |ticket|, with
    /// its |result|, and answers the client: Server::complete() decides what
    /// the answer is, and this carries it out on the connection the ticket
    /// names, then resumes that connection's slot. The overload is the
    /// operation's own, as the Server's. Call it from a Loop callback of the
    /// handler's own, never from inside the call that was given the ticket
    /// (the Server refuses that with NotFound: the ticket is good once the call
    /// has returned Pending), and never use the ticket, or the cookie of the
    /// connection, once it has returned: the connection may be released from
    /// inside it.
    ///
    /// What is carried out, by what the Server answers:
    ///   - a reply (Read, ReadDirents, GetBackingMemory, or an Open's refusal
    ///     of a connection already made) is written on the connection's
    ///     channel. A write that fails, the usual one being a client that went
    ///     away while it waited, is told to the Delegate's write_failed() as
    ///     any failed reply is, and the connection is left alone: its own
    ///     callback sees the client gone once the slot is resumed, releases it
    ///     and closes the slot, and the handler is told release(cookie) then,
    ///     once;
    ///   - a pending Open that was granted becomes a connection on the server
    ///     end the Server held, and gets a slot (watch()); one that cannot get a
    ///     slot, or that the Server refused, is answered with an epitaph on
    ///     that server end, which is then closed. No Open path is left to say,
    ///     so a refusal found here is not reported to open_refused();
    /// and then the connection's slot is resumed, so the client's next message
    /// is read.
    ///
    /// Answers Ok when the Server took the result, whatever became of the
    /// reply: the request is ended, and the result is the reply's now. A
    /// ticket the Server refuses is answered as it refuses it (NotFound for a
    /// ticket that names no pending request, InvArgs for a result of another
    /// operation than the request's), with nothing written and nothing taken
    /// from |result|: a VMO handle in it is still the caller's, and the
    /// cookie of an Opened still the handler's to release. BadState or
    /// NotFound after the reply was written means the slot was not suspended
    /// (someone resumed it, or a Binding answered Rearm for a Pending
    /// request), a bug of the program and not of the handler: the request is
    /// ended and answered all the same. A resume that the kernel refuses (the
    /// wait could not be armed) frees the slot, and the Binding releases the
    /// connection, from inside this call; the answer is Ok, since the reply is
    /// out.
    [[nodiscard]]
    auto complete(Ticket ticket, OpenResult const &result) -> ours::Status;

    [[nodiscard]]
    auto complete(Ticket ticket, ReadResult const &result) -> ours::Status;

    [[nodiscard]]
    auto complete(Ticket ticket, EntriesResult const &result) -> ours::Status;

    [[nodiscard]]
    auto complete(Ticket ticket, VmoResult const &result) -> ours::Status;

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

    /// Releases |connection| and closes the server end of an Open that was
    /// pending on it, if one was: the Server hands it back because nothing
    /// else would answer it. The program's callbacks release a connection
    /// that is not waiting, so it is Invalid in practice.
    auto release(ConnectionId connection) -> void;

    /// A new connection |outcome| made for the request's channel: waits on it
    /// (watch()), and answers an epitaph on the channel, closed, when that
    /// fails. The Bind of dispatch() and of complete().
    auto bind(Outcome const &outcome) -> void;

    /// Writes |outcome|'s epitaph on its handle, the request's channel, and
    /// closes it: the Refuse of dispatch() and of complete().
    auto refuse(Outcome const &outcome) -> void;

    /// Carries out what Server::complete() answered for |ticket|, then
    /// resumes the connection's slot; the four complete() overloads end here.
    [[nodiscard]]
    auto finish(Ticket ticket, ustl::Result<Outcome, ours::Status> const &completed)
        -> ours::Status;

    loop::Loop *m_loop;
    Server m_server;
    Memory m_memory;
    Delegate *m_delegate;
};
} // namespace ioservice

#endif // #ifndef IOSERVICE_LOOP_SERVER_HPP
