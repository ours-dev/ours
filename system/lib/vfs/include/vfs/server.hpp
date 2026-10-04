/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef VFS_SERVER_HPP
#define VFS_SERVER_HPP 1

#include <vfs/tree.hpp>

#include <array>
#include <span>

#include <fuchsia_io/fuchsia_io.hpp>
#include <ours/rights.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

/// The fuchsia.io server (component-framework design, "vfs" and "Events: a
/// component opening a file in /boot"): connections to the nodes of a Tree,
/// each with its own rights, and the handlers of the subset's methods.
///
/// The core touches no channel. A program reads a message from a
/// connection's channel, hands it to Server::dispatch(), and gets back an
/// Outcome saying what to write where and which handles to wait on next.
/// The program does that writing, closing and waiting, on its Loop. So the
/// whole protocol, the Open checks included, runs on the host, where the
/// tests drive it message by message.
namespace vfs {
/// Names one connection of a Server: a slot of its table, and the generation
/// the slot was in when the connection was made. A slot is reused once its
/// connection ends, under a new generation, so an id kept past the end is
/// stale and names nothing, rather than naming whichever connection came
/// next. The Loop keys its packets the same way
/// (docs/design/260924/userspace-filesystem.md §4.2).
struct ConnectionId {
    u32 index;
    u32 generation;

    friend constexpr auto operator==(ConnectionId, ConnectionId) -> bool = default;
};

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
    /// none of them was taken. The connection is already released. This is
    /// how Fuchsia's bindings treat a message that fails to decode, and an
    /// unknown strict method; ours's header cannot mark a call flexible.
    Drop,
    /// |handle|, the request's channel, is now the channel of the connection
    /// |connection|: wait on it. If the program cannot (its Loop is full),
    /// it release()s |connection| and closes |handle|.
    Bind,
    /// |handle|, the request's channel, was opened as the service added with
    /// |service|: hand it to that service, which now owns it. Fuchsia's
    /// `Service::connect` calls the node's closure here; ours gives the
    /// choice back to the program, which routes it (design D10, D11).
    Connect,
    /// Open or Clone failed: write |message|, an epitaph, on |handle|, the
    /// request's channel, then close |handle|. The connection the request
    /// came on stays.
    Refuse,
};

/// What dispatch() decided. Only the fields its Action names mean anything.
struct Outcome {
    Action action;
    /// Reply, ReplyAndClose, Refuse: what to write, inside the ReplyBuffer.
    wire::Message message = {};
    /// Bind, Connect, Refuse: the handle the request carried.
    ours::Uandle handle = ours::Uandle::Invalid;
    /// Bind: the connection |handle| now serves.
    ConnectionId connection = {};
    /// Connect: the value the service was added with.
    u64 service = 0;
};

/// One connection, as a Server's table holds it (design "vfs connection").
/// A program declares an array of them for a Server to use; only the Server
/// reads or writes what is inside.
class Connection final {
  public:
    constexpr Connection() = default;

  private:
    friend class Server;

    /// Where ReadDirents goes on from: "." first, as the Rust vfs lists it,
    /// then the directory's children in name order.
    enum class DirentPhase : u8 {
        Dot,
        Entries,
    };

    /// The channel this connection is served on, recorded so that a Loop
    /// callback can find it from the connection; the Server never writes or
    /// closes it. Invalid marks a free slot.
    ours::Uandle m_channel = ours::Uandle::Invalid;
    NodeId m_node = {};
    fuchsia_io::Operations m_rights = fuchsia_io::Operations::None;
    /// A file's: where Read goes on from.
    u64 m_seek = 0;
    /// A directory's: where ReadDirents goes on from. In Entries, none means
    /// the listing is done.
    DirentPhase m_phase = DirentPhase::Dot;
    ustl::Option<NodeId> m_next_entry = ustl::NONE;
    u32 m_generation = 0;
};

/// Serves the nodes of a Tree over connections kept in storage the program
/// provides, as the Rust vfs serves its nodes over `Connection`s.
///
/// Single-threaded: the program calls it from one thread, one message at a
/// time, as the root's Loop does (design D16).
class Server final {
  public:
    /// A server for |tree|, keeping its connections in |connections|. Both
    /// must outlive it; the tree may still grow while it is served.
    Server(Tree const &tree, std::span<Connection> connections, Syscalls syscalls);

    Server(Server const &) = delete;
    auto operator=(Server const &) -> Server & = delete;

    /// Serves the directory |directory| on |channel|, with |rights|, as if a
    /// client had opened it: no message is sent. This is how the root gives
    /// a component a namespace entry or its /svc (design "Events: starting a
    /// component"). Refused with:
    ///   - InvArgs for Uandle::Invalid, a node that is not of the tree, or a
    ///     right fuchsia.io does not define;
    ///   - NotDir when |directory| is not a directory;
    ///   - NoResource when every connection is in use.
    [[nodiscard]]
    auto bind(ours::Uandle channel, NodeId directory, fuchsia_io::Operations rights)
        -> ustl::Result<ConnectionId, ours::Status>;

    /// Handles one message that arrived on |connection|'s channel and says
    /// what to do next. Its reply, or a refused Open's epitaph, is built in
    /// |out|. A stale |connection| is answered Drop.
    ///
    /// A directory connection serves Open, ReadDirents, Clone and Close, and
    /// a file connection Read, GetBackingMemory, Clone and Close. Anything
    /// else is dropped.
    [[nodiscard]]
    auto dispatch(ConnectionId connection, wire::Message message, ReplyBuffer &out) -> Outcome;

    /// Ends |connection|: its peer closed, a write to it failed, or the
    /// program could not wait on it. The program closes the channel. A stale
    /// id is ignored.
    auto release(ConnectionId connection) -> void;

    /// The channel |connection| is served on; none for a stale id.
    [[nodiscard]]
    auto channel(ConnectionId connection) const -> ustl::Option<ours::Uandle>;

    /// The rights |connection| holds; none for a stale id.
    [[nodiscard]]
    auto rights(ConnectionId connection) const -> ustl::Option<fuchsia_io::Operations>;

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
    auto allocate(ours::Uandle channel, NodeId node, fuchsia_io::Operations rights)
        -> ustl::Option<ConnectionId>;

    /// Releases |id| and answers Drop.
    [[nodiscard]]
    auto drop(ConnectionId id) -> Outcome;

    /// A new connection to |node| on |channel|, the request's: Bind, or
    /// NoResource when every connection is in use.
    [[nodiscard]]
    auto open_connection(ours::Uandle channel, NodeId node, fuchsia_io::Operations rights)
        -> ustl::Result<Outcome, ours::Status>;

    /// Open's checks, in the Rust vfs's order, then the node's own.
    [[nodiscard]]
    auto open(Connection const &connection, fuchsia_io::OpenRequest const &request)
        -> ustl::Result<Outcome, ours::Status>;

    [[nodiscard]]
    auto read_dirents(Connection &connection, fuchsia_io::ReadDirentsRequest const &request,
                      ReplyBuffer &out) -> ustl::Result<wire::Message, ours::Status>;

    [[nodiscard]]
    auto read(Connection &connection, fuchsia_io::ReadRequest const &request, ReplyBuffer &out)
        -> ustl::Result<wire::Message, ours::Status>;

    [[nodiscard]]
    auto get_backing_memory(Connection const &connection,
                            fuchsia_io::GetBackingMemoryRequest const &request, ReplyBuffer &out)
        -> ustl::Result<wire::Message, ours::Status>;

    Tree const *m_tree;
    std::span<Connection> m_connections;
    Syscalls m_syscalls;
};
} // namespace vfs

#endif // #ifndef VFS_SERVER_HPP
