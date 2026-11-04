/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ioservice/server.hpp>

#include <algorithm>
#include <utility>
#include <variant>

namespace ioservice {
namespace {
    using ours::Rights;
    using ours::Status;
    using ours::Uandle;
    using prot::io::Flags;
    using prot::io::Operations;
    using prot::io::VmoFlags;

    /// Every right ours.io defines (Operations). The rights a request asks
    /// for are read straight out of its flags: the PERM_* bits share
    /// Operations' values, and every other bit is dropped, PERM_INHERIT_WRITE
    /// and PERM_INHERIT_EXECUTE included.
    constexpr auto ALL_OPERATIONS =
        Operations::Connect | Operations::ReadBytes | Operations::WriteBytes | Operations::Execute |
        Operations::GetAttributes | Operations::UpdateAttributes | Operations::Enumerate |
        Operations::Traverse | Operations::ModifyDirectory;

    /// The rights a file connection keeps, the ones the file protocol uses.
    /// A file opened with more, with PERM_READABLE's ENUMERATE and TRAVERSE
    /// say, gets these and no error: design D21's "shrink rights instead of
    /// refusing them".
    constexpr auto FILE_OPERATIONS = Operations::GetAttributes | Operations::ReadBytes |
                                     Operations::WriteBytes | Operations::UpdateAttributes |
                                     Operations::Execute;

    /// The flags that ask Open to create what it names. Nothing in an ioservice
    /// tree is created by a client (design non-goals: writes and every other
    /// mutation).
    constexpr auto CREATION_FLAGS =
        Flags::FlagMaybeCreate | Flags::FlagMustCreate | Flags::FlagCreateAsUnnamedTemporary;

    /// What else Open can ask for that the design cut: a node-only
    /// connection, and the OnRepresentation event, which nothing here sends.
    /// A client served without what it asked for would wait for an event
    /// that never comes, or speak the wrong protocol on the connection, so
    /// both are refused rather than ignored.
    constexpr auto CUT_FLAGS = Flags::ProtocolNode | Flags::FlagSendRepresentation;

    auto rights_of(Flags const flags) -> Operations {
        return static_cast<Operations>(std::to_underlying(flags) &
                                       std::to_underlying(ALL_OPERATIONS));
    }

    auto any_of(Flags const flags, Flags const bits) -> bool {
        return (flags & bits) != Flags::None;
    }

    auto any_of(VmoFlags const flags, VmoFlags const bits) -> bool {
        return (flags & bits) != VmoFlags::None;
    }

    auto holds(Operations const rights, Operations const bits) -> bool {
        return (rights & bits) == bits;
    }

    // Which node kinds the flags accept: a protocol bit names one, and no
    // protocol bit at all accepts whatever the node is.

    auto names_no_protocol(Flags const flags) -> bool {
        return !any_of(flags, prot::io::MASK_KNOWN_PROTOCOLS);
    }

    auto allows_directory(Flags const flags) -> bool {
        return any_of(flags, Flags::ProtocolDirectory) || names_no_protocol(flags);
    }

    auto allows_file(Flags const flags) -> bool {
        return any_of(flags, Flags::ProtocolFile) || names_no_protocol(flags);
    }

    auto allows_symlink(Flags const flags) -> bool {
        return any_of(flags, Flags::ProtocolSymlink) || names_no_protocol(flags);
    }

    auto dirent_type(NodeKind const kind) -> prot::io::DirentType {
        switch (kind) {
            case NodeKind::Directory:
                return prot::io::DirentType::Directory;
            case NodeKind::File:
                return prot::io::DirentType::File;
            case NodeKind::Service:
                return prot::io::DirentType::Service;
            case NodeKind::Remote:
                // A mount point stands in for the directory the remote
                // serves.
            case NodeKind::Subtree:
                // A client lists the program's own tree as a directory: it
                // is what an Open of the node makes.
            case NodeKind::Handler:
                // And so is a handler's place: the handler opens it as its
                // root, a directory.
                return prot::io::DirentType::Directory;
            case NodeKind::Removed:
                // A removed node is out of its directory's list, so no
                // listing walks to it; the value exists to keep the switch
                // total.
                return prot::io::DirentType::Unknown;
        }
        return prot::io::DirentType::Unknown;
    }

    /// The epitaph on a refused Open's or Clone's channel.
    auto refuse(Uandle const handle, Status const status, ReplyBuffer &out) -> Outcome {
        // Cannot fail: the buffer holds a whole message, and every status
        // passed here is one ours::Status names. unwrap() would trap if it
        // did.
        auto const epitaph = wire::encode_epitaph(out.bytes, status).unwrap();
        return Outcome{
            .action = Action::Refuse, .message = epitaph, .handle = handle, .status = status};
    }

    /// refuse() for an Open, which also says what the Open asked for.
    auto refuse_open(Uandle const handle, Status const status, std::string_view const path,
                     ReplyBuffer &out) -> Outcome {
        auto outcome = refuse(handle, status, out);
        outcome.path = ustl::some(path);
        return outcome;
    }

    /// An Outcome that writes |message| back on the connection.
    auto reply_of(ustl::Result<wire::Message, Status> const &encoded)
        -> ustl::Result<Outcome, Status> {
        if (encoded.is_err()) {
            return ustl::err(encoded.unwrap_err());
        }
        return ustl::ok(Outcome{.action = Action::Reply, .message = encoded.unwrap()});
    }

    /// Checks 6 and 7 of Open, which come after the walk has decided whose
    /// the target is: Ok, or the status the Open is refused with.
    auto refused_flags(Operations const held, Flags const flags) -> Status {
        // 6. Creating needs MODIFY_DIRECTORY; with it, the answer is still no,
        //    since a client creates nothing here.
        if (any_of(flags, CREATION_FLAGS)) {
            if (!holds(held, Operations::ModifyDirectory)) {
                return Status::AccessDenied;
            }
            return Status::NotSupported;
        }
        // 7. What the design cut.
        if (any_of(flags, CUT_FLAGS)) {
            return Status::NotSupported;
        }
        return Status::Ok;
    }

    /// Check 9 for a directory: the rights its connection keeps, or the
    /// status the Open is refused with.
    auto judge_directory(Flags const flags, Operations const requested)
        -> ustl::Result<Operations, Status> {
        // A directory keeps every right it was asked for: READ_BYTES and
        // EXECUTE on a directory are the ceilings of the files opened
        // through it.
        if (!allows_directory(flags)) {
            if (allows_file(flags)) {
                return ustl::err(Status::NotFile);
            }
            return ustl::err(Status::BadType);
        }
        if (any_of(flags, Flags::FileAppend | Flags::FileTruncate)) {
            return ustl::err(Status::InvArgs);
        }
        return ustl::ok(requested);
    }

    /// Check 9 for a file, up to what every file shares: the rights its
    /// connection keeps, or the status the Open is refused with. Whether the
    /// file can be executed is the file's own to say.
    auto judge_file(Flags const flags, Operations const requested)
        -> ustl::Result<Operations, Status> {
        // A file: only if the flags accept one.
        if (!allows_file(flags)) {
            if (allows_directory(flags) && !allows_symlink(flags)) {
                return ustl::err(Status::NotDir);
            }
            return ustl::err(Status::BadType);
        }
        if (any_of(flags, Flags::FileTruncate) && !any_of(flags, Flags::PermWriteBytes)) {
            return ustl::err(Status::InvArgs);
        }
        auto const rights = requested & FILE_OPERATIONS;
        // A right the file protocol uses but this file cannot honour is
        // refused, not shrunk. A file is readable and never writable.
        if (holds(rights, Operations::WriteBytes)) {
            return ustl::err(Status::AccessDenied);
        }
        return ustl::ok(rights);
    }

    /// The checks a GetBackingMemory's flags and its connection's rights
    /// decide: Ok, or the status the request is refused with.
    auto refused_vmo_flags(Operations const held, VmoFlags const flags) -> Status {
        // What the VMO may do is what the connection may do.
        if (any_of(flags, VmoFlags::PrivateClone) && any_of(flags, VmoFlags::SharedBuffer)) {
            return Status::InvArgs;
        }
        if ((any_of(flags, VmoFlags::Read) && !holds(held, Operations::ReadBytes)) ||
            (any_of(flags, VmoFlags::Write) && !holds(held, Operations::WriteBytes)) ||
            (any_of(flags, VmoFlags::Execute) && !holds(held, Operations::Execute))) {
            return Status::AccessDenied;
        }
        return Status::Ok;
    }

    /// What a handler answered, told apart: |on_done| gets the result of a
    /// Done, |on_pending| the Pending. Neither alternative can be absent, so
    /// there is no third case; get_if is used because a variant access that
    /// throws has nowhere to throw to here.
    template <typename R, typename OnDone, typename OnPending>
    auto answered(Answer<R> const &answer, OnDone &&on_done, OnPending &&on_pending)
        -> ustl::Result<Outcome, Status> {
        if (auto const *const pending = std::get_if<Pending>(&answer); pending != nullptr) {
            return on_pending(*pending);
        }
        return on_done(std::get_if<Done<R>>(&answer)->result);
    }

    /// The room the "." entry takes in a ReadDirents reply: a dirent with a
    /// one-byte name.
    constexpr auto DOT_SIZE = prot::io::DIRENT_HEADER_SIZE + 1;
} // namespace

Server::Server(Tree const &tree, std::span<Connection> const connections, Syscalls const syscalls,
               NodeHandler *const handler)
    : m_tree(&tree), m_connections(connections), m_syscalls(syscalls), m_handler(handler) {
    // Every slot starts free, whatever the storage held before.
    std::ranges::fill(m_connections, Connection{});
}

auto Server::find(ConnectionId const id) -> Connection * {
    if (id.index >= m_connections.size()) {
        return nullptr;
    }
    auto &slot = m_connections[id.index];
    if (slot.m_channel == Uandle::Invalid || slot.m_generation != id.generation) {
        return nullptr;
    }
    return &slot;
}

auto Server::find(ConnectionId const id) const -> Connection const * {
    if (id.index >= m_connections.size()) {
        return nullptr;
    }
    auto const &slot = m_connections[id.index];
    if (slot.m_channel == Uandle::Invalid || slot.m_generation != id.generation) {
        return nullptr;
    }
    return &slot;
}

auto Server::value_of(NodeId const node) const -> u64 {
    // Only a connection to a Handler node's object asks, and remove_service
    // takes services alone -- a handler node is never removed or changed, so
    // the value is there.
    return *m_tree->handler(node);
}

auto Server::allocate(Uandle const channel, NodeId const node, Operations const rights)
    -> ustl::Option<ConnectionId> {
    // A linear scan for a free slot; see docs/repairs/261004/ioservice.md.
    for (auto index = usize{0}; index < m_connections.size(); ++index) {
        auto &slot = m_connections[index];
        if (slot.m_channel != Uandle::Invalid) {
            continue;
        }
        // A fresh connection: "." first for a directory, offset 0 for a
        // file, no request pending, serial 0. Only the generation carries over
        // from the slot's last use.
        auto const generation = slot.m_generation;
        slot = Connection{};
        slot.m_channel = channel;
        slot.m_node = node;
        slot.m_rights = rights;
        slot.m_generation = generation;
        return ustl::some(ConnectionId{.index = static_cast<u32>(index), .generation = generation});
    }
    return ustl::NONE;
}

auto Server::free_slot(ConnectionId const id) -> void {
    auto *const slot = find(id);
    if (slot == nullptr) {
        return;
    }
    // A new generation, so that |id| and every copy of it go stale. It wraps
    // after 2^32 uses of one slot, which is far past any connection's life,
    // and skips 0 when it does, so that no live connection is ever {*, 0}
    // (see ConnectionId).
    auto next = id.generation + 1;
    if (next == 0) {
        next = FIRST_GENERATION;
    }
    *slot = Connection{};
    slot->m_generation = next;
}

auto Server::release(ConnectionId const id) -> Uandle {
    auto const *const slot = find(id);
    if (slot == nullptr) {
        return Uandle::Invalid;
    }
    // What the handler and the caller are owed, taken before the slot is
    // cleared: the handler is told when the connection is already gone, never
    // while it is half so. The connection is freed first and the handler told
    // after, once, because a stale id never reaches this point twice.
    auto const role = slot->m_role;
    auto const node = slot->m_node;
    auto const cookie = slot->m_cookie;
    auto const held = slot->m_request.held;
    free_slot(id);
    if (role != Connection::Role::Node) {
        m_handler->release(value_of(node), cookie);
    }
    return held;
}

auto Server::drop(ConnectionId const id) -> Outcome {
    // A pending Open's server end, if there was one, goes back to the caller
    // with the Drop: nothing else will ever answer it.
    return Outcome{.action = Action::Drop, .handle = release(id)};
}

auto Server::bind(Uandle const channel, NodeId const directory, Operations const rights)
    -> ustl::Result<ConnectionId, Status> {
    auto const kind = m_tree->kind(directory);
    if (channel == Uandle::Invalid || kind.is_none() ||
        (rights & ~ALL_OPERATIONS) != Operations::None) {
        return ustl::err(Status::InvArgs);
    }
    if (*kind != NodeKind::Directory) {
        return ustl::err(Status::NotDir);
    }
    auto const id = allocate(channel, directory, rights);
    if (id.is_none()) {
        return ustl::err(Status::NoResource);
    }
    return ustl::ok(*id);
}

auto Server::channel(ConnectionId const id) const -> ustl::Option<Uandle> {
    auto const *const slot = find(id);
    if (slot == nullptr) {
        return ustl::NONE;
    }
    return ustl::some(slot->m_channel);
}

auto Server::rights(ConnectionId const id) const -> ustl::Option<Operations> {
    auto const *const slot = find(id);
    if (slot == nullptr) {
        return ustl::NONE;
    }
    return ustl::some(slot->m_rights);
}

auto Server::node(ConnectionId const id) const -> ustl::Option<NodeId> {
    auto const *const slot = find(id);
    if (slot == nullptr) {
        return ustl::NONE;
    }
    return ustl::some(slot->m_node);
}

auto Server::open_connection(Uandle const channel, NodeId const node, Operations const rights)
    -> ustl::Result<Outcome, Status> {
    auto const id = allocate(channel, node, rights);
    if (id.is_none()) {
        return ustl::err(Status::NoResource);
    }
    return ustl::ok(Outcome{.action = Action::Bind, .handle = channel, .connection = *id});
}

auto Server::clone(Connection const &connection, Uandle const channel)
    -> ustl::Result<Outcome, Status> {
    if (connection.m_role == Connection::Role::Node) {
        // The same node with the same rights, from the start again.
        return open_connection(channel, connection.m_node, connection.m_rights);
    }
    // The slot first: the handler is asked for a cookie only for a connection
    // that will exist, so there is never a cookie to give back for want of
    // room.
    auto const id = allocate(channel, connection.m_node, connection.m_rights);
    if (id.is_none()) {
        return ustl::err(Status::NoResource);
    }
    auto const cookie = m_handler->clone(value_of(connection.m_node), connection.m_cookie);
    if (cookie.is_err()) {
        // Never announced, so the handler is not told it ended.
        free_slot(*id);
        return ustl::err(cookie.unwrap_err());
    }
    auto &slot = m_connections[id->index];
    slot.m_role = connection.m_role;
    slot.m_cookie = cookie.unwrap();
    return ustl::ok(Outcome{.action = Action::Bind, .handle = channel, .connection = *id});
}

auto Server::issue(ConnectionId const id, Connection &connection) -> Ticket {
    // Every request the handler is asked about gets a number of its own, the
    // ones it answers Done included, so that a ticket always names the
    // latest request and only while that one is pending.
    ++connection.m_serial;
    return Ticket{.connection = id, .serial = connection.m_serial};
}

auto Server::park(Connection &connection, Ticket const issued, Pending const answer,
                  Connection::Waiting const op, Connection::Request const request)
    -> ustl::Result<Outcome, Status> {
    if (answer.ticket != issued) {
        // The handler promised a request it was not given: nobody could end
        // the one it will wait for, so it is not waited for.
        return ustl::err(Status::InternalError);
    }
    connection.m_waiting = op;
    connection.m_request = request;
    return ustl::ok(Outcome{.action = Action::Pending});
}

auto Server::end_request(Connection &connection) -> Connection::Request {
    connection.m_waiting = Connection::Waiting::None;
    return std::exchange(connection.m_request, Connection::Request{});
}

auto Server::ask_open(ConnectionId const id, Connection &connection, NodeId const node,
                      ustl::Option<u64> const directory, std::string_view const path,
                      Flags const flags, Uandle const channel) -> ustl::Result<Outcome, Status> {
    if (m_handler == nullptr) {
        // A Handler node in a tree whose server was given no handler: a
        // program that cannot serve what it declared.
        return ustl::err(Status::NotSupported);
    }
    auto const ticket = issue(id, connection);
    auto const answer = m_handler->open(value_of(node), directory, path, flags, ticket);
    return answered<OpenResult>(
        answer, [&](OpenResult const &result) { return finish_open(node, flags, channel, result); },
        [&](Pending const &pending) {
            // The server end is held until the Open is answered, and the
            // flags kept to judge what the handler opens.
            return park(connection, ticket, pending, Connection::Waiting::Open,
                        Connection::Request{
                            .argument = std::to_underlying(flags), .node = node, .held = channel});
        });
}

auto Server::finish_open(NodeId const node, Flags const flags, Uandle const channel,
                         OpenResult const &result) -> ustl::Result<Outcome, Status> {
    if (result.is_err()) {
        return ustl::err(result.unwrap_err());
    }
    auto const opened = result.unwrap();
    auto const requested = rights_of(flags);

    // 9. The node's own rules, on the kind the handler reports.
    auto judged = ustl::Result<Operations, Status>{ustl::ok(Operations::None)};
    auto role = Connection::Role::HandledFile;
    if (opened.kind == ObjectKind::Directory) {
        judged = judge_directory(flags, requested);
        role = Connection::Role::HandledDirectory;
    } else {
        judged = judge_file(flags, requested);
    }
    if (judged.is_err()) {
        // The handler made the object, so it is told it is not wanted.
        m_handler->release(value_of(node), opened.cookie);
        return ustl::err(judged.unwrap_err());
    }
    auto const id = allocate(channel, node, judged.unwrap());
    if (id.is_none()) {
        m_handler->release(value_of(node), opened.cookie);
        return ustl::err(Status::NoResource);
    }
    auto &slot = m_connections[id->index];
    slot.m_role = role;
    slot.m_cookie = opened.cookie;
    return ustl::ok(Outcome{.action = Action::Bind, .handle = channel, .connection = *id});
}

auto Server::open(ConnectionId const id, Connection &connection,
                  prot::io::OpenRequest const &request, ReplyBuffer &out)
    -> ustl::Result<Outcome, Status> {
    auto const flags = request.flags;
    auto const requested = rights_of(flags);

    // The connection's checks, in a fixed order. The order is part of the
    // protocol: it decides which error a request that is wrong in several
    // ways gets, and so what a client can learn. A connection without
    // TRAVERSE answers AccessDenied for every name, whether or not it
    // exists, before the tree is looked at.

    // 1. The path's syntax (design D21).
    if (!is_valid_path(request.path)) {
        return ustl::err(Status::BadPath);
    }
    // 2. Going below the directory needs TRAVERSE; "." stays where it is.
    if (request.path != "." && !holds(connection.m_rights, Operations::Traverse)) {
        return ustl::err(Status::AccessDenied);
    }
    // 3. A connection never grows: what it opens holds a subset of its
    //    rights. This is what makes a namespace entry's rights a ceiling
    //    for everything opened through it.
    if ((requested & ~connection.m_rights) != Operations::None) {
        return ustl::err(Status::AccessDenied);
    }
    // 4. Connecting to a service needs CONNECT.
    if (any_of(flags, Flags::ProtocolService) && !holds(connection.m_rights, Operations::Connect)) {
        return ustl::err(Status::AccessDenied);
    }
    // 5. GET_ATTRIBUTES, when Open asks for attributes. It asks in its
    //    options table, which the subset cuts (design non-goals), so there
    //    is nothing to check.

    if (connection.m_role == Connection::Role::HandledDirectory) {
        // The connection is to a directory the handler opened, so what the
        // path names is in the handler's tree and not this one's: there is no
        // walk here (8), and a name that is not there is the handler's to
        // refuse. 6 and 7 are the connection's, as ever, and run before the
        // handler is asked.
        if (auto const status = refused_flags(connection.m_rights, flags); status != Status::Ok) {
            return ustl::err(status);
        }
        return ask_open(id, connection, connection.m_node, ustl::some(connection.m_cookie),
                        request.path, flags, request.object);
    }

    // The walk runs before 6 and 7, because whether the target is this
    // tree's decides whether they are this server's to judge at all: an Open
    // that reaches a remote entry is served by the server that owns it,
    // creation flags and cut flags included. A local target never sees the
    // difference: below, 6 and 7 still refuse before the walk's own answer,
    // so the order a client observes for everything this tree serves is the
    // order the design fixed. The walk stops at a remote entry, a Subtree
    // node or a Handler node, whichever it meets first.
    auto const found = m_tree->resolve(connection.m_node, request.path);
    if (found.is_ok() && *m_tree->kind(found->node) == NodeKind::Remote) {
        // A mount point: what is left of the path, "." when the path named
        // the entry itself, is the remote server's to walk. The flags pass
        // through unchanged -- the rights among them already held to a
        // subset of this connection's by check 3 -- and the remote judges
        // the rest its own way.
        auto const forwarded =
            prot::io::encode_open(out.bytes, out.handles,
                                  {.flags = flags, .path = found->rest, .object = request.object});
        if (forwarded.is_err()) {
            // Neither refusal reason can arise -- the path passed check 1 and
            // the buffer holds a whole message -- but a failed encoding
            // reaches the client as a refused Open rather than as nonsense.
            return ustl::err(forwarded.unwrap_err());
        }
        return ustl::ok(Outcome{.action = Action::Forward,
                                .message = forwarded.unwrap(),
                                .forward_channel = *m_tree->remote(found->node)});
    }

    // 6 and 7: creating, and what the design cut.
    if (auto const status = refused_flags(connection.m_rights, flags); status != Status::Ok) {
        return ustl::err(status);
    }

    // 8. The walk's own answer, which 6 and 7 have had their turn ahead of.
    if (found.is_err()) {
        return ustl::err(found.unwrap_err());
    }
    auto const node = found->node;

    // What is below a Subtree node is the program's tree, so the Open goes to
    // the program, whole. Checks 1 to 8 were the connection's and passed,
    // and are all this server holds the Open to. The flags go on unchanged:
    // they ask for no more than the connection holds (check 3), which is what
    // keeps rights shrinking below the node. No connection is made for the
    // channel, which the program owns from here.
    auto const kind = *m_tree->kind(node);
    if (kind == NodeKind::Subtree) {
        return ustl::ok(Outcome{
            .action = Action::Subtree,
            .handle = request.object,
            .service = *m_tree->subtree(node),
            .rest = found->rest,
            .flags = flags,
        });
    }

    // What is below a Handler node is the handler's, and the protocol on what
    // it opens is this server's: checks 1 to 8 passed on this connection, and
    // check 9 is made on the kind the handler reports.
    if (kind == NodeKind::Handler) {
        return ask_open(id, connection, node, ustl::NONE, found->rest, flags, request.object);
    }

    // 9. The node's own rules.
    if (kind == NodeKind::Directory) {
        auto const judged = judge_directory(flags, requested);
        if (judged.is_err()) {
            return ustl::err(judged.unwrap_err());
        }
        return open_connection(request.object, node, judged.unwrap());
    }

    if (kind == NodeKind::File) {
        auto const judged = judge_file(flags, requested);
        if (judged.is_err()) {
            return ustl::err(judged.unwrap_err());
        }
        auto const rights = judged.unwrap();
        // A file connection holds EXECUTE only if it passed here, and
        // GetBackingMemory hands out the Execute right only to such a
        // connection. A VmoFile is executable only when it was added so.
        if (holds(rights, Operations::Execute) && !m_tree->file(node)->executable) {
            return ustl::err(Status::AccessDenied);
        }
        return open_connection(request.object, node, rights);
    }

    // A service. It takes no rights and no other flag; the channel stops
    // being ours.io here.
    if (any_of(flags & prot::io::MASK_KNOWN_PROTOCOLS, ~Flags::ProtocolService)) {
        if (allows_directory(flags)) {
            return ustl::err(Status::NotDir);
        }
        if (allows_file(flags)) {
            return ustl::err(Status::NotFile);
        }
        return ustl::err(Status::BadType);
    }
    if (any_of(flags, ~Flags::ProtocolService)) {
        return ustl::err(Status::InvArgs);
    }
    // Check 4 looks at CONNECT only when the flags name PROTOCOL_SERVICE, so
    // a request with no flags at all would reach a service from a connection
    // without CONNECT. Here CONNECT is what connecting to a service takes,
    // however the request is spelled.
    if (!holds(connection.m_rights, Operations::Connect)) {
        return ustl::err(Status::AccessDenied);
    }
    return ustl::ok(Outcome{
        .action = Action::Connect,
        .handle = request.object,
        .service = *m_tree->service(node),
    });
}

auto Server::read_dirents(Connection &connection, prot::io::ReadDirentsRequest const &request,
                          ReplyBuffer &out) -> ustl::Result<Outcome, Status> {
    if (!holds(connection.m_rights, Operations::Enumerate)) {
        return ustl::err(Status::AccessDenied);
    }
    // More than a reply can carry: OutOfRange, as Read answers for its own
    // limit.
    if (request.max_bytes > prot::io::MAX_BUF) {
        return ustl::err(Status::OutOfRange);
    }

    // The entries are packed straight into the reply, where the encoder
    // leaves them: no copy.
    auto writer =
        prot::io::DirentWriter(prot::io::reply_payload(out.bytes).first(request.max_bytes));
    if (connection.m_phase == Connection::DirentPhase::Dot) {
        auto const dot = prot::io::Dirent{
            .ino = prot::io::INO_UNKNOWN,
            .type = prot::io::DirentType::Directory,
            .name = ".",
        };
        if (writer.append(dot) != Status::Ok) {
            return ustl::err(Status::BufferTooSmall);
        }
        connection.m_phase = Connection::DirentPhase::Entries;
        connection.m_next_entry = m_tree->first_child(connection.m_node);
    }
    while (connection.m_next_entry.is_some()) {
        auto const entry = *connection.m_next_entry;
        auto const dirent = prot::io::Dirent{
            .ino = prot::io::INO_UNKNOWN,
            .type = dirent_type(*m_tree->kind(entry)),
            .name = m_tree->name(entry),
        };
        if (writer.append(dirent) != Status::Ok) {
            // The first entry of a reply that does not fit is BufferTooSmall,
            // with the cursor left where it is; a later one ends the reply,
            // and is sent first next time.
            if (writer.written().empty()) {
                return ustl::err(Status::BufferTooSmall);
            }
            break;
        }
        connection.m_next_entry = m_tree->next_sibling(entry);
    }
    return reply_of(prot::io::encode_read_dirents_reply(
        out.bytes, request.txid, {.status = Status::Ok, .dirents = writer.written()}));
}

auto Server::read(Connection &connection, prot::io::ReadRequest const &request, ReplyBuffer &out)
    -> ustl::Result<Outcome, Status> {
    if (!holds(connection.m_rights, Operations::ReadBytes)) {
        return ustl::err(Status::BadHandle);
    }
    if (request.count > prot::io::MAX_TRANSFER_SIZE) {
        return ustl::err(Status::OutOfRange);
    }
    // Copied from the program's own view of the file into the reply: the
    // one copy in the server that design D17 counts. Past the end is 0
    // bytes, which is how end of file reads.
    auto const bytes = m_tree->file(connection.m_node)->bytes;
    auto const offset = std::min<u64>(connection.m_seek, bytes.size());
    auto const size = std::min<u64>(request.count, bytes.size() - offset);
    auto const payload = prot::io::reply_payload(out.bytes).first(size);
    std::ranges::copy(bytes.subspan(offset, size), payload.begin());
    connection.m_seek = offset + size;
    return reply_of(prot::io::encode_read_reply(out.bytes, request.txid,
                                                {.status = Status::Ok, .data = payload}));
}

auto Server::get_backing_memory(Connection const &connection,
                                prot::io::GetBackingMemoryRequest const &request, ReplyBuffer &out)
    -> ustl::Result<Outcome, Status> {
    auto const flags = request.flags;
    if (auto const status = refused_vmo_flags(connection.m_rights, flags); status != Status::Ok) {
        return ustl::err(status);
    }

    // Basic, Map and GetProperty always, plus the rights the flags name. Write
    // never gets this far, since no file connection holds WRITE_BYTES. Basic
    // holds Duplicate, which launch::start() needs beside Read, Map and
    // Execute.
    //
    // PRIVATE_CLONE gets the same duplicate (design D21), not a snapshot:
    // a snapshot drops Execute (system/public/ours/syscalls.gni:851-852),
    // which would break fsio's spawn, which asks READ|EXECUTE|PRIVATE_CLONE.
    // And a private copy of a VMO nobody can write is the VMO itself.
    auto rights = Rights::Basic | Rights::Map | Rights::GetProperty;
    if (any_of(flags, VmoFlags::Read)) {
        rights |= Rights::Read;
    }
    if (any_of(flags, VmoFlags::Execute)) {
        rights |= Rights::Execute;
    }
    if (m_syscalls.handle_duplicate == nullptr) {
        return ustl::err(Status::NotSupported);
    }
    auto vmo = Uandle::Invalid;
    if (auto const status =
            m_syscalls.handle_duplicate(m_tree->file(connection.m_node)->vmo, rights, &vmo);
        status != Status::Ok) {
        return ustl::err(status);
    }
    // Cannot fail with a valid handle: the txid passed the decoder and |out|
    // holds a whole message and a handle. Were it to, the duplicate would
    // stay in this process, since nothing here can close it.
    return reply_of(prot::io::encode_get_backing_memory_reply(out.bytes, out.handles, request.txid,
                                                              {.status = Status::Ok, .vmo = vmo}));
}

auto Server::finish_read(Connection &connection, u32 const txid, u64 const count,
                         ReadResult const &result, ReplyBuffer &out)
    -> ustl::Result<Outcome, Status> {
    if (result.is_err()) {
        return ustl::err(result.unwrap_err());
    }
    auto const bytes = result.unwrap();
    // The handler may return fewer bytes than asked, never more: the request
    // was held to a reply's room before the handler saw it.
    if (bytes.size() > count) {
        return ustl::err(Status::InternalError);
    }
    // Copied from the handler's bytes into the reply: the one copy a Read
    // makes here, as for a file of the tree.
    auto const payload = prot::io::reply_payload(out.bytes).first(bytes.size());
    std::ranges::copy(bytes, payload.begin());
    auto reply = reply_of(
        prot::io::encode_read_reply(out.bytes, txid, {.status = Status::Ok, .data = payload}));
    if (reply.is_ok()) {
        // The offset moves by what the reply carries, once there is a reply,
        // and not before: a Read that is still pending, or that failed,
        // moves nothing.
        connection.m_seek += bytes.size();
    }
    return reply;
}

auto Server::finish_read_dirents(Connection &connection, u32 const txid, u64 const max_bytes,
                                 EntriesResult const &result, ReplyBuffer &out)
    -> ustl::Result<Outcome, Status> {
    // The "." entry is the first thing a directory lists and is the Server's,
    // not the handler's: it goes first, in the room read_dirents_handled() set
    // aside for it.
    auto const dot = connection.m_phase == Connection::DirentPhase::Dot;
    auto room = max_bytes;
    if (dot) {
        room -= DOT_SIZE;
    }

    auto entries = Entries{.dirents = {}, .next = connection.m_seek};
    if (result.is_err()) {
        // The first entry after the "." does not fit: the reply is the "."
        // alone, the cursor stays and the entry is sent first next time, as
        // for a tree. Without a "." to send, the client is told.
        if (!dot || result.unwrap_err() != Status::BufferTooSmall) {
            return ustl::err(result.unwrap_err());
        }
    } else {
        entries = result.unwrap();
        if (entries.dirents.size() > room) {
            return ustl::err(Status::InternalError);
        }
    }

    auto const payload = prot::io::reply_payload(out.bytes).first(max_bytes);
    auto writer = prot::io::DirentWriter(payload);
    if (dot) {
        auto const dot_entry = prot::io::Dirent{
            .ino = prot::io::INO_UNKNOWN,
            .type = prot::io::DirentType::Directory,
            .name = ".",
        };
        // Room for it was set aside, so it fits.
        if (writer.append(dot_entry) != Status::Ok) {
            return ustl::err(Status::InternalError);
        }
    }
    auto const used = writer.written().size();
    std::ranges::copy(entries.dirents, payload.begin() + static_cast<std::ptrdiff_t>(used));
    auto reply = reply_of(prot::io::encode_read_dirents_reply(
        out.bytes, txid,
        {.status = Status::Ok, .dirents = payload.first(used + entries.dirents.size())}));
    if (reply.is_ok()) {
        connection.m_phase = Connection::DirentPhase::Entries;
        connection.m_seek = entries.next;
    }
    return reply;
}

auto Server::finish_backing_memory(u32 const txid, VmoResult const &result, ReplyBuffer &out)
    -> ustl::Result<Outcome, Status> {
    if (result.is_err()) {
        return ustl::err(result.unwrap_err());
    }
    // The handle goes into the reply and from there to the client; the reply
    // refuses an invalid one, which would be the handler's bug.
    auto reply = reply_of(prot::io::encode_get_backing_memory_reply(
        out.bytes, out.handles, txid, {.status = Status::Ok, .vmo = result.unwrap()}));
    if (reply.is_err()) {
        return ustl::err(Status::InternalError);
    }
    return reply;
}

auto Server::read_handled(ConnectionId const id, Connection &connection,
                          prot::io::ReadRequest const &request, ReplyBuffer &out)
    -> ustl::Result<Outcome, Status> {
    // The protocol's checks, before the handler is asked.
    if (!holds(connection.m_rights, Operations::ReadBytes)) {
        return ustl::err(Status::BadHandle);
    }
    if (request.count > prot::io::MAX_TRANSFER_SIZE) {
        return ustl::err(Status::OutOfRange);
    }
    auto const ticket = issue(id, connection);
    auto const answer = m_handler->read(value_of(connection.m_node), connection.m_cookie,
                                        connection.m_seek, request.count, ticket);
    return answered<ReadResult>(
        answer,
        [&](ReadResult const &result) {
            return finish_read(connection, request.txid, request.count, result, out);
        },
        [&](Pending const &pending) {
            return park(connection, ticket, pending, Connection::Waiting::Read,
                        Connection::Request{.argument = request.count, .txid = request.txid});
        });
}

auto Server::read_dirents_handled(ConnectionId const id, Connection &connection,
                                  prot::io::ReadDirentsRequest const &request, ReplyBuffer &out)
    -> ustl::Result<Outcome, Status> {
    if (!holds(connection.m_rights, Operations::Enumerate)) {
        return ustl::err(Status::AccessDenied);
    }
    if (request.max_bytes > prot::io::MAX_BUF) {
        return ustl::err(Status::OutOfRange);
    }
    // Room for the "." entry is set aside before the handler is asked, so the
    // handler is given what is left and never has to know about it.
    auto room = request.max_bytes;
    if (connection.m_phase == Connection::DirentPhase::Dot) {
        if (room < DOT_SIZE) {
            return ustl::err(Status::BufferTooSmall);
        }
        room -= DOT_SIZE;
    }
    auto const ticket = issue(id, connection);
    auto const answer = m_handler->read_dirents(value_of(connection.m_node), connection.m_cookie,
                                                connection.m_seek, room, ticket);
    return answered<EntriesResult>(
        answer,
        [&](EntriesResult const &result) {
            return finish_read_dirents(connection, request.txid, request.max_bytes, result, out);
        },
        [&](Pending const &pending) {
            return park(connection, ticket, pending, Connection::Waiting::ReadDirents,
                        Connection::Request{.argument = request.max_bytes, .txid = request.txid});
        });
}

auto Server::get_backing_memory_handled(ConnectionId const id, Connection &connection,
                                        prot::io::GetBackingMemoryRequest const &request,
                                        ReplyBuffer &out) -> ustl::Result<Outcome, Status> {
    if (auto const status = refused_vmo_flags(connection.m_rights, request.flags);
        status != Status::Ok) {
        return ustl::err(status);
    }
    auto const ticket = issue(id, connection);
    auto const answer = m_handler->get_backing_memory(value_of(connection.m_node),
                                                      connection.m_cookie, request.flags, ticket);
    return answered<VmoResult>(
        answer,
        [&](VmoResult const &result) { return finish_backing_memory(request.txid, result, out); },
        [&](Pending const &pending) {
            return park(connection, ticket, pending, Connection::Waiting::GetBackingMemory,
                        Connection::Request{.txid = request.txid});
        });
}

auto Server::claim(Ticket const ticket, Connection::Waiting const op)
    -> ustl::Result<Connection *, Status> {
    auto *const connection = find(ticket.connection);
    // The ticket names the latest request of a connection that is alive and
    // still waiting for it. A released connection fails the generation of
    // find(); a spent ticket finds nothing pending; an earlier request's
    // finds a later one with another serial.
    if (connection == nullptr || connection->m_waiting == Connection::Waiting::None ||
        connection->m_serial != ticket.serial) {
        return ustl::err(Status::NotFound);
    }
    if (connection->m_waiting != op) {
        return ustl::err(Status::InvArgs);
    }
    return ustl::ok(connection);
}

auto Server::answer(ConnectionId const id, wire::Method const &method, u32 const txid,
                    ustl::Result<Outcome, Status> const &handled, ReplyBuffer &out) -> Outcome {
    // A two-way method's answer: its Ok reply as it was built, or the status
    // form of its failure.
    if (handled.is_ok()) {
        return handled.unwrap();
    }
    auto const encoded = wire::encode_status_reply(out.bytes, method, txid, handled.unwrap_err());
    if (encoded.is_err()) {
        return drop(id);
    }
    return Outcome{.action = Action::Reply, .message = encoded.unwrap()};
}

auto Server::complete(Ticket const ticket, OpenResult const &result, ReplyBuffer &out)
    -> ustl::Result<Outcome, Status> {
    auto const claimed = claim(ticket, Connection::Waiting::Open);
    if (claimed.is_err()) {
        return ustl::err(claimed.unwrap_err());
    }
    auto &connection = *claimed.unwrap();
    // The request is ended from here on, whatever the Server makes of the
    // handler's result.
    auto const request = end_request(connection);
    auto const finished =
        finish_open(request.node, static_cast<Flags>(request.argument), request.held, result);
    if (finished.is_ok()) {
        return ustl::ok(finished.unwrap());
    }
    // Answered the way any other refused Open is: an epitaph on the server end
    // the Server held. No path: the Open's bytes are gone, so a refusal that
    // came late is not reported to the delegate.
    return ustl::ok(refuse(request.held, finished.unwrap_err(), out));
}

auto Server::complete(Ticket const ticket, ReadResult const &result, ReplyBuffer &out)
    -> ustl::Result<Outcome, Status> {
    auto const claimed = claim(ticket, Connection::Waiting::Read);
    if (claimed.is_err()) {
        return ustl::err(claimed.unwrap_err());
    }
    auto &connection = *claimed.unwrap();
    auto const request = end_request(connection);
    return ustl::ok(answer(ticket.connection, prot::io::READ, request.txid,
                           finish_read(connection, request.txid, request.argument, result, out),
                           out));
}

auto Server::complete(Ticket const ticket, EntriesResult const &result, ReplyBuffer &out)
    -> ustl::Result<Outcome, Status> {
    auto const claimed = claim(ticket, Connection::Waiting::ReadDirents);
    if (claimed.is_err()) {
        return ustl::err(claimed.unwrap_err());
    }
    auto &connection = *claimed.unwrap();
    auto const request = end_request(connection);
    return ustl::ok(
        answer(ticket.connection, prot::io::READ_DIRENTS, request.txid,
               finish_read_dirents(connection, request.txid, request.argument, result, out), out));
}

auto Server::complete(Ticket const ticket, VmoResult const &result, ReplyBuffer &out)
    -> ustl::Result<Outcome, Status> {
    auto const claimed = claim(ticket, Connection::Waiting::GetBackingMemory);
    if (claimed.is_err()) {
        return ustl::err(claimed.unwrap_err());
    }
    auto &connection = *claimed.unwrap();
    auto const request = end_request(connection);
    return ustl::ok(answer(ticket.connection, prot::io::GET_BACKING_MEMORY, request.txid,
                           finish_backing_memory(request.txid, result, out), out));
}

auto Server::dispatch(ConnectionId const id, wire::Message const message, ReplyBuffer &out)
    -> Outcome {
    auto *const connection = find(id);
    if (connection == nullptr) {
        return Outcome{.action = Action::Drop};
    }
    // A connection with a request the handler has not answered is not read
    // until the answer is written, so a message for it is a program that did
    // not wait: the connection goes, as one that sent a malformed message
    // does, and the Open it held with it.
    if (connection->m_waiting != Connection::Waiting::None) {
        return drop(id);
    }
    // The direction byte first: a message going to a client is never a
    // request, whatever its ordinal (design I5).
    auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
    if (header.is_err()) {
        return drop(id);
    }
    auto const ordinal = header->ordinal;

    // Node: every connection.
    if (ordinal == prot::io::CLOSE.ordinal) {
        auto const request = prot::io::decode_close(message);
        if (request.is_err()) {
            return drop(id);
        }
        auto const reply =
            prot::io::encode_close_reply(out.bytes, request->txid, {.status = Status::Ok});
        // Nothing is pending here, so there is no server end to give back.
        release(id);
        if (reply.is_err()) {
            return Outcome{.action = Action::Drop};
        }
        return Outcome{.action = Action::ReplyAndClose, .message = reply.unwrap()};
    }
    if (ordinal == prot::io::CLONE.ordinal) {
        auto const request = prot::io::decode_clone(message);
        if (request.is_err()) {
            return drop(id);
        }
        auto cloned = clone(*connection, request->request);
        if (cloned.is_err()) {
            return refuse(request->request, cloned.unwrap_err(), out);
        }
        return cloned.unwrap();
    }

    auto const handled = connection->m_role != Connection::Role::Node;
    auto is_directory = connection->m_role == Connection::Role::HandledDirectory;
    if (!handled) {
        auto const kind = m_tree->kind(connection->m_node);
        // A removed node (or any id that stopped naming a node of this
        // tree): the connection has nothing left to serve.
        if (kind.is_none()) {
            return drop(id);
        }
        is_directory = *kind == NodeKind::Directory;
    }

    if (is_directory) {
        if (ordinal == prot::io::OPEN.ordinal) {
            auto const request = prot::io::decode_open(message);
            if (request.is_err()) {
                return drop(id);
            }
            auto opened = open(id, *connection, request.unwrap(), out);
            if (opened.is_err()) {
                return refuse_open(request->object, opened.unwrap_err(), request->path, out);
            }
            return opened.unwrap();
        }
        if (ordinal == prot::io::READ_DIRENTS.ordinal) {
            auto const request = prot::io::decode_read_dirents(message);
            if (request.is_err()) {
                return drop(id);
            }
            if (handled) {
                return answer(id, prot::io::READ_DIRENTS, request->txid,
                              read_dirents_handled(id, *connection, request.unwrap(), out), out);
            }
            return answer(id, prot::io::READ_DIRENTS, request->txid,
                          read_dirents(*connection, request.unwrap(), out), out);
        }
    } else {
        if (ordinal == prot::io::READ.ordinal) {
            auto const request = prot::io::decode_read(message);
            if (request.is_err()) {
                return drop(id);
            }
            if (handled) {
                return answer(id, prot::io::READ, request->txid,
                              read_handled(id, *connection, request.unwrap(), out), out);
            }
            return answer(id, prot::io::READ, request->txid,
                          read(*connection, request.unwrap(), out), out);
        }
        if (ordinal == prot::io::GET_BACKING_MEMORY.ordinal) {
            auto const request = prot::io::decode_get_backing_memory(message);
            if (request.is_err()) {
                return drop(id);
            }
            if (handled) {
                return answer(id, prot::io::GET_BACKING_MEMORY, request->txid,
                              get_backing_memory_handled(id, *connection, request.unwrap(), out),
                              out);
            }
            return answer(id, prot::io::GET_BACKING_MEMORY, request->txid,
                          get_backing_memory(*connection, request.unwrap(), out), out);
        }
    }

    // A method this connection does not serve: a File method on a
    // directory, say, or an ordinal nobody defines. The header has no bit
    // that would mark a call as one an older server may answer "unknown" to
    // (byte 4 is the direction), so every unknown call closes the channel.
    return drop(id);
}
} // namespace ioservice
