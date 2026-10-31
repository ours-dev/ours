/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ioservice/server.hpp>

#include <algorithm>
#include <utility>

namespace ioservice {
namespace {
    using ours::Rights;
    using ours::Status;
    using ours::Uandle;
    using ours_io::Flags;
    using ours_io::Operations;
    using ours_io::VmoFlags;

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
        return !any_of(flags, ours_io::MASK_KNOWN_PROTOCOLS);
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

    auto dirent_type(NodeKind const kind) -> ours_io::DirentType {
        switch (kind) {
            case NodeKind::Directory:
                return ours_io::DirentType::Directory;
            case NodeKind::File:
                return ours_io::DirentType::File;
            case NodeKind::Service:
                return ours_io::DirentType::Service;
            case NodeKind::Remote:
                // A mount point stands in for the directory the remote
                // serves.
                return ours_io::DirentType::Directory;
        }
        return ours_io::DirentType::Unknown;
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
} // namespace

Server::Server(Tree const &tree, std::span<Connection> const connections, Syscalls const syscalls)
    : m_tree(&tree), m_connections(connections), m_syscalls(syscalls) {
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

auto Server::allocate(Uandle const channel, NodeId const node, Operations const rights)
    -> ustl::Option<ConnectionId> {
    // A linear scan for a free slot; see docs/repairs/261004/ioservice.md.
    for (auto index = usize{0}; index < m_connections.size(); ++index) {
        auto &slot = m_connections[index];
        if (slot.m_channel != Uandle::Invalid) {
            continue;
        }
        // A fresh connection: "." first for a directory, offset 0 for a
        // file. Only the generation carries over from the slot's last use.
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

auto Server::release(ConnectionId const id) -> void {
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

auto Server::drop(ConnectionId const id) -> Outcome {
    release(id);
    return Outcome{.action = Action::Drop};
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

auto Server::open(Connection const &connection, ours_io::OpenRequest const &request,
                  ReplyBuffer &out) -> ustl::Result<Outcome, Status> {
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

    // The walk runs before 6 and 7, because whether the target is this
    // tree's decides whether they are this server's to judge at all: an Open
    // that reaches a remote entry is served by the server that owns it,
    // creation flags and cut flags included. A local target never sees the
    // difference: below, 6 and 7 still refuse before the walk's own answer,
    // so the order a client observes for everything this tree serves is the
    // order the design fixed.
    auto const walked = m_tree->walk(connection.m_node, request.path);
    if (walked.is_ok() && *m_tree->kind(walked->node) == NodeKind::Remote) {
        // A mount point: what is left of the path, "." when the path named
        // the entry itself, is the remote server's to walk. The flags pass
        // through unchanged -- the rights among them already held to a
        // subset of this connection's by check 3 -- and the remote judges
        // the rest its own way.
        auto path = walked->remainder;
        if (path.empty()) {
            path = ".";
        }
        auto const forwarded = ours_io::encode_open(
            out.bytes, out.handles, {.flags = flags, .path = path, .object = request.object});
        if (forwarded.is_err()) {
            // Neither refusal reason can arise -- the path passed check 1 and
            // the buffer holds a whole message -- but a failed encoding
            // reaches the client as a refused Open rather than as nonsense.
            return ustl::err(forwarded.unwrap_err());
        }
        return ustl::ok(Outcome{.action = Action::Forward,
                                .message = forwarded.unwrap(),
                                .forward_channel = *m_tree->remote(walked->node)});
    }

    // 6. Creating needs MODIFY_DIRECTORY; with it, the answer is still no,
    //    since a client creates nothing here.
    if (any_of(flags, CREATION_FLAGS)) {
        if (!holds(connection.m_rights, Operations::ModifyDirectory)) {
            return ustl::err(Status::AccessDenied);
        }
        return ustl::err(Status::NotSupported);
    }
    // 7. What the design cut.
    if (any_of(flags, CUT_FLAGS)) {
        return ustl::err(Status::NotSupported);
    }

    // 8. The walk's own answer, which 6 and 7 have had their turn ahead of.
    if (walked.is_err()) {
        return ustl::err(walked.unwrap_err());
    }
    auto const node = walked->node;

    // 9. The node's own rules.
    auto const kind = *m_tree->kind(node);
    if (kind == NodeKind::Directory) {
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
        return open_connection(request.object, node, requested);
    }

    if (kind == NodeKind::File) {
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
        // refused, not shrunk. A VmoFile is readable and never writable, and
        // executable only when it was added so. This is the EXECUTE gate: a
        // file connection holds EXECUTE only if it passed here, and
        // GetBackingMemory hands out the Execute right only to such a
        // connection.
        if (holds(rights, Operations::WriteBytes)) {
            return ustl::err(Status::AccessDenied);
        }
        if (holds(rights, Operations::Execute) && !m_tree->file(node)->executable) {
            return ustl::err(Status::AccessDenied);
        }
        return open_connection(request.object, node, rights);
    }

    // A service. It takes no rights and no other flag; the channel stops
    // being ours.io here.
    if (any_of(flags & ours_io::MASK_KNOWN_PROTOCOLS, ~Flags::ProtocolService)) {
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

auto Server::read_dirents(Connection &connection, ours_io::ReadDirentsRequest const &request,
                          ReplyBuffer &out) -> ustl::Result<wire::Message, Status> {
    if (!holds(connection.m_rights, Operations::Enumerate)) {
        return ustl::err(Status::AccessDenied);
    }
    // More than a reply can carry: OutOfRange, as Read answers for its own
    // limit.
    if (request.max_bytes > ours_io::MAX_BUF) {
        return ustl::err(Status::OutOfRange);
    }

    // The entries are packed straight into the reply, where the encoder
    // leaves them: no copy.
    auto writer = ours_io::DirentWriter(ours_io::reply_payload(out.bytes).first(request.max_bytes));
    if (connection.m_phase == Connection::DirentPhase::Dot) {
        auto const dot = ours_io::Dirent{
            .ino = ours_io::INO_UNKNOWN,
            .type = ours_io::DirentType::Directory,
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
        auto const dirent = ours_io::Dirent{
            .ino = ours_io::INO_UNKNOWN,
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
    return ours_io::encode_read_dirents_reply(out.bytes, request.txid,
                                              {.status = Status::Ok, .dirents = writer.written()});
}

auto Server::read(Connection &connection, ours_io::ReadRequest const &request, ReplyBuffer &out)
    -> ustl::Result<wire::Message, Status> {
    if (!holds(connection.m_rights, Operations::ReadBytes)) {
        return ustl::err(Status::BadHandle);
    }
    if (request.count > ours_io::MAX_TRANSFER_SIZE) {
        return ustl::err(Status::OutOfRange);
    }
    // Copied from the program's own view of the file into the reply: the
    // one copy in the server that design D17 counts. Past the end is 0
    // bytes, which is how end of file reads.
    auto const bytes = m_tree->file(connection.m_node)->bytes;
    auto const offset = std::min<u64>(connection.m_seek, bytes.size());
    auto const size = std::min<u64>(request.count, bytes.size() - offset);
    auto const payload = ours_io::reply_payload(out.bytes).first(size);
    std::ranges::copy(bytes.subspan(offset, size), payload.begin());
    connection.m_seek = offset + size;
    return ours_io::encode_read_reply(out.bytes, request.txid,
                                      {.status = Status::Ok, .data = payload});
}

auto Server::get_backing_memory(Connection const &connection,
                                ours_io::GetBackingMemoryRequest const &request, ReplyBuffer &out)
    -> ustl::Result<wire::Message, Status> {
    auto const flags = request.flags;
    // What the VMO may do is what the connection may do.
    if (any_of(flags, VmoFlags::PrivateClone) && any_of(flags, VmoFlags::SharedBuffer)) {
        return ustl::err(Status::InvArgs);
    }
    if ((any_of(flags, VmoFlags::Read) && !holds(connection.m_rights, Operations::ReadBytes)) ||
        (any_of(flags, VmoFlags::Write) && !holds(connection.m_rights, Operations::WriteBytes)) ||
        (any_of(flags, VmoFlags::Execute) && !holds(connection.m_rights, Operations::Execute))) {
        return ustl::err(Status::AccessDenied);
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
    return ours_io::encode_get_backing_memory_reply(out.bytes, out.handles, request.txid,
                                                    {.status = Status::Ok, .vmo = vmo});
}

auto Server::dispatch(ConnectionId const id, wire::Message const message, ReplyBuffer &out)
    -> Outcome {
    auto *const connection = find(id);
    if (connection == nullptr) {
        return Outcome{.action = Action::Drop};
    }
    // The direction byte first: a message going to a client is never a
    // request, whatever its ordinal (design I5).
    auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
    if (header.is_err()) {
        return drop(id);
    }
    auto const ordinal = header->ordinal;

    // A two-way method's answer: its Ok reply as the handler encoded it, or
    // the status form of its failure.
    auto const answer = [&](wire::Method const &method, u32 const txid,
                            ustl::Result<wire::Message, Status> const &handled) -> Outcome {
        if (handled.is_ok()) {
            return Outcome{.action = Action::Reply, .message = handled.unwrap()};
        }
        auto const encoded =
            wire::encode_status_reply(out.bytes, method, txid, handled.unwrap_err());
        if (encoded.is_err()) {
            return drop(id);
        }
        return Outcome{.action = Action::Reply, .message = encoded.unwrap()};
    };

    // Node: every connection.
    if (ordinal == ours_io::CLOSE.ordinal) {
        auto const request = ours_io::decode_close(message);
        if (request.is_err()) {
            return drop(id);
        }
        auto const reply =
            ours_io::encode_close_reply(out.bytes, request->txid, {.status = Status::Ok});
        release(id);
        if (reply.is_err()) {
            return Outcome{.action = Action::Drop};
        }
        return Outcome{.action = Action::ReplyAndClose, .message = reply.unwrap()};
    }
    if (ordinal == ours_io::CLONE.ordinal) {
        auto const request = ours_io::decode_clone(message);
        if (request.is_err()) {
            return drop(id);
        }
        // The same node with the same rights, from the start again.
        auto cloned = open_connection(request->request, connection->m_node, connection->m_rights);
        if (cloned.is_err()) {
            return refuse(request->request, cloned.unwrap_err(), out);
        }
        return cloned.unwrap();
    }

    if (*m_tree->kind(connection->m_node) == NodeKind::Directory) {
        if (ordinal == ours_io::OPEN.ordinal) {
            auto const request = ours_io::decode_open(message);
            if (request.is_err()) {
                return drop(id);
            }
            auto opened = open(*connection, request.unwrap(), out);
            if (opened.is_err()) {
                return refuse_open(request->object, opened.unwrap_err(), request->path, out);
            }
            return opened.unwrap();
        }
        if (ordinal == ours_io::READ_DIRENTS.ordinal) {
            auto const request = ours_io::decode_read_dirents(message);
            if (request.is_err()) {
                return drop(id);
            }
            return answer(ours_io::READ_DIRENTS, request->txid,
                          read_dirents(*connection, request.unwrap(), out));
        }
    } else {
        if (ordinal == ours_io::READ.ordinal) {
            auto const request = ours_io::decode_read(message);
            if (request.is_err()) {
                return drop(id);
            }
            return answer(ours_io::READ, request->txid, read(*connection, request.unwrap(), out));
        }
        if (ordinal == ours_io::GET_BACKING_MEMORY.ordinal) {
            auto const request = ours_io::decode_get_backing_memory(message);
            if (request.is_err()) {
                return drop(id);
            }
            return answer(ours_io::GET_BACKING_MEMORY, request->txid,
                          get_backing_memory(*connection, request.unwrap(), out));
        }
    }

    // A method this connection does not serve: a File method on a
    // directory, say, or an ordinal nobody defines. The header has no bit
    // that would mark a call as one an older server may answer "unknown" to
    // (byte 4 is the direction), so every unknown call closes the channel.
    return drop(id);
}
} // namespace ioservice
