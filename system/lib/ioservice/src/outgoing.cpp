/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ioservice/outgoing.hpp>

#include <algorithm>
#include <array>
#include <span>

#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

namespace ioservice {
namespace {
    using ours::Status;
    using ours::Uandle;

    /// How much of a refused path the line shows: a path may be as long as
    /// ours.io allows, which is more than one debug_write takes. A longer
    /// path is cut (printable_path()).
    constexpr auto MAX_SHOWN_PATH = usize{128};

    /// Takes the first segment off |rest|, which holds a valid path or
    /// nothing, and leaves what follows its '/'. So the last segment leaves
    /// |rest| empty, and a valid path never has an empty one before it.
    auto take_segment(std::string_view &rest) -> std::string_view {
        auto const slash = rest.find('/');
        auto const segment = rest.substr(0, slash);
        if (slash == std::string_view::npos) {
            rest = {};
        } else {
            rest.remove_prefix(slash + 1);
        }
        return segment;
    }

    /// Whether |node| is |ancestor| or lies below it, found by walking
    /// |ancestor|'s children: a Tree keeps no link from a node to its parent.
    /// The walk recurses once for each level of the tree, and a tree is never
    /// deeper than the nodes its storage holds (Outgoing::MAX_NODES).
    auto at_or_below(Tree const &tree, NodeId const ancestor, NodeId const node) -> bool {
        if (ancestor == node) {
            return true;
        }
        for (auto child = tree.first_child(ancestor); child.is_some();
             child = tree.next_sibling(*child)) {
            if (at_or_below(tree, *child, node)) {
                return true;
            }
        }
        return false;
    }

    /// A line being assembled in the room one debug_write takes, with no
    /// allocation: the pieces are copied in, and a piece that does not fit is
    /// cut. Written out in one call, so that the line cannot be split by
    /// another program printing at the same moment.
    class Line final {
      public:
        auto append(std::string_view const text) -> void {
            // One byte stays free for the newline.
            auto const room = m_text.size() - 1 - m_size;
            auto const count = std::min(text.size(), room);
            std::copy_n(text.data(), count, m_text.data() + m_size);
            m_size += count;
        }

        /// Ends the line and writes it to the debug console.
        [[nodiscard]]
        auto write() -> Status {
            m_text[m_size] = '\n';
            m_size += 1;
            return ours::debug_write(m_text.data(), m_size);
        }

      private:
        std::array<char, ours::MAX_DEBUG_WRITE> m_text{};
        usize m_size = 0;
    };
} // namespace

Outgoing::Outgoing(Storage &storage, loop::Loop &loop)
    : m_storage(&storage)
    , m_tree(storage.nodes)
    , m_server(m_tree, loop, Syscalls{.handle_duplicate = ours::handle_duplicate},
               LoopServer::Memory{
                   .connections = storage.connections,
                   .bindings = storage.bindings,
                   .request_bytes = storage.request_bytes,
                   .request_handles = storage.request_handles,
                   .reply = &storage.reply,
               },
               this)
    // The tree's first two nodes, which Storage sizes room for: neither add
    // can fail, and unwrap() traps if one ever does.
    , m_root(m_tree.add_root().unwrap())
    , m_svc(m_tree.add_directory(m_root, SVC_DIRECTORY).unwrap()) {}

auto Outgoing::add_protocol(std::string_view const name, Handler &handler) -> Status {
    if (m_handlers == MAX_PROTOCOLS) {
        return Status::NoResource;
    }
    // The service node's value is the handler's index, which connect() takes
    // back. The handler is stored only once the node is in, so a refused
    // name leaves nothing behind.
    auto const added = m_tree.add_service(m_svc, name, m_handlers);
    if (added.is_err()) {
        return added.unwrap_err();
    }
    m_storage->handlers[m_handlers] = &handler;
    m_handlers += 1;
    return Status::Ok;
}

auto Outgoing::is_declared(NodeId const node) const -> bool {
    auto const declared = std::span{m_storage->directories}.first(m_declared);
    return std::ranges::find(declared, node) != declared.end();
}

auto Outgoing::in_declared_tree(NodeId const node) const -> bool {
    auto const declared = std::span{m_storage->directories}.first(m_declared);
    return std::ranges::any_of(
        declared, [this, node](NodeId const root) { return at_or_below(m_tree, root, node); });
}

auto Outgoing::add_directory(std::string_view const path) -> ustl::Result<NodeId, Status> {
    // 1. The path's syntax, judged before the tree is looked at. The
    //    relative form is what the tree walks: it is a valid ours.io path
    //    but for ".", which is the directory itself and not a place to put
    //    one.
    if (!path.starts_with('/')) {
        return ustl::err(Status::InvArgs);
    }
    auto const relative = path.substr(1);
    if (relative == "." || !is_valid_path(relative)) {
        return ustl::err(Status::InvArgs);
    }
    // 2. svc/ is the protocols', by segment: "svcx" is a name of its own.
    if (auto rest = relative; take_segment(rest) == SVC_DIRECTORY) {
        return ustl::err(Status::InvArgs);
    }

    // 3. Where the path leaves the tree. Walked a segment at a time, so
    //    "overlaps" means sharing nodes, never sharing leading bytes. Every
    //    node outside svc/ is on the way to a declared directory, is one, or
    //    is below one, so a path that meets a declared directory is equal to
    //    it or below it, and one that is wholly there is above one.
    auto parent = m_root;
    auto missing = relative;
    while (!missing.empty()) {
        auto rest = missing;
        auto const found = m_tree.lookup(parent, take_segment(rest));
        if (found.is_err()) {
            if (found.unwrap_err() != Status::NotFound) {
                return ustl::err(found.unwrap_err());
            }
            break;
        }
        parent = found.unwrap();
        if (is_declared(parent)) {
            return ustl::err(Status::AlreadyExists);
        }
        missing = rest;
    }
    if (missing.empty()) {
        return ustl::err(Status::AlreadyExists);
    }

    // 4. Room for the whole of what is missing, so that no refusal leaves
    //    the first directories of the path behind.
    auto needed = usize{1};
    for (auto const byte: missing) {
        if (byte == '/') {
            needed += 1;
        }
    }
    if (m_tree.available() < needed) {
        return ustl::err(Status::NoResource);
    }

    // 5. Made. Nothing below can fail: the names are valid, none exists in
    //    its parent, the parents are directories and there is room, and
    //    unwrap() traps if one ever does. A declaration is a node, so the
    //    table of them has room for it too.
    while (!missing.empty()) {
        parent = m_tree.add_directory(parent, take_segment(missing)).unwrap();
    }
    m_storage->directories[m_declared] = parent;
    m_declared += 1;
    return ustl::ok(parent);
}

auto Outgoing::add_directory(NodeId const parent, std::string_view const name)
    -> ustl::Result<NodeId, Status> {
    return m_tree.add_directory(parent, name);
}

auto Outgoing::add_file(NodeId const parent, std::string_view const name, VmoFile const &file)
    -> ustl::Result<NodeId, Status> {
    return m_tree.add_file(parent, name, file);
}

auto Outgoing::add_service(NodeId const parent, std::string_view const name, Handler &handler)
    -> ustl::Result<NodeId, Status> {
    // 1. Only a directory capability's tree. The protocols' nodes are
    //    add_protocol()'s: a service node at the root or in svc/ would be
    //    one the program has not declared, or one add_protocol() did not make
    //    and could answer AlreadyExists for. A parent that is no node of the
    //    tree at all is refused here too, which is what Tree would say.
    if (!in_declared_tree(parent)) {
        return ustl::err(Status::InvArgs);
    }
    // 2. Room in the table of handlers, before the tree is touched: a node,
    //    once added, stays, and one whose value indexes past the table would
    //    be an Open that reaches no handler.
    if (m_handlers == MAX_PROTOCOLS) {
        return ustl::err(Status::NoResource);
    }
    // 3. The tree's own refusals. The value is the handler's index, which
    //    connect() takes back, and the handler is stored only once the node
    //    is in, so a refused name leaves nothing behind.
    auto const added = m_tree.add_service(parent, name, m_handlers);
    if (added.is_err()) {
        return added;
    }
    m_storage->handlers[m_handlers] = &handler;
    m_handlers += 1;
    return added;
}

auto Outgoing::serve(Uandle const directory_request) -> Status {
    return m_server.serve(directory_request, m_root, OUTGOING_RIGHTS);
}

auto Outgoing::connect(u64 const service, Uandle const channel) -> void {
    // An Open of svc/<name>, or of a service node in a directory
    // capability's tree: the node's handler owns the channel from here. The
    // value is an index add_protocol() or add_service() gave out, and the
    // tree holds no other service node; one past them would be a bug, and its
    // channel only closes.
    if (service >= m_handlers) {
        // Nothing can be done about a close that fails.
        auto _ = ours::handle_close(channel);
        return;
    }
    m_storage->handlers[service]->connect(channel);
}

auto Outgoing::open_refused(NodeId const directory, std::string_view const path,
                            Status const status) -> void {
    // Only the Opens the root writes, which arrive at the outgoing root or
    // at svc/. One through a connection of a declared directory is a
    // consumer's, which it reads in its own epitaph, and saying it here would
    // let any consumer with a connection fill the provider's console.
    if (directory != m_root && directory != m_svc) {
        return;
    }
    // Every refusal, not only a name that is not served: the others (a path
    // that is not valid, rights the connection does not hold, flags that do
    // not fit the node) are as silent to the provider, and the status says
    // which it was.
    //
    // The path is the client's own bytes, so it goes in through
    // printable_path(): a client cannot end the line and write another.
    std::array<char, MAX_SHOWN_PATH> shown{};
    auto line = Line{};
    line.append("ioservice: the outgoing directory refused an Open of \"");
    line.append(printable_path(path, shown));
    line.append("\": ");
    line.append(ours::to_string(status));
    // A console that does not take the line leaves nothing to do about it.
    auto _ = line.write();
}
} // namespace ioservice
