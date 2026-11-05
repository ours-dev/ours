/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ioservice/tree.hpp>

#include <algorithm>
#include <utility>

#include <prot/io.hpp>

namespace ioservice {
namespace {
    using ours::Status;

    auto index_of(NodeId const id) -> usize {
        return std::to_underlying(id);
    }

    /// Whether a walk stops at a node of |kind|: what lies below it is not
    /// this tree's. A Subtree node's belongs to the program, a Handler
    /// node's to the NodeHandler, a remote entry's to the server that owns it.
    auto ends_walk(NodeKind const kind) -> bool {
        return kind == NodeKind::Subtree || kind == NodeKind::Handler || kind == NodeKind::Remote;
    }

    /// Splits a path into its segments, one at a time. The caller has checked
    /// the path with is_valid_path() first, or checks each segment itself.
    class Segments final {
      public:
        explicit Segments(std::string_view const path) : m_rest(path) {}

        /// The next segment, or none after the last.
        [[nodiscard]]
        auto next() -> ustl::Option<std::string_view> {
            if (m_rest.is_none()) {
                return ustl::NONE;
            }
            auto &rest = m_rest.value();
            auto const slash = rest.find('/');
            if (slash == std::string_view::npos) {
                // The last segment, and nothing after it.
                return m_rest.take();
            }
            auto const segment = rest.substr(0, slash);
            rest.remove_prefix(slash + 1);
            return ustl::some(segment);
        }

        /// What follows the segment next() returned last: the segments still
        /// to come, joined by '/', and empty after the last.
        [[nodiscard]]
        auto rest() const -> std::string_view {
            if (m_rest.is_none()) {
                return {};
            }
            return m_rest.value();
        }

      private:
        /// What is left to split, or none once the last segment has gone.
        /// Not an empty view: "a/" ends in an empty segment, which next()
        /// still has to return, so empty and finished are different states.
        ustl::Option<std::string_view> m_rest;
    };
} // namespace

auto is_valid_name(std::string_view const name) -> bool {
    if (name.empty() || name.size() > prot::io::MAX_NAME_LENGTH || name == "." || name == "..") {
        return false;
    }
    // A NUL would end the name for any C reader of it, so two programs could
    // disagree about which entry it is.
    constexpr auto FORBIDDEN = std::string_view{"/\0", 2};
    return name.find_first_of(FORBIDDEN) == std::string_view::npos;
}

auto is_valid_path(std::string_view const path) -> bool {
    if (path == ".") {
        return true;
    }
    if (path.size() > prot::io::MAX_PATH_LENGTH) {
        return false;
    }
    // An empty path, a leading or trailing '/' and "//" all show up as an
    // empty segment, which is not a valid name.
    auto segments = Segments(path);
    for (auto segment = segments.next(); segment.is_some(); segment = segments.next()) {
        if (!is_valid_name(*segment)) {
            return false;
        }
    }
    return true;
}

auto Tree::get(NodeId const id) const -> Node const * {
    if (index_of(id) >= m_size) {
        return nullptr;
    }
    return &m_nodes[index_of(id)];
}

auto Tree::find_child(Node const &directory, std::string_view const name) const
    -> ustl::Option<NodeId> {
    for (auto id = directory.m_first_child; id != detail::NO_NODE;
         id = m_nodes[index_of(id)].m_next_sibling) {
        auto const &child = m_nodes[index_of(id)];
        if (child.m_name == name) {
            return ustl::some(id);
        }
        // The list is in name order: past |name|'s place, it is not there.
        if (child.m_name > name) {
            break;
        }
    }
    return ustl::NONE;
}

auto Tree::available() const -> usize {
    // The storage's size, and the index no node may have (NO_NODE ends a list
    // of siblings), whichever comes first.
    auto const limit = std::min<usize>(m_nodes.size(), index_of(detail::NO_NODE));
    return limit - m_size;
}

auto Tree::allocate(Node const &node) -> ustl::Result<NodeId, Status> {
    if (available() == 0) {
        return ustl::err(Status::NoResource);
    }
    auto const id = NodeId{static_cast<u32>(m_size)};
    m_nodes[m_size] = node;
    ++m_size;
    return ustl::ok(id);
}

auto Tree::add_root() -> ustl::Result<NodeId, Status> {
    return allocate(Node{});
}

auto Tree::add_child(NodeId const parent, Node const &node) -> ustl::Result<NodeId, Status> {
    if (!is_valid_name(node.m_name) || get(parent) == nullptr) {
        return ustl::err(Status::InvArgs);
    }
    if (m_nodes[index_of(parent)].m_kind != NodeKind::Directory) {
        return ustl::err(Status::NotDir);
    }

    // Find the child to insert after, keeping the list in name order: the
    // last child whose name sorts before the new one. std::string_view
    // compares as unsigned bytes (std::char_traits<char>), so the order
    // does not depend on whether char is signed.
    auto previous = detail::NO_NODE;
    auto next = m_nodes[index_of(parent)].m_first_child;
    while (next != detail::NO_NODE && m_nodes[index_of(next)].m_name < node.m_name) {
        previous = next;
        next = m_nodes[index_of(next)].m_next_sibling;
    }
    if (next != detail::NO_NODE && m_nodes[index_of(next)].m_name == node.m_name) {
        return ustl::err(Status::AlreadyExists);
    }

    auto added = allocate(node);
    if (added.is_err()) {
        return added;
    }
    auto const id = added.unwrap();
    m_nodes[index_of(id)].m_next_sibling = next;
    if (previous == detail::NO_NODE) {
        m_nodes[index_of(parent)].m_first_child = id;
    } else {
        m_nodes[index_of(previous)].m_next_sibling = id;
    }
    return ustl::ok(id);
}

auto Tree::add_directory(NodeId const parent, std::string_view const name)
    -> ustl::Result<NodeId, Status> {
    auto node = Node{};
    node.m_kind = NodeKind::Directory;
    node.m_name = name;
    return add_child(parent, node);
}

auto Tree::add_file(NodeId const parent, std::string_view const name, VmoFile const &file)
    -> ustl::Result<NodeId, Status> {
    auto node = Node{};
    node.m_kind = NodeKind::File;
    node.m_name = name;
    node.m_file = file;
    return add_child(parent, node);
}

auto Tree::add_service(NodeId const parent, std::string_view const name, u64 const service)
    -> ustl::Result<NodeId, Status> {
    auto node = Node{};
    node.m_kind = NodeKind::Service;
    node.m_name = name;
    node.m_value = service;
    return add_child(parent, node);
}

auto Tree::add_subtree(NodeId const parent, std::string_view const name, u64 const value)
    -> ustl::Result<NodeId, Status> {
    auto node = Node{};
    node.m_kind = NodeKind::Subtree;
    node.m_name = name;
    node.m_value = value;
    return add_child(parent, node);
}

auto Tree::add_handler(NodeId const parent, std::string_view const name, u64 const value)
    -> ustl::Result<NodeId, Status> {
    auto node = Node{};
    node.m_kind = NodeKind::Handler;
    node.m_name = name;
    node.m_value = value;
    return add_child(parent, node);
}
auto Tree::remove_service(NodeId const node) -> Status {
    if (get(node) == nullptr) {
        return Status::NotFound;
    }
    auto &removed = m_nodes[index_of(node)];
    if (removed.m_kind != NodeKind::Service) {
        // A directory, a file, or a node that was already removed: only a
        // live service node is this call's to take.
        return Status::InvArgs;
    }

    // Neither the parent nor the previous sibling is stored on the node, so
    // the unlink walks the directories' children looking for it. Every
    // parent of a service node is a directory (add_child checks), and a
    // node is linked into exactly one list.
    for (auto index = usize{0}; index < m_size; ++index) {
        auto &candidate = m_nodes[index];
        if (candidate.m_kind != NodeKind::Directory) {
            continue;
        }
        auto previous = detail::NO_NODE;
        auto child = candidate.m_first_child;
        while (child != detail::NO_NODE) {
            if (child == node) {
                if (previous == detail::NO_NODE) {
                    candidate.m_first_child = removed.m_next_sibling;
                } else {
                    m_nodes[index_of(previous)].m_next_sibling = removed.m_next_sibling;
                }
                // Out of the list, no walk or listing reaches it; kind()
                // answers none, as for an id that is not a node of this
                // tree. The slot is retired: never reused, so a NodeId
                // never comes to name a different node.
                removed.m_kind = NodeKind::Removed;
                return Status::Ok;
            }
            previous = child;
            child = m_nodes[index_of(child)].m_next_sibling;
        }
    }
    return Status::NotFound;
}

auto Tree::add_remote(NodeId const parent, std::string_view const name, ours::Uandle const channel)
    -> ustl::Result<NodeId, Status> {
    auto node = Node{};
    node.m_kind = NodeKind::Remote;
    node.m_name = name;
    node.m_remote = channel;
    return add_child(parent, node);
}

auto Tree::lookup(NodeId const directory, std::string_view const path) const
    -> ustl::Result<NodeId, Status> {
    auto const resolved = resolve(directory, path);
    if (resolved.is_err()) {
        return ustl::err(resolved.unwrap_err());
    }
    // resolve() stops at a Subtree node, a Handler node or a remote entry,
    // with the path's rest. This walk is over the nodes of this tree, and
    // none of them has any below it that are: a path that goes on past one
    // goes through a node that is not a directory. One that ends on it, rest
    // ".", names the node.
    if (!resolved->rest.empty() && resolved->rest != ".") {
        return ustl::err(Status::NotDir);
    }
    return ustl::ok(resolved->node);
}

auto Tree::resolve(NodeId const directory, std::string_view const path) const
    -> ustl::Result<Resolved, Status> {
    if (!is_valid_path(path)) {
        return ustl::err(Status::BadPath);
    }
    auto const *const start = get(directory);
    if (start == nullptr) {
        return ustl::err(Status::InvArgs);
    }
    if (start->m_kind != NodeKind::Directory) {
        return ustl::err(Status::NotDir);
    }
    if (path == ".") {
        return ustl::ok(Resolved{.node = directory, .rest = {}});
    }

    // One segment at a time from |directory|: with no ".." in a valid path,
    // the walk can only go down, so it never leaves the subtree it started
    // in, which is what makes a connection's node its ceiling. A Subtree
    // node, a Handler node or a remote entry ends the walk where it stands,
    // the rest of the path left for the program, for the handler or for the
    // server that owns the remote's subtree.
    auto current = directory;
    auto rest = path;
    while (!rest.empty()) {
        auto const &node = m_nodes[index_of(current)];
        if (ends_walk(node.m_kind)) {
            return ustl::ok(Resolved{.node = current, .rest = rest});
        }
        if (node.m_kind != NodeKind::Directory) {
            return ustl::err(Status::NotDir);
        }
        auto const slash = rest.find('/');
        auto const segment = rest.substr(0, slash);
        auto const child = find_child(node, segment);
        if (child.is_none()) {
            return ustl::err(Status::NotFound);
        }
        current = *child;
        if (slash == std::string_view::npos) {
            rest = {};
        } else {
            rest.remove_prefix(slash + 1);
        }
    }
    // A path that ends on a Subtree node, a Handler node or a remote entry
    // names the node itself: "." says so, so the program's, the handler's or
    // the remote's own server opens it as its root.
    auto const &last = m_nodes[index_of(current)];
    if (ends_walk(last.m_kind)) {
        return ustl::ok(Resolved{.node = current, .rest = "."});
    }
    return ustl::ok(Resolved{.node = current, .rest = {}});
}
auto Tree::kind(NodeId const node) const -> ustl::Option<NodeKind> {
    auto const *const found = get(node);
    if (found == nullptr) {
        return ustl::NONE;
    }
    return ustl::some(found->m_kind);
}

auto Tree::name(NodeId const node) const -> std::string_view {
    auto const *const found = get(node);
    if (found == nullptr) {
        return {};
    }
    return found->m_name;
}

auto Tree::file(NodeId const node) const -> ustl::Option<VmoFile const &> {
    auto const *const found = get(node);
    if (found == nullptr || found->m_kind != NodeKind::File) {
        return ustl::NONE;
    }
    return ustl::some_ref(found->m_file);
}

auto Tree::service(NodeId const node) const -> ustl::Option<u64> {
    auto const *const found = get(node);
    if (found == nullptr || found->m_kind != NodeKind::Service) {
        return ustl::NONE;
    }
    return ustl::some(found->m_value);
}

auto Tree::subtree(NodeId const node) const -> ustl::Option<u64> {
    auto const *const found = get(node);
    if (found == nullptr || found->m_kind != NodeKind::Subtree) {
        return ustl::NONE;
    }
    return ustl::some(found->m_value);
}

auto Tree::handler(NodeId const node) const -> ustl::Option<u64> {
    auto const *const found = get(node);
    if (found == nullptr || found->m_kind != NodeKind::Handler) {
        return ustl::NONE;
    }
    return ustl::some(found->m_value);
}

auto Tree::remote(NodeId const node) const -> ustl::Option<ours::Uandle> {
    auto const *const found = get(node);
    if (found == nullptr || found->m_kind != NodeKind::Remote) {
        return ustl::NONE;
    }
    return ustl::some(found->m_remote);
}

auto Tree::first_child(NodeId const directory) const -> ustl::Option<NodeId> {
    auto const *const found = get(directory);
    if (found == nullptr || found->m_kind != NodeKind::Directory ||
        found->m_first_child == detail::NO_NODE) {
        return ustl::NONE;
    }
    return ustl::some(found->m_first_child);
}

auto Tree::next_sibling(NodeId const node) const -> ustl::Option<NodeId> {
    auto const *const found = get(node);
    if (found == nullptr || found->m_next_sibling == detail::NO_NODE) {
        return ustl::NONE;
    }
    return ustl::some(found->m_next_sibling);
}

auto printable_path(std::string_view const path, std::span<char> const buffer) -> std::string_view {
    constexpr auto ELLIPSIS = std::string_view{"..."};
    auto const count = std::min(path.size(), buffer.size());
    for (auto index = usize{0}; index < count; ++index) {
        auto const byte = path[index];
        if (byte < ' ' || byte > '~') {
            buffer[index] = '?';
        } else {
            buffer[index] = byte;
        }
    }
    if (path.size() > buffer.size() && buffer.size() >= ELLIPSIS.size()) {
        std::ranges::copy(ELLIPSIS, buffer.end() - ELLIPSIS.size());
    }
    return {buffer.data(), count};
}
} // namespace ioservice
