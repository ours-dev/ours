/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef IOSERVICE_OUTGOING_HPP
#define IOSERVICE_OUTGOING_HPP 1

#include <ioservice/loop_server.hpp>
#include <ioservice/server.hpp>
#include <ioservice/tree.hpp>

#include <array>
#include <string_view>

#include <loop/loop.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <prot/io.hpp>
#include <wire/wire.hpp>

/// The provider side of a component (component-framework design,
/// "Outgoing"): the outgoing directory a component serves on the
/// DirectoryRequest handle its runner hands it. It holds two kinds of thing:
///
///   svc/<name>         a protocol the component provides (add_protocol()):
///                      a service node, whose Open hands the request's channel
///                      to the program, which serves it however it likes;
///   export/data/...    a directory capability the component declares
///                      (add_directory()), at the path its declaration names,
///                      with a tree under it that the program fills in. The
///                      tree may hold service nodes of its own
///                      (add_service()), which hand an Open's channel to the
///                      program exactly as a protocol's does;
///   export/data        or the same declared path served by the program's own
///                      code (add_subtree()): a Subtree node, which hands the
///                      program every Open that reaches it, with the rest of
///                      the path, to serve from a tree of its own.
///
/// The objects:
///   - the tree: the outgoing directory, its svc/ directory, a service node
///     per protocol and the tree of each directory capability, all in a
///     Tree (<ioservice/tree.hpp>);
///   - the LoopServer that serves the tree on the Loop, with its
///     connection table: the DirectoryRequest channel is the first
///     connection, and an Open or Clone of a directory through it makes
///     another, each with a Loop slot;
///   - a Handler per protocol and per service node, which an Open of the
///     node hands the request's channel to, and a SubtreeHandler per Subtree
///     node, which gets the Open's rest of path, flags and channel. They are
///     one table, whichever call added them.
///
/// The events: the root routes a client's Open of /svc/<name> to this
/// component and writes Open("svc/<name>", PROTOCOL_SERVICE, channel) on the
/// outgoing directory's client end. The connection's slot fires, the
/// LoopServer finds the service node and calls back into this class with the
/// node's value, and the protocol's Handler gets the channel. From then on the
/// client and this component talk directly: the Handler usually adds the
/// channel to the same Loop.
///
/// A directory capability is routed the same way, by one Open the root writes
/// when the consumer starts: Open("export/data/<subdir>", rights |
/// PROTOCOL_DIRECTORY, channel). The server walks the path like any other,
/// and a directory node at its end makes a connection on the channel with
/// exactly the rights asked for; from then on the consumer and this
/// component talk directly, and every Open through that connection is held
/// to its rights. The Open needs no code of this class: the tree is the whole
/// of it. When the program serves the declared path itself (add_subtree()),
/// the server checks the Open as it would any other, up to the Subtree node,
/// and the program's SubtreeHandler gets the rest of the path, the flags and
/// the channel.
///
/// An Open the outgoing directory itself refuses, the usual one being a name
/// it does not serve because the manifest and add_protocol() or
/// add_directory() spell the protocol or the path differently, is answered
/// with an epitaph the client reads and nothing else, so the Outgoing says so
/// on the debug console with one line: the path the Open asked for and the
/// status. The outgoing directory here is the root and svc/, the two places
/// the root's Opens arrive at. An Open refused through a connection of a
/// directory capability, at or below its declared path, is one of the
/// consumer's, who reads the epitaph: it is not said, so that a consumer
/// cannot fill the provider's console. The line is written with the vDSO's
/// debug_write, as handles are closed with the vDSO's handle_close, so a
/// program has nothing to set up and the library needs no print helper of
/// its own.
///
/// One thread, as the Loop it runs on: nothing is locked, and a callback runs
/// to completion before the next one starts.
namespace ioservice {

/// What an Open of one protocol, or of one service node in a directory
/// capability's tree, is handed to: the program's object that serves it, and
/// derives from this. Nothing is deleted through a Handler, so its destructor
/// is not virtual: a virtual one would add a deleting destructor, which a
/// program with no heap has no use for.
class Handler {
  public:
    /// |channel| is the request's, and the function owns it from the call on:
    /// it serves it, typically by adding it to the Loop, or closes it. It
    /// runs inside a Loop callback, so it may add() slots but must not wait.
    virtual auto connect(ours::Uandle channel) -> void = 0;

  protected:
    Handler() = default;
    Handler(Handler const &) = default;
    auto operator=(Handler const &) -> Handler & = default;
    ~Handler() = default;
};

/// What an Open that walks to a Subtree node of a directory capability's tree
/// is handed to: the program's object that serves the declared path itself,
/// and derives from this. It is the counterpart of Handler for a place that is
/// a tree and not a protocol. Nothing is deleted through one, so its
/// destructor is not virtual, for Handler's reason.
class SubtreeHandler {
  public:
    /// An Open reached the node this handler was added with (add_subtree()).
    /// The outgoing directory has already held the Open to what it checks on
    /// the connection it came on: a valid path, TRAVERSE, rights that are a
    /// subset of the connection's, no creation flag, the walk to the node. So
    /// |flags| ask for no more than that connection holds, and are the
    /// consumer's own, unchanged; whatever the handler makes of |channel|
    /// must be held to the rights in them, never to more. What it does not
    /// check is the rest of the path and the flags' fit to what that names:
    /// they are its own tree's.
    ///
    /// |rest| is the part of the Open's path after the node, "." when the
    /// Open names the node itself. It is a view of the request, valid for the
    /// call only: a handler that parks the Open to answer it later copies it.
    ///
    /// |channel| is the Open's, and the function owns it from the call on: it
    /// serves it, forwards it, answers it with an epitaph and closes it, or
    /// closes it. No connection of the outgoing directory was made for it. It
    /// runs inside a Loop callback, so it may add() slots but must not wait.
    virtual auto open(std::string_view rest, prot::io::Flags flags, ours::Uandle channel)
        -> void = 0;

  protected:
    SubtreeHandler() = default;
    SubtreeHandler(SubtreeHandler const &) = default;
    auto operator=(SubtreeHandler const &) -> SubtreeHandler & = default;
    ~SubtreeHandler() = default;
};

/// One outgoing directory, served on a Loop. It is the LoopServer's Delegate:
/// an Open of a service node, or of a Subtree node, comes back to it with the
/// node's value.
class Outgoing final: private LoopServer::Delegate {
  public:
    /// How many handlers one outgoing directory may hold: its protocols
    /// (add_protocol()), its service nodes (add_service()) and its Subtree
    /// nodes (add_subtree()) together, since all three register their handler
    /// in the one table that a node's value indexes. So it is a limit on the
    /// sum: eight protocols leave no room for a service node or a Subtree
    /// node, and so on in any order. The constant keeps the name it had
    /// before add_service() and add_subtree() shared the table.
    static constexpr auto MAX_PROTOCOLS = usize{8};

    /// How many connections to the outgoing directory may be open at once:
    /// the root's own, on the DirectoryRequest channel, and whatever Clone or
    /// Open of a directory is made through it. A directory capability spends
    /// one on each consumer's entry, one on every directory or file a
    /// consumer opens below it, and one on each clone made of an entry. An
    /// Open of a protocol makes none: its channel goes to the protocol's
    /// Handler. A program's Loop needs a slot for each, so its table is this
    /// plus whatever else it waits on.
    static constexpr auto MAX_CONNECTIONS = usize{32};

    /// How many nodes the tree holds: the outgoing root, svc/ and a service
    /// node per protocol (2 + MAX_PROTOCOLS of them once every protocol is
    /// added), and the rest for the directory capabilities: a node for each
    /// segment of a declared path, a segment two paths share once, and every
    /// directory and file the program adds below it. The nodes are one pool,
    /// not a share for each use, so a program that wants all its protocols
    /// adds them before a large tree.
    static constexpr auto MAX_NODES = usize{32};
    static_assert(MAX_NODES >= 2 + MAX_PROTOCOLS);

    /// The directory protocols live in, inside the outgoing directory: where
    /// the root forwards an Open to.
    static constexpr auto SVC_DIRECTORY = std::string_view{"svc"};

    /// The rights the outgoing directory is served with: every right ours.io
    /// defines. This is the ceiling of the root's first Open of a declared
    /// directory, so the only limit on it is the route's own check. A file in
    /// the tree is a VmoFile, which the server never lets be written and lets
    /// be run only if it was added executable, so no right reaches anything
    /// it should not; whoever opens it is held to its own Open's rights.
    static constexpr auto OUTGOING_RIGHTS =
        prot::io::Operations::Connect | prot::io::Operations::ReadBytes |
        prot::io::Operations::WriteBytes | prot::io::Operations::Execute |
        prot::io::Operations::GetAttributes | prot::io::Operations::UpdateAttributes |
        prot::io::Operations::Enumerate | prot::io::Operations::Traverse |
        prot::io::Operations::ModifyDirectory;

    /// One place of the handler table: the handler a node's value indexes.
    /// Whichever call added the node set exactly one of the two pointers, a
    /// service handler for a protocol or a service node and a subtree handler
    /// for a Subtree node, and the other stays null.
    struct HandlerEntry {
        Handler *service = nullptr;
        SubtreeHandler *subtree = nullptr;
    };

    /// Everything the directory keeps, sized by MAX_NODES, MAX_PROTOCOLS and
    /// MAX_CONNECTIONS: about 13 KB (13,064 bytes in a user program), two
    /// thirds of it the message buffers, so a program declares it constinit,
    /// outside its stack. Only the Outgoing reads or writes what is inside.
    struct Storage {
        /// The outgoing directory, svc/, a service node per protocol, and the
        /// trees of the directory capabilities.
        std::array<ioservice::Node, MAX_NODES> nodes;
        std::array<ioservice::Connection, MAX_CONNECTIONS> connections;
        std::array<ioservice::LoopServer::Binding, MAX_CONNECTIONS> bindings;
        /// Indexed by the value each service or Subtree node was added with:
        /// the protocols' and the directory capabilities' alike.
        std::array<HandlerEntry, MAX_PROTOCOLS> handlers;
        /// The node at the end of each declared path (a directory, or a
        /// Subtree node), in the order they were added. A declaration takes
        /// a node of its own, so there are never more than MAX_NODES.
        std::array<NodeId, MAX_NODES> directories;
        /// Where a connection's callback reads the one message it handles,
        /// and where the server builds its answer. One of each serves every
        /// callback, since one callback runs at a time.
        std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
        std::array<ours::Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
        ReplyBuffer reply;
    };

    /// An empty outgoing directory, holding svc/ and nothing in it, whose
    /// tree and connections live in |storage| and whose connections wait on
    /// |loop|. Both must outlive it. Nothing is served until serve().
    Outgoing(Storage &storage, loop::Loop &loop);

    /// The server and its slots keep pointers to this object.
    Outgoing(Outgoing const &) = delete;
    auto operator=(Outgoing const &) -> Outgoing & = delete;

    /// Serves the protocol |name| at svc/<name>: an Open of it hands the
    /// request's channel to |handler|. It may be called before or after
    /// serve(): an Open finds what is there when it is handled. |name| is
    /// kept as a view and |handler| by address, so both must outlive the
    /// directory. Refused with:
    ///   - InvArgs when |name| is not a valid ours.io name
    ///     (is_valid_name());
    ///   - AlreadyExists when svc/ has an entry of that name;
    ///   - NoResource once MAX_PROTOCOLS handlers are held, protocols and
    ///     service nodes together (add_service()).
    [[nodiscard]]
    auto add_protocol(std::string_view name, Handler &handler) -> ours::Status;

    /// Puts a directory capability's tree at its declared path: makes the
    /// directory |path| ends at, and every directory missing on the way,
    /// and returns the last one. |path| is the absolute path the manifest's
    /// declaration states, such as "/export/data". The program fills the tree
    /// below the returned node with the calls that follow, and the root's
    /// Open of "export/data" and of what lies under it reaches it. Two paths
    /// may share the directories they begin with, "/export/a" and
    /// "/export/b" sharing export/.
    ///
    /// May be called before or after serve(). |path| is kept as views, one
    /// for each directory's name, so its bytes must outlive the directory.
    /// Refused with:
    ///   - InvArgs when |path| is not absolute, is "/", or holds an empty,
    ///     "." or ".." segment or a segment that is not a valid ours.io name
    ///     (is_valid_path()), or is "/svc" or lies under it, where the
    ///     protocols are (by segments: "/svcx" is fine);
    ///   - AlreadyExists when |path| is equal to, above or below a declared
    ///     path already added (with this call or add_subtree()), by segments
    ///     ("/export/data2" is not below "/export/data"). The manifest
    ///     compiler refuses the same overlaps; they are held again here
    ///     because a program can be wrong without a manifest;
    ///   - NoResource when the nodes it needs do not fit.
    /// A refused call changes nothing: it is judged whole before the first
    /// directory is made, since a node, once added, stays.
    [[nodiscard]]
    auto add_directory(std::string_view path) -> ustl::Result<NodeId, ours::Status>;

    /// Adds an empty directory named |name| to |parent|, a node that
    /// add_directory(path) or this call returned, so that the tree of a
    /// directory capability can be filled in. Refused as Tree::add_directory()
    /// refuses it. |name| is kept as a view, so its bytes must outlive the
    /// directory.
    ///
    /// The tree itself is not reachable: the Outgoing keeps the service nodes
    /// under svc/ pointing at the handlers it holds, and the declared paths
    /// it has checked, and a program that could add a node anywhere could
    /// break either.
    [[nodiscard]]
    auto add_directory(NodeId parent, std::string_view name) -> ustl::Result<NodeId, ours::Status>;

    /// Adds a file named |name| serving |file| to |parent|, as above. The
    /// bytes |name| and |file| view must outlive the directory.
    [[nodiscard]]
    auto add_file(NodeId parent, std::string_view name, VmoFile const &file)
        -> ustl::Result<NodeId, ours::Status>;

    /// Adds a service node named |name| to |parent|, a directory capability's
    /// tree: the node add_directory(path) returned or one made below it. An
    /// Open of the node with PROTOCOL_SERVICE, by a connection that holds
    /// CONNECT, hands the request's channel to |handler|'s connect(), as an
    /// Open of svc/<name> hands it to a protocol's; an Open of it as a
    /// directory or a file is refused with NotDir or NotFile, and a listing
    /// of its directory shows it as a service. The connection the Open comes
    /// through must hold CONNECT, as every use that asks for r* does, so a
    /// consumer that can read the directory can also connect to the node.
    ///
    /// The node is in the tree of the capability, never under svc/, which is
    /// add_protocol()'s: |parent| that is not at or below a declared
    /// directory, such as the outgoing root, svc/, the directories on the way
    /// to a declared path, or an id of no node, is refused.
    ///
    /// |handler| is registered in the table add_protocol() fills, so
    /// MAX_PROTOCOLS limits the two together. |name| is kept as a view and
    /// |handler| by address, so both must outlive the directory. May be
    /// called before or after serve(). Refused with, in this order:
    ///   - InvArgs when |parent| is not a node of a declared directory's tree
    ///     (above);
    ///   - NoResource when MAX_PROTOCOLS handlers are held already;
    ///   - what Tree::add_service() refuses with: InvArgs for a |name| that is
    ///     not a valid ours.io name, NotDir when |parent| is not a directory,
    ///     AlreadyExists when it has a child of that name, and NoResource when
    ///     the nodes are used up.
    /// A refused call changes nothing. The handler table is judged before the
    /// node is added, since a node, once added, stays: one added without a
    /// handler to give it would be an Open that reaches nothing.
    [[nodiscard]]
    auto add_service(NodeId parent, std::string_view name, Handler &handler)
        -> ustl::Result<NodeId, ours::Status>;

    /// Serves a directory capability's declared path by the program's own
    /// code: makes a Subtree node at |path|, and every directory missing on
    /// the way, and returns the node. An Open that walks to it, the root's
    /// Open of "export/data/<subdir>" for one, goes to |handler|'s open()
    /// with the part of the path after the node, the Open's flags and its
    /// channel, once the outgoing directory has checked what it checks on the
    /// connection the Open came on. No connection is made, and nothing can be
    /// added below the node: what lies below is |handler|'s.
    ///
    /// It is a declaration as add_directory(path) is, at the path the
    /// manifest's declaration states, with the same rules: the path is
    /// absolute, and the node is at "/export/data" and not below an
    /// add_directory() node, since the manifest declares the path and the
    /// program serves the place the declaration names. Two paths may share
    /// the directories they begin with.
    ///
    /// |handler| is registered in the table add_protocol() and add_service()
    /// fill, so MAX_PROTOCOLS limits the three together. May be called before
    /// or after serve(). |path| is kept as views, one for each node's name,
    /// and |handler| by address, so both must outlive the directory. Refused
    /// with, in this order:
    ///   - InvArgs when |path| is not absolute, is "/", or holds an empty,
    ///     "." or ".." segment or a segment that is not a valid ours.io name,
    ///     or is "/svc" or lies under it (by segments: "/svcx" is fine);
    ///   - AlreadyExists when |path| is equal to, above or below a declared
    ///     path already added with add_directory() or add_subtree(), by
    ///     segments;
    ///   - NoResource when the nodes the path needs do not fit, or when
    ///     MAX_PROTOCOLS handlers are held already.
    /// A refused call changes nothing: it is judged whole before the first
    /// node is made, since a node, once added, stays.
    [[nodiscard]]
    auto add_subtree(std::string_view path, SubtreeHandler &handler)
        -> ustl::Result<NodeId, ours::Status>;

    /// Serves the outgoing directory on |directory_request|, the server end
    /// of the DirectoryRequest startup handle, with OUTGOING_RIGHTS, and
    /// waits on it. The program takes the handle itself, with
    /// ort::take_startup_handle(DirectoryRequest, 0), and passes it here, so
    /// that this library runs on the host too.
    ///
    /// On Ok the channel is the Loop's, its slot's to close. On failure
    /// nothing changed and the channel is still the caller's: InvArgs for
    /// Uandle::Invalid, NoResource when every connection or every Loop slot
    /// is in use, or what the Loop's add() answered.
    [[nodiscard]]
    auto serve(ours::Uandle directory_request) -> ours::Status;

  private:
    /// Hands |channel|, the request's, to the handler of the protocol added
    /// as |service|, the value its service node was added with.
    auto connect(u64 service, ours::Uandle channel) -> void override;

    /// Hands |channel|, the request's, to the subtree handler added as
    /// |value|, the value its Subtree node was added with, with the rest of
    /// the Open's path and its flags.
    auto subtree(u64 value, std::string_view rest, prot::io::Flags flags, ours::Uandle channel)
        -> void override;

    /// Writes the one line that says an Open of |path| was refused with
    /// |status|, when |directory| is the outgoing root or svc/. The directory
    /// is not named: the root's Opens arrive at one of those two, and say a
    /// path from it. An Open refused below a declared path is not said.
    auto open_refused(ioservice::NodeId directory, std::string_view path, ours::Status status)
        -> void override;

    /// A declared path judged: where its walk leaves the tree, and what it
    /// leaves to be made.
    struct Declaration {
        /// The node the path leaves the tree at: the last one it shares.
        NodeId parent;
        /// The segments of the path after |parent|, joined by '/': never
        /// empty, and each one a node to be made, the last the declaration's.
        std::string_view missing;
    };

    /// Judges the declared path |path| for add_directory() and
    /// add_subtree(), which differ only in the kind of node at its end: its
    /// syntax, svc/, the overlap with a declared path, and the room for the
    /// nodes it needs, in that order. Nothing is changed.
    [[nodiscard]]
    auto judge(std::string_view path) const -> ustl::Result<Declaration, ours::Status>;

    /// Makes the directories on the way of |declaration|, every missing
    /// segment but the last, and returns the node the last goes in. That
    /// segment is all that is left in |declaration.missing|. It cannot fail
    /// once judge() passed.
    [[nodiscard]]
    auto make_parents(Declaration &declaration) -> NodeId;

    /// Whether |node| is the end of a path add_directory() or add_subtree()
    /// was given.
    [[nodiscard]]
    auto is_declared(NodeId node) const -> bool;

    /// Whether |node| is a declared directory or lies below one: a node of a
    /// directory capability's tree, where the program's own nodes go.
    [[nodiscard]]
    auto in_declared_tree(NodeId node) const -> bool;

    Storage *m_storage;
    Tree m_tree;
    LoopServer m_server;
    NodeId m_root;
    NodeId m_svc;
    /// The first m_handlers entries of Storage::handlers are in use: the
    /// protocols', the directory capabilities' service nodes' and their
    /// Subtree nodes', in the order they were added.
    usize m_handlers = 0;
    /// The first m_declared entries of Storage::directories are in use.
    usize m_declared = 0;
};
} // namespace ioservice

#endif // #ifndef IOSERVICE_OUTGOING_HPP
