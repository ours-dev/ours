/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef IOSERVICE_TREE_HPP
#define IOSERVICE_TREE_HPP 1

#include <span>
#include <string_view>

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>

/// The nodes an ioservice::Server serves (component-framework design,
/// "ioservice"): the four node kinds ours needs, kept in storage the program
/// provides.
///
/// A tree is built by the program that serves it, never by a client: the
/// root adds /boot's directories and files at ingest and one /svc directory
/// per component it starts. Clients only read it, through connections
/// (<ioservice/server.hpp>). So there is no removal, and a node, once added, stays
/// where it is for the life of the tree.
namespace ioservice {
/// A node of a Tree: an index into the storage that tree was given. It means
/// something only to the tree that returned it; another tree's id names
/// whatever happens to sit at that index there, or nothing.
enum class NodeId : u32 {};

namespace detail {
    /// Ends a list of siblings. No node is ever given this index: a Tree
    /// answers NoResource before its storage reaches it.
    inline constexpr auto NO_NODE = NodeId{0xFFFF'FFFF};
} // namespace detail

/// The five kinds of node: a pseudo-directory, a file backed by a VMO, a
/// service, a directory whose subtree another server owns, and a place the
/// program serves itself.
enum class NodeKind : u8 {
    /// A pseudo-directory: named children, in name order.
    Directory,
    /// A read-only file whose contents are a VMO.
    File,
    /// A protocol endpoint. Opening it hands the request's channel back to
    /// the program serving the tree, with the value given to add_service().
    Service,
    /// A directory whose subtree another server owns, reached through the
    /// channel add_remote() keeps. An Open that reaches one is forwarded
    /// there with the rest of its path; the subtree behind it is not this
    /// tree's to walk or list.
    Remote,
    /// A place whose tree is the program's own, not this one's. An Open that
    /// walks to it goes back to the program, with the part of its path after
    /// the node and the value given to add_subtree() (Action::Subtree of
    /// <ioservice/server.hpp>). To the tree it is a leaf: nothing is added
    /// below it, and a walk stops at it (Tree::resolve()). To a client that
    /// lists the directory it is in, it is a directory.
    Subtree,
};

/// What a File node serves. Nothing here is owned by the tree: the program
/// keeps the VMO and the bytes alive for as long as the tree is served.
struct VmoFile {
    /// The file's VMO. GetBackingMemory hands out duplicates of it with Basic,
    /// Map, GetProperty and Read, plus Execute when asked, so it must hold
    /// those rights and Duplicate. Nobody may be able to write it: a private
    /// clone is answered with a duplicate too (design D21), which is only a
    /// private copy when the contents never change.
    ours::Uandle vmo;

    /// The file's contents as this process sees them, where File.Read copies
    /// from (design D17). Its size is the file's size.
    std::span<u8 const> bytes;

    /// Whether a connection may hold EXECUTE, so that GetBackingMemory can
    /// hand out a VMO with Execute. The VMO must then hold Execute itself.
    bool executable;
};

/// One node, as a Tree's storage holds it. A program declares an array of
/// them for a Tree to use; only the Tree reads or writes what is inside.
class Node final {
  public:
    constexpr Node() = default;

  private:
    friend class Tree;

    NodeKind m_kind = NodeKind::Directory;
    /// Empty for a root.
    std::string_view m_name;
    /// The sibling lists are threaded through the nodes, so that a directory
    /// needs no storage of its own for its children.
    NodeId m_first_child = detail::NO_NODE;
    NodeId m_next_sibling = detail::NO_NODE;
    VmoFile m_file = {};
    /// A Service's or a Subtree's: the value the program gave it.
    u64 m_value = 0;
    /// A Remote node's: this process's end of the connection to the server
    /// that owns the subtree.
    ours::Uandle m_remote = ours::Uandle::Invalid;
};

/// Where Tree::resolve() ended a walk: the node it stopped at, and what was
/// left of the path.
struct Resolved {
    NodeId node;
    /// What is left of the path when the walk stopped at a Subtree node or a
    /// remote entry: the segments after the node, joined by '/', a valid
    /// ours.io path. "." when the path ended on the node itself, so that it
    /// is always something an Open can be handed on with. Empty when the
    /// walk ended on any other node, so |rest| is non-empty exactly when
    /// |node| is a Subtree node or a remote entry.
    /// A view of the path that was resolved (or of a literal, for "."), valid
    /// for as long as that path is.
    std::string_view rest;
};

/// A set of node trees in storage the program provides.
///
/// Nothing is allocated: user programs have no heap
/// (system/lib/ort/src/cpp_dependencies.cpp), so the program sizes the
/// storage, and a node past it answers NoResource (design D27).
///
/// A directory keeps its children in one list in name order, compared byte
/// by byte, so a listing comes out sorted. Finding a child walks that
/// list, which is linear in the number of children; see
/// docs/repairs/261004/ioservice.md.
class Tree final {
  public:
    /// A tree whose nodes live in |storage|, which must outlive it.
    explicit Tree(std::span<Node> storage) : m_nodes(storage) {}

    /// A Server keeps a pointer to the tree it serves.
    Tree(Tree const &) = delete;
    auto operator=(Tree const &) -> Tree & = delete;

    /// A directory with no parent: the root of a tree served on its own, such
    /// as the root's /boot, or one component's /svc. NoResource when the
    /// storage is full.
    [[nodiscard]]
    auto add_root() -> ustl::Result<NodeId, ours::Status>;

    /// Adds a child named |name| to the directory |parent|: an empty
    /// directory, a file serving |file|, or a service that Open hands back
    /// with |service|, a value the program chooses to tell its services apart.
    ///
    /// |name| is kept as a view, so the bytes it points to must outlive the
    /// tree: the BootFS mapping, say, or a manifest's string table. Refused
    /// with:
    ///   - InvArgs when |name| is not a valid ours.io name (is_valid_name)
    ///     or |parent| is not a node of this tree;
    ///   - NotDir when |parent| is not a directory;
    ///   - AlreadyExists when |parent| has a child of that name already;
    ///   - NoResource when the storage is full.
    [[nodiscard]]
    auto add_directory(NodeId parent, std::string_view name) -> ustl::Result<NodeId, ours::Status>;

    [[nodiscard]]
    auto add_file(NodeId parent, std::string_view name, VmoFile const &file)
        -> ustl::Result<NodeId, ours::Status>;

    [[nodiscard]]
    auto add_service(NodeId parent, std::string_view name, u64 service)
        -> ustl::Result<NodeId, ours::Status>;

    /// Adds a child named |name| to the directory |parent| whose subtree is
    /// another server's: |channel| is this process's end of a connection to
    /// that server's directory, and an Open that reaches the child is
    /// forwarded there with the rest of its path. The channel is kept for
    /// the life of the tree. Refused as add_directory is.
    [[nodiscard]]
    auto add_remote(NodeId parent, std::string_view name, ours::Uandle channel)
        -> ustl::Result<NodeId, ours::Status>;

    /// Adds a Subtree node named |name| to |parent|, refused as above. |value|
    /// is what the program chooses to tell its Subtree nodes apart by, and
    /// what an Open that reaches the node comes back with. Nothing can be
    /// added below the node: it is not a directory, so |parent| naming it is
    /// NotDir.
    [[nodiscard]]
    auto add_subtree(NodeId parent, std::string_view name, u64 value)
        -> ustl::Result<NodeId, ours::Status>;

    /// The node |path| names under the directory |directory|, walked one
    /// segment at a time from that node, never by joining strings, so that a
    /// path cannot leave the subtree it starts in. "." is |directory| itself.
    /// A remote entry is a leaf here: a path that goes on below one is
    /// refused with NotDir, its subtree belonging to the server that owns it.
    /// Refused with:
    ///   - BadPath when |path| is not a valid ours.io path
    ///     (is_valid_path);
    ///   - InvArgs when |directory| is not a node of this tree;
    ///   - NotDir when |directory|, or a segment before the last, is not a
    ///     directory (a Subtree node is not one);
    ///   - NotFound when a segment does not exist.
    [[nodiscard]]
    auto lookup(NodeId directory, std::string_view path) const
        -> ustl::Result<NodeId, ours::Status>;

    /// lookup() for a client's Open: the same walk, which stops at the first
    /// Subtree node or remote entry it meets, the last segment's included,
    /// because what lies below either is not this tree's -- the program's or
    /// the remote server's. Then the answer is the node and the rest of
    /// |path| (Resolved::rest), and no segment after the node is looked at,
    /// so it may not exist. A walk that meets neither ends where lookup()
    /// does, with an empty rest. Refused as lookup() is, up to the point the
    /// walk stops.
    [[nodiscard]]
    auto resolve(NodeId directory, std::string_view path) const
        -> ustl::Result<Resolved, ours::Status>;

    /// What |node| is; none when it is not a node of this tree.
    [[nodiscard]]
    auto kind(NodeId node) const -> ustl::Option<NodeKind>;

    /// |node|'s name in its directory; empty for a root, or for an id that
    /// is not a node of this tree.
    [[nodiscard]]
    auto name(NodeId node) const -> std::string_view;

    /// What the file |node| serves; none when it is not a file.
    [[nodiscard]]
    auto file(NodeId node) const -> ustl::Option<VmoFile const &>;

    /// The value the service |node| was added with; none when it is not a
    /// service.
    [[nodiscard]]
    auto service(NodeId node) const -> ustl::Option<u64>;

    /// The channel the remote |node| forwards to; none when it is not a
    /// remote entry.
    [[nodiscard]]
    auto remote(NodeId node) const -> ustl::Option<ours::Uandle>;

    /// The value the Subtree |node| was added with; none when it is not a
    /// Subtree node.
    [[nodiscard]]
    auto subtree(NodeId node) const -> ustl::Option<u64>;

    /// How many more nodes the storage has room for: what is left before an
    /// add answers NoResource. A caller that has to add several nodes or none
    /// asks first, since a node, once added, stays.
    [[nodiscard]]
    auto available() const -> usize;

    /// The first child of the directory |directory|, in name order; none when
    /// it has none, or is not a directory of this tree.
    [[nodiscard]]
    auto first_child(NodeId directory) const -> ustl::Option<NodeId>;

    /// The child after |node| in its directory's name order; none after the
    /// last, or for an id that is not a node of this tree.
    [[nodiscard]]
    auto next_sibling(NodeId node) const -> ustl::Option<NodeId>;

  private:
    /// The node |id| names, or nullptr when it is not a node of this tree.
    [[nodiscard]]
    auto get(NodeId id) const -> Node const *;

    /// Puts |node| in the next free place of the storage. NoResource when
    /// there is none.
    [[nodiscard]]
    auto allocate(Node const &node) -> ustl::Result<NodeId, ours::Status>;

    /// Links |node|, filled in but for its links, into |parent|'s children at
    /// its place in name order.
    [[nodiscard]]
    auto add_child(NodeId parent, Node const &node) -> ustl::Result<NodeId, ours::Status>;

    /// The child of |directory| named |name|, if there is one.
    [[nodiscard]]
    auto find_child(Node const &directory, std::string_view name) const -> ustl::Option<NodeId>;

    std::span<Node> m_nodes;
    /// Nodes are never removed, so the first m_size are the live ones.
    usize m_size = 0;
};

/// Whether |name| is a valid ours.io name: not empty, at most MAX_NAME_LENGTH
/// bytes, not "." or "..", and holding no '/' and no NUL. Any other byte is
/// allowed: a name is bytes, not UTF-8.
[[nodiscard]]
auto is_valid_name(std::string_view name) -> bool;

/// Whether |path| is a valid ours.io path (design D21): at most
/// MAX_PATH_LENGTH bytes, and either exactly "." or valid names joined by
/// '/'. So it is not empty and has no leading or trailing '/', no "//", and
/// no "." or ".." segment. A client sends this canonical form (fsio cleans
/// every path first), and the server holds it to that, so a path has one
/// spelling.
[[nodiscard]]
auto is_valid_path(std::string_view path) -> bool;

/// |path|, as a client sent it, made fit for one line of a log: copied into
/// |buffer| and returned as the view of what was copied. A path is bytes the
/// client chose, so a line that quoted it as it came would let a client end
/// the line and write another, or send the console a control sequence. Every
/// byte outside printable ASCII (0x20 to 0x7E) becomes '?'. A path longer
/// than |buffer| is cut to fit and ends in "...", when |buffer| has room for
/// them. Allocates nothing; the view lives as long as |buffer| does.
[[nodiscard]]
auto printable_path(std::string_view path, std::span<char> buffer) -> std::string_view;
} // namespace ioservice

#endif // #ifndef IOSERVICE_TREE_HPP
