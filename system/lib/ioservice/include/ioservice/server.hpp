/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef IOSERVICE_SERVER_HPP
#define IOSERVICE_SERVER_HPP 1

#include <ioservice/tree.hpp>

#include <array>
#include <span>
#include <string_view>
#include <variant>

#include <ours/rights.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <prot/io.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// The ours.io server (component-framework design, "ioservice" and "Events: a
/// component opening a file in /boot"): connections to the nodes of a Tree,
/// each with its own rights, and the handlers of the subset's methods.
///
/// The core touches no channel. A program reads a message from a
/// connection's channel, hands it to Server::dispatch(), and gets back an
/// Outcome saying what to write where and which handles to wait on next.
/// The program does that writing, closing and waiting, on its Loop. So the
/// whole protocol, the Open checks included, runs on the host, where the
/// tests drive it message by message.
///
/// A place whose content is not a tree of nodes, a filesystem's, is a Handler
/// node (NodeKind::Handler) with a NodeHandler behind it: the Server keeps the
/// protocol on the connections the handler opens (the Open checks, rights, the
/// seek offset, the dirent cursor, every reply) and asks the handler only for
/// content, which it may answer later. dispatch() then answers
/// Action::Pending, the program leaves that connection unread, and the
/// handler ends the request with complete(), whose Outcome the program carries
/// out as it does dispatch()'s.
namespace ioservice {
/// Names one connection of a Server: a slot of its table, and the generation
/// the slot was in when the connection was made. A slot is reused once its
/// connection ends, under a new generation, so an id kept past the end is
/// stale and names nothing, rather than naming whichever connection came
/// next. The Loop keys its packets the same way
/// (docs/design/260924/userspace-filesystem.md §4.2).
///
/// Generation 0 never names a live connection: every slot starts at
/// FIRST_GENERATION, and a release that would wrap a slot's generation to 0
/// skips it. So a ConnectionId that was never filled in, `ConnectionId{}` or
/// a table of them zeroed in .bss, is stale by construction instead of
/// naming whatever connection happens to sit in slot 0.
struct ConnectionId {
    u32 index;
    u32 generation;

    friend constexpr auto operator==(ConnectionId, ConnectionId) -> bool = default;
};

/// The generation a slot has before its first connection.
inline constexpr auto FIRST_GENERATION = u32{1};

/// The one system call the core needs: GetBackingMemory hands out a
/// duplicate of a file's VMO. The program passes ours::handle_duplicate; a
/// host test passes a function of its own, which is how the core stays free
/// of system calls without leaving GetBackingMemory to the program.
struct Syscalls {
    /// ours::handle_duplicate's shape: a new handle to |handle|'s object with
    /// |rights|, written to |out|.
    auto (*handle_duplicate)(ours::Uandle handle, ours::Rights rights, ours::Uandle *out)
        -> ours::Status;
};

/// Where dispatch() builds the message it answers with: as many bytes as a
/// channel message holds, and one handle, the most any reply of the subset
/// carries (GetBackingMemory's VMO). Its size is fixed so that no message
/// dispatch() could build fails to fit. Every byte of a message dispatch()
/// answers with is written by that call, so one buffer serves every message
/// in turn without one client's bytes reaching the next.
struct ReplyBuffer {
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes;
    std::array<ours::Uandle, 1> handles;
};

// --- Handler nodes ---------------------------------------------------------
//
// A Handler node (NodeKind::Handler) is a place whose content a NodeHandler
// answers: the content of a filesystem that keeps its own state and may have
// to wait for it. The Server keeps the protocol and asks the handler only for
// content. A request the handler can answer now is answered by Done; one that
// has to wait is answered by Pending, and ended later by Server::complete().

/// The kind of object a NodeHandler opened. The Server serves Open and
/// ReadDirents on a directory's connection, and Read and GetBackingMemory on
/// a file's.
enum class ObjectKind : u8 {
    Directory,
    File,
};

/// What NodeHandler::open() made: an object, and the handler's name for it.
struct Opened {
    ObjectKind kind;
    /// A number the handler chooses to tell its objects apart by. The Server
    /// keeps it in the connection made for the object, hands it back on every
    /// call that concerns the object, and never reads it. The handler is told
    /// it is no longer wanted by release(), once, when the connection ends.
    u64 cookie;
};

/// What NodeHandler::read_dirents() lists: the next dirents of a directory,
/// and where the listing goes on from.
struct Entries {
    /// Whole dirents, packed as prot::io::DirentWriter packs them, in the
    /// order the listing has them, at most as many bytes as the call allowed.
    /// None at all means the listing is done. The bytes are the handler's, and
    /// must stay as they are until the Server call that received them
    /// returns; the Server copies them into its reply.
    std::span<u8 const> dirents;
    /// The cursor the next call is given: the Server keeps it for the
    /// connection and never reads it. The first call is given 0.
    u64 next;
};

/// What an operation of a NodeHandler comes to: the operation's payload, or
/// the status that refused it. So a result is a status plus the payload, and
/// Ok is never carried without it.
using OpenResult = ustl::Result<Opened, ours::Status>;
/// The bytes a Read returns. They are the handler's, and must stay as they are
/// until the Server call that received them returns; the Server copies them
/// into its reply, the one copy a Read makes.
using ReadResult = ustl::Result<std::span<u8 const>, ours::Status>;
using EntriesResult = ustl::Result<Entries, ours::Status>;
/// The VMO a GetBackingMemory returns: a handle the handler made for this
/// request, with the rights the request's flags ask for. It is the handler's
/// until a Server call accepts it, and then the reply's; see Server::complete()
/// for who holds it when a call refuses.
using VmoResult = ustl::Result<ours::Uandle, ours::Status>;

/// Names one request that a NodeHandler answered Pending: the connection it
/// came on, and the serial that connection gave it. A connection has one
/// request outstanding at a time, and bumps its serial for every request it
/// asks the handler about, so a ticket is good for one request only: once that
/// request is ended, or the connection is, or a later request began, the
/// ticket names nothing and Server::complete() refuses it.
struct Ticket {
    ConnectionId connection;
    u32 serial;

    friend constexpr auto operator==(Ticket, Ticket) -> bool = default;
};

/// A NodeHandler answer: the operation is done, and |result| is its result.
template <typename Result>
struct Done {
    Result result;
};

/// A NodeHandler answer: the operation has to wait. |ticket| is the one the
/// operation was given, handed back; the handler ends the request later with
/// Server::complete(), with that ticket.
struct Pending {
    Ticket ticket;
};

/// What an operation of a NodeHandler that may wait answers with.
template <typename Result>
using Answer = std::variant<Done<Result>, Pending>;

/// What a Handler node's content is, as the Server asks for it. The program's
/// own object (a filesystem) derives from it and gives the Server a pointer.
///
/// The Server has already done everything that is the protocol's when it asks:
/// an Open has passed the checks that come before the walk (a valid path,
/// TRAVERSE, rights that are a subset of the connection's, CONNECT for a
/// service, no creation flag, nothing the design cut), a Read is on a
/// connection that holds READ_BYTES and asks no more than a reply carries, a
/// ReadDirents is on one that holds ENUMERATE, a GetBackingMemory asks for
/// rights its connection has. The handler judges only its own content. What
/// stays with the Server is the seek offset and the dirent phase (the "."
/// entry comes first and is the Server's), and the encoding of every reply.
///
/// Every operation is given |value|, the value the Handler node was added with
/// (Tree::add_handler()), so that one handler can serve several nodes. The
/// handler's objects are named by cookies, which it chooses and the Server
/// never reads.
///
/// The operations that may wait, open(), read(), read_dirents() and
/// get_backing_memory(), are also given a Ticket and answer Done or Pending. A
/// handler that has no reason to wait always answers Done. One that answers
/// Pending must end the request later with Server::complete(), on the answer
/// or on the failure of whatever it waits for, with an error status; the Server
/// has no timer, and a request nobody completes keeps its connection waiting
/// for good. It must not complete a ticket from inside the call that was given
/// it: the ticket is good once the call has returned Pending, and not before;
/// a handler that has the answer then answers Done.
///
/// clone() and release() never wait. A handler that must flush when an object
/// is released keeps that work in its own state.
///
/// Nothing is deleted through a NodeHandler, so its destructor is not
/// virtual: a virtual one would add a deleting destructor, which a program
/// with no heap has no use for.
class NodeHandler {
  public:
    /// An Open came to the handler's place. |directory| is none when the Open
    /// walked to the Handler node itself, and |path| is what was left of its
    /// path after the node ("." when it named the node): the handler's own
    /// root is where |path| starts. It is the cookie of a directory the
    /// handler opened earlier when the Open came on that directory's
    /// connection, and |path| is the whole path the client sent, to be
    /// walked from that directory. |flags| are the client's, unchanged; they
    /// ask for no more than the connection the Open came on holds.
    ///
    /// The handler opens the object |path| names, or refuses with the status
    /// the client reads from the epitaph it gets. The Server then judges the
    /// kind the handler reports against the flags (a file for a request that
    /// accepts only directories is NotDir, the reverse is NotFile) and gives
    /// the new connection exactly the rights the flags ask for, which for a
    /// file are the ones the file protocol uses. A right the handler cannot
    /// honour (EXECUTE on a file that is not executable) is its to refuse
    /// here, with AccessDenied, since the Server cannot tell.
    ///
    /// |path| views the request and is valid for the call only: a handler
    /// that answers Pending and needs it later copies it.
    virtual auto open(u64 value, ustl::Option<u64> directory, std::string_view path,
                      prot::io::Flags flags, Ticket ticket) -> Answer<OpenResult> = 0;

    /// A Read on the file |cookie| names. |offset| is the connection's seek
    /// offset, which the Server moves by the bytes of a Read that is
    /// completed, and not before; |count| is at most prot::io::MAX_TRANSFER_SIZE.
    /// Fewer bytes than |count| are allowed, and none is the end of the file;
    /// more are the handler's bug, and the Server answers the client
    /// InternalError.
    virtual auto read(u64 value, u64 cookie, u64 offset, u64 count, Ticket ticket)
        -> Answer<ReadResult> = 0;

    /// A ReadDirents on the directory |cookie| names, at most |max_bytes| of
    /// dirents long. The Server has already set aside room for the "." entry
    /// when this is the connection's first call, so |max_bytes| is what is
    /// left. |cursor| is what the last call's Entries::next said, 0 at the
    /// start. A first entry that does not fit is BufferTooSmall, and the
    /// cursor stays where it was.
    virtual auto read_dirents(u64 value, u64 cookie, u64 cursor, u64 max_bytes, Ticket ticket)
        -> Answer<EntriesResult> = 0;

    /// A GetBackingMemory on the file |cookie| names. The Server has checked
    /// that the connection's rights allow what |flags| ask for; the VMO the
    /// handler makes holds those rights.
    virtual auto get_backing_memory(u64 value, u64 cookie, prot::io::VmoFlags flags, Ticket ticket)
        -> Answer<VmoResult> = 0;

    /// A Clone of the connection to the object |cookie| names: a second
    /// connection to the same object, which starts over (offset 0, listing
    /// from the start). Answers the cookie of the new connection, which may be
    /// the same number or a new one, or refuses. Does not wait.
    virtual auto clone(u64 value, u64 cookie) -> ustl::Result<u64, ours::Status> = 0;

    /// The connection to the object |cookie| names has ended, for any reason:
    /// the client closed it, went away, or the program could not serve it. Once
    /// for every cookie the handler made, whether the connection ended with a
    /// request pending or not. The handler must not use |cookie| after, nor
    /// the ticket of a request of that connection. Does not wait.
    virtual auto release(u64 value, u64 cookie) -> void = 0;

  protected:
    NodeHandler() = default;
    NodeHandler(NodeHandler const &) = default;
    auto operator=(NodeHandler const &) -> NodeHandler & = default;
    ~NodeHandler() = default;
};

/// What the program does after dispatch(). Exactly one thing, since every
/// method of the subset either answers on the connection or does something
/// with the one handle its request carries.
enum class Action : u8 {
    /// Write |message| on the connection's channel. The connection stays.
    Reply,
    /// Write |message| on the connection's channel, then close the channel:
    /// Node.Close was answered. The connection is already released.
    ReplyAndClose,
    /// The message was malformed, named a method this connection does not
    /// serve, or came for a stale id: close the connection's channel without
    /// writing anything, and close every handle the message carried, since
    /// none of them was taken. The connection is already released. A peer
    /// that sends such a message is not speaking the protocol, so no answer
    /// would help it, and the header has no bit to mark a call as one an
    /// unknown method may be answered for. A message that came for a
    /// connection with a request pending is dropped the same way (nothing
    /// reads such a connection until its request is ended, so only a program
    /// bug sends one). If |handle| is valid, it is the server end of an Open
    /// that was pending on the connection, which nothing will answer now:
    /// close it as well.
    Drop,
    /// |handle|, the request's channel, is now the channel of the connection
    /// |connection|: wait on it. If the program cannot (its Loop is full),
    /// it release()s |connection| and closes |handle|.
    Bind,
    /// |handle|, the request's channel, was opened as the service added with
    /// |service|: hand it to that service, which now owns it. The server
    /// calls nothing itself here: the choice goes back to the program, which
    /// routes it (design D10, D11).
    Connect,
    /// The Open walked to a Subtree node, a place the program serves itself:
    /// hand |handle|, the request's channel, to the program that added it
    /// with |service|, which now owns the channel, together with |rest|, the
    /// part of the path after the node, and |flags|, the Open's flags
    /// exactly as the client sent them. The Open's own checks have run on the
    /// connection it came on and passed, so the rights |flags| ask for are
    /// within that connection's: rights only shrink from here on, and the
    /// program must not widen them. No connection is made, and the server
    /// calls nothing itself, as for Connect.
    Subtree,
    /// Open or Clone failed: write |message|, an epitaph, on |handle|, the
    /// request's channel, then close |handle|. The connection the request
    /// came on stays. |status| and, for an Open, |path| say what was refused
    /// and why, for a program that reports it.
    Refuse,
    /// Open reached a remote entry: |message| is the Open to serve there,
    /// with |handle|, the request's server end, among its handles, to be
    /// written on |forward_channel|. The connection the request came on
    /// stays.
    Forward,
    /// The request was handed to a NodeHandler, which answered Pending: its
    /// answer comes later, through complete(), which returns the Outcome to
    /// carry out then. Nothing is written now, and the connection the request
    /// came on stays, with the request outstanding: the program must not read
    /// it again until it has carried out what complete() returns (a Loop
    /// callback answers Suspend, and resumes the slot after that write). The
    /// Open's server end, if the request was an Open, is held until then, so
    /// there is no |handle|.
    Pending,
};

/// What dispatch() decided. Only the fields its Action names mean anything.
struct Outcome {
    Action action;
    /// Reply, ReplyAndClose, Refuse: what to write, inside the ReplyBuffer.
    wire::Message message = {};
    /// Bind, Connect, Subtree, Refuse: the handle the request carried (for a
    /// request that complete() ends, the one an Open held while it waited).
    /// Drop: a handle the Server held for a request that was pending; none
    /// otherwise.
    ours::Uandle handle = ours::Uandle::Invalid;
    /// Bind: the connection |handle| now serves.
    ConnectionId connection = {};
    /// Connect, Subtree: the value the service or the Subtree node was added
    /// with.
    u64 service = 0;
    /// Forward: the remote entry's channel, where |message| goes.
    ours::Uandle forward_channel = ours::Uandle::Invalid;
    /// Refuse: the status |message|'s epitaph carries, so that the program
    /// need not decode its own answer to report it.
    ours::Status status = ours::Status::Ok;
    /// Refuse: the path the refused Open asked for, whatever it was, a bad
    /// one included; none for a refused Clone, which asks for no path. A view
    /// of the request's bytes, so it holds until the program reads its next
    /// message.
    ustl::Option<std::string_view> path = ustl::NONE;
    /// Subtree: what is left of the Open's path after the Subtree node, "."
    /// when the Open names the node itself. A view of the request's bytes,
    /// so it holds until the program reads its next message.
    std::string_view rest = {};
    /// Subtree: the Open's flags, unchanged.
    prot::io::Flags flags = prot::io::Flags::None;
};

/// One connection, as a Server's table holds it (design "ioservice connection").
/// A program declares an array of them for a Server to use; only the Server
/// reads or writes what is inside.
class Connection final {
  public:
    constexpr Connection() = default;

  private:
    friend class Server;

    /// Where ReadDirents goes on from: "." first, then the directory's
    /// children in name order.
    enum class DirentPhase : u8 {
        Dot,
        Entries,
    };

    /// What the connection is to: a node of the tree, whose kind says what it
    /// serves, or an object a NodeHandler opened.
    enum class Role : u8 {
        Node,
        HandledDirectory,
        HandledFile,
    };

    /// The operation a NodeHandler answered Pending and complete() has not
    /// ended. None is the connection's usual state.
    enum class Waiting : u8 {
        None,
        Open,
        Read,
        ReadDirents,
        GetBackingMemory,
    };

    /// What complete() needs of the request it ends, which the Server kept
    /// because the handler's answer does not carry it. Only the fields of the
    /// connection's |m_waiting| mean anything.
    struct Request {
        /// Open: the flags. Read: the count. ReadDirents: the byte limit of
        /// the whole reply. GetBackingMemory: unused.
        u64 argument = 0;
        /// A two-way request's: the reply carries it.
        u32 txid = 0;
        /// Open: the Handler node it walked to, whose value the handler is
        /// told.
        NodeId node = {};
        /// Open: the request's server end, held until the Open is answered.
        ours::Uandle held = ours::Uandle::Invalid;
    };

    /// The channel this connection is served on, recorded so that a Loop
    /// callback can find it from the connection; the Server never writes or
    /// closes it. Invalid marks a free slot.
    ours::Uandle m_channel = ours::Uandle::Invalid;
    /// A Node's: the node. A handled object's: the Handler node it came from,
    /// whose value tells the handler which of its trees this is.
    NodeId m_node = {};
    prot::io::Operations m_rights = prot::io::Operations::None;
    /// A file's: where Read goes on from. A handled directory's: the cursor
    /// the handler gave for the next ReadDirents, which the Server never reads.
    u64 m_seek = 0;
    /// A handled object's: the handler's name for it, which the Server never
    /// reads.
    u64 m_cookie = 0;
    /// A directory's: where ReadDirents goes on from. In Entries, none means
    /// the listing is done (a Node's; a handled directory's cursor is in
    /// |m_seek|).
    ustl::Option<NodeId> m_next_entry = ustl::NONE;
    /// Never 0 (see ConnectionId).
    u32 m_generation = FIRST_GENERATION;
    /// How many requests the connection has asked its handler about, which
    /// numbers its Tickets. The request a Ticket names is the latest one, and
    /// only while it is pending, so one counter is the whole check.
    u32 m_serial = 0;
    DirentPhase m_phase = DirentPhase::Dot;
    Role m_role = Role::Node;
    /// The request the handler has not answered; None when the connection can
    /// be read.
    Waiting m_waiting = Waiting::None;
    /// What that request left for complete(); empty unless |m_waiting|.
    Request m_request = {};
};

/// Serves the nodes of a Tree over connections kept in storage the program
/// provides, one Connection per client channel.
///
/// Single-threaded: the program calls it from one thread, one message at a
/// time, as the root's Loop does (design D16).
class Server final {
  public:
    /// A server for |tree|, keeping its connections in |connections|. Both
    /// must outlive it; the tree may still grow while it is served.
    /// |handler| answers the Handler nodes of |tree| (NodeKind::Handler); it
    /// must outlive the server too. Without one, an Open that walks to a
    /// Handler node is refused with NotSupported.
    Server(Tree const &tree, std::span<Connection> connections, Syscalls syscalls,
           NodeHandler *handler = nullptr);

    Server(Server const &) = delete;
    auto operator=(Server const &) -> Server & = delete;

    /// Serves the directory |directory| on |channel|, with |rights|, as if a
    /// client had opened it: no message is sent. This is how the root gives
    /// a component a namespace entry or its /svc (design "Events: starting a
    /// component"). Refused with:
    ///   - InvArgs for Uandle::Invalid, a node that is not of the tree, or a
    ///     right ours.io does not define;
    ///   - NotDir when |directory| is not a directory;
    ///   - NoResource when every connection is in use.
    [[nodiscard]]
    auto bind(ours::Uandle channel, NodeId directory, prot::io::Operations rights)
        -> ustl::Result<ConnectionId, ours::Status>;

    /// Handles one message that arrived on |connection|'s channel and says
    /// what to do next. Its reply, or a refused Open's epitaph, is built in
    /// |out|. A stale |connection| is answered Drop.
    ///
    /// A directory connection serves Open, ReadDirents, Clone and Close, and
    /// a file connection Read, GetBackingMemory, Clone and Close. Anything
    /// else is dropped. The same goes for a connection to an object a
    /// NodeHandler opened, which the handler is asked for the content of.
    ///
    /// A request the handler answers Pending is answered Action::Pending, and
    /// ended later by complete(). The connection then has a request
    /// outstanding and is not to be read: a message that comes for it anyway
    /// is dropped and the connection released, as for a message that is not
    /// the protocol's.
    [[nodiscard]]
    auto dispatch(ConnectionId connection, wire::Message message, ReplyBuffer &out) -> Outcome;

    /// Ends the request the handler answered Pending with |ticket|, with the
    /// handler's |result|, and says what to do next, as dispatch() does: the
    /// reply, or the Open's connection or epitaph, is built in |out| and the
    /// program carries it out on the channel of |ticket|'s connection. Like
    /// dispatch() it touches no channel. The connection can be read again
    /// once the program has carried the Outcome out. The overload is the
    /// operation's own: the result of another operation than the request's is
    /// refused.
    ///
    /// A completed Read moves the connection's seek offset by the bytes it
    /// returned, and a ReadDirents moves its cursor to the handler's next; a
    /// result that is an error status moves nothing. A completed Open that
    /// the Server then refuses (a kind the flags do not accept, no free
    /// connection) is answered with an epitaph on the Open's server end, and
    /// the handler's cookie is released.
    ///
    /// Refused, with the connection and the request exactly as they were and
    /// nothing written to |out|:
    ///   - NotFound when |ticket| names no pending request: its connection is
    ///     released or was reused (a stale generation), has no request
    ///     pending (the ticket was spent, or the request was never pending),
    ///     or began a later request since (an earlier serial);
    ///   - InvArgs when |ticket| names a pending request of another
    ///     operation than |result|'s.
    ///
    /// A refused call has taken nothing from |result|: if it carries a VMO
    /// handle, the handle is still the caller's, to close, and if it carries
    /// an object's cookie, the object is still the handler's, to release. An
    /// accepted call has put the handle in the reply (the Outcome's message),
    /// whose write consumes it whether or not the write succeeds, and made
    /// the connection for the cookie, which release() ends; a result that is
    /// an error status has no handle or cookie to take. An Open's server end
    /// is never the caller's: the Server holds it from the Open's dispatch()
    /// and hands it back in the Outcome, or in the Drop that ends the
    /// connection it waits on.
    [[nodiscard]]
    auto complete(Ticket ticket, OpenResult const &result, ReplyBuffer &out)
        -> ustl::Result<Outcome, ours::Status>;

    [[nodiscard]]
    auto complete(Ticket ticket, ReadResult const &result, ReplyBuffer &out)
        -> ustl::Result<Outcome, ours::Status>;

    [[nodiscard]]
    auto complete(Ticket ticket, EntriesResult const &result, ReplyBuffer &out)
        -> ustl::Result<Outcome, ours::Status>;

    [[nodiscard]]
    auto complete(Ticket ticket, VmoResult const &result, ReplyBuffer &out)
        -> ustl::Result<Outcome, ours::Status>;

    /// Ends |connection|: its peer closed, a write to it failed, or the
    /// program could not wait on it. The program closes the channel. A stale
    /// id is ignored. A connection to an object a NodeHandler opened tells the
    /// handler (release()) exactly once, whatever ended it, with a request
    /// pending or not; the ticket of a pending request names nothing from
    /// here on.
    ///
    /// Answers the server end of an Open that was pending on the connection,
    /// which nothing will answer now and the caller closes; Invalid for any
    /// other connection. The program releases a connection from its own
    /// callback, never while a request is pending, so that is a program bug
    /// the answer keeps from leaking a handle.
    auto release(ConnectionId connection) -> ours::Uandle;

    /// The channel |connection| is served on; none for a stale id.
    [[nodiscard]]
    auto channel(ConnectionId connection) const -> ustl::Option<ours::Uandle>;

    /// The rights |connection| holds; none for a stale id.
    [[nodiscard]]
    auto rights(ConnectionId connection) const -> ustl::Option<prot::io::Operations>;

    /// The node |connection| is a connection to; none for a stale id.
    [[nodiscard]]
    auto node(ConnectionId connection) const -> ustl::Option<NodeId>;

  private:
    [[nodiscard]]
    auto find(ConnectionId id) -> Connection *;

    [[nodiscard]]
    auto find(ConnectionId id) const -> Connection const *;

    /// A free slot made into a connection to |node|; none when every slot is
    /// in use.
    [[nodiscard]]
    auto allocate(ours::Uandle channel, NodeId node, prot::io::Operations rights)
        -> ustl::Option<ConnectionId>;

    /// Frees |id|'s slot under a new generation, and nothing else: the
    /// handler is not told. For a connection that was made and never
    /// announced, and for release().
    auto free_slot(ConnectionId id) -> void;

    /// Releases |id| and answers Drop.
    [[nodiscard]]
    auto drop(ConnectionId id) -> Outcome;

    /// A new connection to |node| on |channel|, the request's: Bind, or
    /// NoResource when every connection is in use.
    [[nodiscard]]
    auto open_connection(ours::Uandle channel, NodeId node, prot::io::Operations rights)
        -> ustl::Result<Outcome, ours::Status>;

    /// Clone: a connection to what |connection| is to, on |channel|. A
    /// connection to an object a NodeHandler opened asks the handler for the
    /// new connection's cookie, once the slot is certain.
    [[nodiscard]]
    auto clone(Connection const &connection, ours::Uandle channel)
        -> ustl::Result<Outcome, ours::Status>;

    /// Open's checks, in their fixed order, then the node's own. A target
    /// behind a remote entry is forwarded instead: the Open re-encoded, with
    /// the path left for the remote to walk, into |out|. A Subtree node
    /// answers Action::Subtree, once the checks that come before the node
    /// have passed. A Handler node, or an Open on the connection of a
    /// directory a NodeHandler opened, asks the handler (ask_open()).
    [[nodiscard]]
    auto open(ConnectionId id, Connection &connection, prot::io::OpenRequest const &request,
              ReplyBuffer &out) -> ustl::Result<Outcome, ours::Status>;

    /// Asks the handler of |node|, a Handler node, to open |path|: from the
    /// directory |directory| names, or from the node itself when none. Done is
    /// finished at once (finish_open()); Pending parks the request.
    [[nodiscard]]
    auto ask_open(ConnectionId id, Connection &connection, NodeId node, ustl::Option<u64> directory,
                  std::string_view path, prot::io::Flags flags, ours::Uandle channel)
        -> ustl::Result<Outcome, ours::Status>;

    /// Check 9 for what the handler opened, then the connection on |channel|:
    /// Bind. A refusal releases the cookie, since the handler made it.
    [[nodiscard]]
    auto finish_open(NodeId node, prot::io::Flags flags, ours::Uandle channel,
                     OpenResult const &result) -> ustl::Result<Outcome, ours::Status>;

    [[nodiscard]]
    auto read_dirents(Connection &connection, prot::io::ReadDirentsRequest const &request,
                      ReplyBuffer &out) -> ustl::Result<Outcome, ours::Status>;

    [[nodiscard]]
    auto read(Connection &connection, prot::io::ReadRequest const &request, ReplyBuffer &out)
        -> ustl::Result<Outcome, ours::Status>;

    [[nodiscard]]
    auto get_backing_memory(Connection const &connection,
                            prot::io::GetBackingMemoryRequest const &request, ReplyBuffer &out)
        -> ustl::Result<Outcome, ours::Status>;

    /// The same three on a connection to an object a NodeHandler opened: the
    /// protocol's checks, then the handler.
    [[nodiscard]]
    auto read_dirents_handled(ConnectionId id, Connection &connection,
                              prot::io::ReadDirentsRequest const &request, ReplyBuffer &out)
        -> ustl::Result<Outcome, ours::Status>;

    [[nodiscard]]
    auto read_handled(ConnectionId id, Connection &connection, prot::io::ReadRequest const &request,
                      ReplyBuffer &out) -> ustl::Result<Outcome, ours::Status>;

    [[nodiscard]]
    auto get_backing_memory_handled(ConnectionId id, Connection &connection,
                                    prot::io::GetBackingMemoryRequest const &request,
                                    ReplyBuffer &out) -> ustl::Result<Outcome, ours::Status>;

    /// The reply to a Read, a ReadDirents or a GetBackingMemory once the
    /// handler has answered, now or through complete(): the one place the
    /// connection's offset or cursor moves, and the reply is built.
    [[nodiscard]]
    auto finish_read(Connection &connection, u32 txid, u64 count, ReadResult const &result,
                     ReplyBuffer &out) -> ustl::Result<Outcome, ours::Status>;

    [[nodiscard]]
    auto finish_read_dirents(Connection &connection, u32 txid, u64 max_bytes,
                             EntriesResult const &result, ReplyBuffer &out)
        -> ustl::Result<Outcome, ours::Status>;

    [[nodiscard]]
    auto finish_backing_memory(u32 txid, VmoResult const &result, ReplyBuffer &out)
        -> ustl::Result<Outcome, ours::Status>;

    /// A request the handler is about to be asked about: bumps |connection|'s
    /// serial and names the request with it.
    [[nodiscard]]
    auto issue(ConnectionId id, Connection &connection) -> Ticket;

    /// The handler answered Pending: |connection| waits on |op|, with
    /// |request|. A ticket other than the one issued is the handler's bug,
    /// answered InternalError without parking anything.
    [[nodiscard]]
    auto park(Connection &connection, Ticket issued, Pending answer, Connection::Waiting op,
              Connection::Request request) -> ustl::Result<Outcome, ours::Status>;

    /// Ends the request |connection| waits on and answers what it left.
    [[nodiscard]]
    auto end_request(Connection &connection) -> Connection::Request;

    /// The connection |ticket| names, which must be waiting on |op|; see
    /// complete() for the refusals.
    [[nodiscard]]
    auto claim(Ticket ticket, Connection::Waiting op) -> ustl::Result<Connection *, ours::Status>;

    /// The answer to a two-way request of |method|: |handled|'s reply, or the
    /// status form of its failure. The connection is dropped when even that
    /// cannot be built.
    [[nodiscard]]
    auto answer(ConnectionId id, wire::Method const &method, u32 txid,
                ustl::Result<Outcome, ours::Status> const &handled, ReplyBuffer &out) -> Outcome;

    /// The value of the Handler node |node|, which the handler is told.
    [[nodiscard]]
    auto value_of(NodeId node) const -> u64;

    Tree const *m_tree;
    std::span<Connection> m_connections;
    Syscalls m_syscalls;
    NodeHandler *m_handler;
};
} // namespace ioservice

#endif // #ifndef IOSERVICE_SERVER_HPP
