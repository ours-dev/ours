/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// memfs -- a writable in-memory filesystem (macro-fs design): a flat
/// directory of files a client creates, writes and reads back, speaking
/// ours.io on a Loop.
///
/// dev's meta/root.cm starts it as the eager child `memfs` and offers its
/// directory `tmp` to fshost, whose tree mounts it at the entry `tmp`; a
/// client of /vfs therefore reaches these files at /vfs/tmp/..., through
/// fshost's forwarded Open. It does not serve VMOs: GetBackingMemory answers
/// NotSupported, and nothing here is executable.
///
/// It speaks the protocol directly, over the wire and ours_io codecs, and
/// not through ioservice: an ioservice Tree is read-only and closed, its
/// nodes fixed spans a program fills before serving, and what makes memfs a
/// filesystem is that a client changes both the set of files and their
/// bytes. What carries over from the server library is the shape of the
/// dispatch -- one message per wait, the Open checks in their order, an
/// epitaph on a refused Open -- and the LoopServer's per-connection Binding.
///
/// The objects:
///   - the Loop, one port and a slot per connection;
///   - the file pool: a fixed array of files, each a name and a fixed-size
///     byte array with a size; no heap, and no VMO behind them;
///   - the connection table: one row per client channel -- the outgoing
///     directory itself, directories, and files -- each with its rights, the
///     file it is to, its seek offset and its listing cursor;
///   - one Binding per connection, which reads one message and answers it.
///
/// The events: fshost writes Open("tmp", rights, server) into the outgoing
/// directory when a component whose manifest uses `tmp` starts; the root
/// connection binds |server| to the root directory. An Open of a name below
/// it finds the file or, with MAYBE_CREATE and MODIFY_DIRECTORY, makes one;
/// Read and Write move the connection's seek offset; Close ends the
/// connection and frees its row. The files last as long as this process.
#include <ours/port.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

#include <ioservice/tree.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
#include <ours_io/ours_io.hpp>
#include <ustl/lazy_init.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Signals;
using ours::Status;
using ours::Uandle;
using ours_io::Flags;
using ours_io::Operations;

/// How many files, how big each, how long a name. Fixed because there is no
/// heap: a file past the pool is answered NoResource, a byte past a file's
/// array is a short write, a longer name NotFound.
constexpr auto MAX_FILES = usize{16};
constexpr auto FILE_SIZE = usize{2048};
constexpr auto NAME_SIZE = usize{32};

/// How many connections may be live at once: the outgoing directory, plus
/// every directory and file connection clients hold.
constexpr auto MAX_CONNECTIONS = usize{8};

/// The Loop's slots: one per connection, one spare.
constexpr auto SLOTS = MAX_CONNECTIONS + 1;

static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS);

/// The name root.cm routes by, which init's forwarded Open carries: an Open
/// of it is an Open of the pool's root, as "." is a clone of it.
constexpr auto EXPOSED_NAME = std::string_view{"tmp"};

/// What the outgoing root is served with: the ceiling of what a use of
/// `tmp` may ask. root.cm offers it rw*, whose every bit a use states has to
/// be below this ceiling -- a missing one is AccessDenied on the forwarded
/// Open, and the mount point dies with it.
constexpr auto SERVED_RIGHTS = ours_io::RW_STAR_DIR;

/// Every right the protocol defines, what a request's rights are read
/// against.
constexpr auto ALL_OPERATIONS =
    Operations::Connect | Operations::ReadBytes | Operations::WriteBytes | Operations::Execute |
    Operations::GetAttributes | Operations::UpdateAttributes | Operations::Enumerate |
    Operations::Traverse | Operations::ModifyDirectory;

/// The flags that ask Open to create what it names.
constexpr auto CREATION_FLAGS = Flags::FlagMaybeCreate | Flags::FlagMustCreate;

auto rights_of(Flags const flags) -> Operations {
    return static_cast<Operations>(std::to_underlying(flags) & std::to_underlying(ALL_OPERATIONS));
}

auto holds(Operations const rights, Operations const bits) -> bool {
    return (rights & bits) == bits;
}

/// One file: its name and its bytes. Rows are taken in order and never
/// freed, so a row past the pool is NoResource, not a hole.
struct File {
    bool used = false;
    u32 size = 0;
    std::array<char, NAME_SIZE> name{};
    std::array<u8, FILE_SIZE> data{};
};

/// What a connection is to: the root directory, or the file at a pool index
/// (|file|).
enum class Target : u8 {
    Root,
    File,
};

/// One client channel and its state. |channel| Invalid marks a free row.
struct Connection {
    Uandle channel = Uandle::Invalid;
    Target target = Target::Root;
    usize file = 0;
    Operations rights = Operations::None;
    /// A file's: where Read and Write go on from.
    u64 seek = 0;
    /// The root's: whether "." has been listed, and the next pool index
    /// ReadDirents lists, past the last when the listing is done.
    bool listed_dot = false;
    usize next_entry = 0;
};

/// Frees the row |index|, and serves one message on it; defined below, and
/// what a Binding's callback does.
auto forget(usize index) -> void;
auto handle(usize index) -> loop::Action;

/// One connection's Loop slot, as ioservice::LoopServer::Binding is: the
/// Loop keeps its address, so the array of them is static storage that never
/// moves, and a row's Binding is reused only after its row is freed.
class Binding final: public loop::Waiter {
  public:
    constexpr Binding() = default;

    auto on_signals(Signals const observed) -> loop::Action override {
        if (!(observed & Signals::Readable)) {
            // The client went away. Free the row; the Loop closes the
            // channel, this callback having answered Close.
            forget(index);
            return loop::Action::Close;
        }
        return handle(index);
    }

    u32 index = 0;
};

// The state, outside the stack: a user program runs no dynamic
// initialisers, so main() builds into raw storage (ustl::LazyInit, as
// echo_server's is).
struct Storage {
    std::array<File, MAX_FILES> files;
    std::array<Connection, MAX_CONNECTIONS> connections;
    std::array<Binding, MAX_CONNECTIONS> bindings;
    std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
    std::array<u8, wire::MAX_MESSAGE_BYTES> reply_bytes;
    std::array<Uandle, 1> reply_handles;
};

constinit ustl::LazyInit<Storage> STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};

/// The Loop the connections are armed on, set before any message can
/// arrive.
loop::Loop *LOOP = nullptr;

/// A free row, filled in as far as |channel| and |rights|; MAX_CONNECTIONS
/// when every row is in use. The caller sets the target, the file and the
/// cursors.
auto allocate(Uandle const channel, Operations const rights) -> usize {
    auto &storage = *STORAGE;
    for (auto index = usize{0}; index < MAX_CONNECTIONS; ++index) {
        auto &connection = storage.connections[index];
        if (connection.channel != Uandle::Invalid) {
            continue;
        }
        connection = {.channel = channel, .rights = rights};
        return index;
    }
    return MAX_CONNECTIONS;
}

/// Frees the row |index|. Its channel closes right after, when the callback
/// that freed it answered Close.
auto forget(usize const index) -> void {
    if (index < MAX_CONNECTIONS) {
        STORAGE->connections[index] = {};
    }
}

/// Arms |channel| on the connection |index|'s Binding. On failure the row is
/// freed and the channel is still the caller's.
auto watch(Uandle const channel, usize const index) -> Status {
    auto &storage = *STORAGE;
    auto &binding = storage.bindings[index];
    binding.index = static_cast<u32>(index);
    auto const armed = LOOP->add(channel, Signals::Readable | Signals::PeerClosed, binding);
    if (armed.is_err()) {
        forget(index);
        return armed.unwrap_err();
    }
    return Status::Ok;
}

/// The file |name| is at, or MAX_FILES for none.
auto find_file(std::string_view const name) -> usize {
    auto &storage = *STORAGE;
    for (auto index = usize{0}; index < MAX_FILES; ++index) {
        auto const &file = storage.files[index];
        if (file.used && std::string_view{file.name.data()} == name) {
            return index;
        }
    }
    return MAX_FILES;
}

/// Makes the file |name|, or MAX_FILES when the pool or the name leaves no
/// room.
auto create_file(std::string_view const name) -> usize {
    if (name.empty() || name.size() >= NAME_SIZE) {
        return MAX_FILES;
    }
    auto &storage = *STORAGE;
    for (auto index = usize{0}; index < MAX_FILES; ++index) {
        auto &file = storage.files[index];
        if (file.used) {
            continue;
        }
        file = {.used = true, .size = 0};
        name.copy(file.name.data(), name.size());
        return index;
    }
    return MAX_FILES;
}

/// Writes |message| on |channel|; what channel_write answers, which the
/// callers here cannot act on.
auto write(Uandle const channel, wire::Message const message) -> Status {
    return ours::channel_write(channel, ours::ChannelWriteOptions::None, message.bytes.data(),
                               static_cast<u32>(message.bytes.size()), message.handles.data(),
                               static_cast<u32>(message.handles.size()));
}

/// Closes |channel| after an epitaph saying |status|: how a refused Open is
/// answered, there being no reply to carry the status.
auto refuse(Uandle const channel, Status const status) -> void {
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    auto const epitaph = wire::encode_epitaph(bytes, status);
    if (epitaph.is_ok()) {
        auto _ = write(channel, epitaph.unwrap());
    }
    auto _ = ours::handle_close(channel);
}

/// Answers the two-way call |txid| named |method| with |status| alone.
auto answer_status(Uandle const channel, wire::Method const &method, u32 const txid,
                   Status const status) -> void {
    std::array<u8, wire::STATUS_FORM_SIZE> bytes{};
    auto const encoded = wire::encode_status_reply(bytes, method, txid, status);
    if (encoded.is_ok()) {
        auto _ = write(channel, encoded.unwrap());
    }
}

/// Read on the file connection |index|.
auto read_file(Connection &connection, ours_io::ReadRequest const &request,
               std::span<u8> reply_bytes) -> void {
    auto &file = STORAGE->files[connection.file];
    auto const offset = std::min<u64>(connection.seek, file.size);
    auto const size =
        std::min<u64>(std::min<u64>(request.count, file.size - offset), ours_io::MAX_TRANSFER_SIZE);
    auto const payload = reply_bytes.subspan(wire::STATUS_FORM_SIZE).first(size);
    std::ranges::copy(file.data.data() + offset, file.data.data() + offset + size, payload.begin());
    connection.seek = offset + size;
    auto const encoded = ours_io::encode_read_reply(reply_bytes, request.txid,
                                                    {.status = Status::Ok, .data = payload});
    if (encoded.is_ok()) {
        auto _ = write(connection.channel, encoded.unwrap());
    }
}

/// Write on the file connection |index|.
auto write_file(Connection &connection, ours_io::WriteRequest const &request,
                std::span<u8> reply_bytes) -> void {
    auto &file = STORAGE->files[connection.file];
    // Writing past the size zeroes up to the offset, as a sparse write reads
    // back; past the file's array is a short write.
    auto const offset = std::min<u64>(connection.seek, FILE_SIZE);
    if (offset > file.size) {
        std::ranges::fill(file.data.data() + file.size, file.data.data() + offset, u8{0});
    }
    auto const size = std::min<u64>(request.data.size(), FILE_SIZE - offset);
    std::ranges::copy(request.data.first(size), file.data.data() + offset);
    file.size = std::max<u32>(file.size, static_cast<u32>(offset + size));
    connection.seek = offset + size;
    auto const encoded = ours_io::encode_write_reply(
        reply_bytes, request.txid, {.status = Status::Ok, .actual = static_cast<u32>(size)});
    if (encoded.is_ok()) {
        auto _ = write(connection.channel, encoded.unwrap());
    }
}

/// ReadDirents on the root connection: "." first, then the files in pool
/// order, the entries packed where the encoder keeps them.
auto read_dirents(Connection &connection, ours_io::ReadDirentsRequest const &request,
                  std::span<u8> reply_bytes) -> void {
    auto &storage = *STORAGE;
    if (!holds(connection.rights, Operations::Enumerate)) {
        answer_status(connection.channel, ours_io::READ_DIRENTS, request.txid,
                      Status::AccessDenied);
        return;
    }
    auto writer = ours_io::DirentWriter(reply_bytes.subspan(wire::STATUS_FORM_SIZE));
    if (!connection.listed_dot) {
        if (writer.append({.ino = ours_io::INO_UNKNOWN,
                           .type = ours_io::DirentType::Directory,
                           .name = "."}) != Status::Ok) {
            answer_status(connection.channel, ours_io::READ_DIRENTS, request.txid,
                          Status::BufferTooSmall);
            return;
        }
        connection.listed_dot = true;
    }
    while (connection.next_entry < MAX_FILES) {
        auto const &file = storage.files[connection.next_entry];
        if (!file.used) {
            connection.next_entry += 1;
            continue;
        }
        if (writer.append({.ino = ours_io::INO_UNKNOWN,
                           .type = ours_io::DirentType::File,
                           .name = std::string_view{file.name.data()}}) != Status::Ok) {
            break;
        }
        connection.next_entry += 1;
    }
    auto const encoded = ours_io::encode_read_dirents_reply(
        reply_bytes, request.txid, {.status = Status::Ok, .dirents = writer.written()});
    if (encoded.is_ok()) {
        auto _ = write(connection.channel, encoded.unwrap());
    }
}

/// Binds |open|'s server end to a connection of its own: the root when
/// |file_index| is MAX_FILES, the file at that index otherwise.
auto open_below_root_bind(ours_io::OpenRequest const &open, Operations const requested,
                          usize const file_index) -> void;

/// Open on the root connection: find what |open| names, or make it, and
/// bind its server end to a connection of its own. The checks are
/// ioservice's, in its order.
auto open_below_root(Connection &connection, ours_io::OpenRequest const &open) -> void {
    auto const flags = open.flags;
    if (!ioservice::is_valid_path(open.path)) {
        refuse(open.object, Status::BadPath);
        return;
    }
    if (open.path != "." && !holds(connection.rights, Operations::Traverse)) {
        refuse(open.object, Status::AccessDenied);
        return;
    }
    auto const requested = rights_of(flags);
    if ((requested & ~connection.rights) != Operations::None) {
        refuse(open.object, Status::AccessDenied);
        return;
    }
    if ((flags & CREATION_FLAGS) != Flags::None &&
        !holds(connection.rights, Operations::ModifyDirectory)) {
        refuse(open.object, Status::AccessDenied);
        return;
    }

    // The root is the only directory, and a file is one name: a path with a
    // '/' in it asks below the flat root, where nothing is.
    if (open.path != "." && open.path.find('/') != std::string_view::npos) {
        refuse(open.object, Status::NotFound);
        return;
    }

    // "." and the exposed name are both the pool's root; everything else
    // names a file.
    auto const at_root = open.path == "." || open.path == EXPOSED_NAME;
    auto const file_index = at_root ? MAX_FILES : find_file(open.path);
    if (!at_root && file_index == MAX_FILES) {
        if ((flags & CREATION_FLAGS) == Flags::None) {
            refuse(open.object, Status::NotFound);
            return;
        }
        auto const made = create_file(open.path);
        if (made == MAX_FILES) {
            refuse(open.object, Status::NoResource);
            return;
        }
        return open_below_root_bind(open, requested, made);
    }
    if ((flags & Flags::FlagMustCreate) != Flags::None) {
        refuse(open.object, Status::AlreadyExists);
        return;
    }
    return open_below_root_bind(open, requested, file_index);
}

/// Binds |open|'s server end to a connection of its own: the root when
/// |file_index| is MAX_FILES, the file at that index otherwise.
auto open_below_root_bind(ours_io::OpenRequest const &open, Operations const requested,
                          usize const file_index) -> void {
    auto &storage = *STORAGE;
    auto const made = allocate(open.object, requested);
    if (made == MAX_CONNECTIONS) {
        refuse(open.object, Status::NoResource);
        return;
    }
    auto &made_connection = storage.connections[made];
    made_connection.target = file_index == MAX_FILES ? Target::Root : Target::File;
    made_connection.file = file_index;
    if (watch(open.object, made) != Status::Ok) {
        refuse(open.object, Status::NoResource);
    }
}

/// Serves one message that arrived on the connection |index|'s channel: the
/// dispatch, one message per wait, as ioservice's Server::dispatch is. The
/// replies go through the shared reply buffer, which one callback at a time
/// fills and drains.
auto handle(usize const index) -> loop::Action {
    auto &storage = *STORAGE;
    auto &connection = storage.connections[index];
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    if (auto const read = ours::channel_read(
            connection.channel, ours::ChannelReadOptions::None, storage.request_bytes.data(),
            storage.request_handles.data(), static_cast<u32>(storage.request_bytes.size()),
            static_cast<u32>(storage.request_handles.size()), &num_bytes, &num_handles);
        read != Status::Ok) {
        forget(index);
        return loop::Action::Close;
    }
    auto const message = wire::Message{
        .bytes = std::span{storage.request_bytes}.first(num_bytes),
        .handles = std::span{storage.request_handles}.first(num_handles),
    };
    auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
    if (header.is_err()) {
        forget(index);
        return loop::Action::Close;
    }
    auto const ordinal = header->ordinal;

    // Node.Close, on every connection.
    if (ordinal == ours_io::CLOSE.ordinal) {
        auto const request = ours_io::decode_close(message);
        if (request.is_err()) {
            forget(index);
            return loop::Action::Close;
        }
        auto const encoded =
            ours_io::encode_close_reply(storage.reply_bytes, request->txid, {.status = Status::Ok});
        auto const channel = connection.channel;
        forget(index);
        if (encoded.is_ok()) {
            auto _ = write(channel, encoded.unwrap());
        }
        return loop::Action::Close;
    }

    // Cloneable.Clone: another connection to the same target, with the same
    // rights, from the start again.
    if (ordinal == ours_io::CLONE.ordinal) {
        auto const request = ours_io::decode_clone(message);
        if (request.is_err()) {
            forget(index);
            return loop::Action::Close;
        }
        auto const made = allocate(request->request, connection.rights);
        if (made == MAX_CONNECTIONS) {
            refuse(request->request, Status::NoResource);
            return loop::Action::Rearm;
        }
        auto &made_connection = storage.connections[made];
        made_connection.target = connection.target;
        made_connection.file = connection.file;
        if (watch(request->request, made) != Status::Ok) {
            refuse(request->request, Status::NoResource);
        }
        return loop::Action::Rearm;
    }

    // Directory.Open, on the root only.
    if (ordinal == ours_io::OPEN.ordinal) {
        if (connection.target != Target::Root) {
            forget(index);
            return loop::Action::Close;
        }
        auto const request = ours_io::decode_open(message);
        if (request.is_err()) {
            forget(index);
            return loop::Action::Close;
        }
        open_below_root(connection, request.unwrap());
        return loop::Action::Rearm;
    }

    // Directory.ReadDirents, on the root only.
    if (ordinal == ours_io::READ_DIRENTS.ordinal) {
        auto const request = ours_io::decode_read_dirents(message);
        if (request.is_err() || connection.target != Target::Root) {
            forget(index);
            return loop::Action::Close;
        }
        read_dirents(connection, request.unwrap(), storage.reply_bytes);
        return loop::Action::Rearm;
    }

    // File.Read and File.Write, on a file connection.
    if (ordinal == ours_io::READ.ordinal || ordinal == ours_io::WRITE.ordinal) {
        auto const is_read = ordinal == ours_io::READ.ordinal;
        if (connection.target != Target::File) {
            forget(index);
            return loop::Action::Close;
        }
        if (is_read && !holds(connection.rights, Operations::ReadBytes)) {
            answer_status(connection.channel, ours_io::READ, header->txid, Status::AccessDenied);
            return loop::Action::Rearm;
        }
        if (!is_read && !holds(connection.rights, Operations::WriteBytes)) {
            answer_status(connection.channel, ours_io::WRITE, header->txid, Status::AccessDenied);
            return loop::Action::Rearm;
        }
        if (is_read) {
            auto const request = ours_io::decode_read(message);
            if (request.is_err()) {
                forget(index);
                return loop::Action::Close;
            }
            read_file(connection, request.unwrap(), storage.reply_bytes);
        } else {
            auto const request = ours_io::decode_write(message);
            if (request.is_err()) {
                forget(index);
                return loop::Action::Close;
            }
            write_file(connection, request.unwrap(), storage.reply_bytes);
        }
        return loop::Action::Rearm;
    }

    // GetBackingMemory: nothing here is backed by a VMO, now or ever.
    if (ordinal == ours_io::GET_BACKING_MEMORY.ordinal) {
        auto const request = ours_io::decode_get_backing_memory(message);
        if (request.is_err()) {
            forget(index);
            return loop::Action::Close;
        }
        answer_status(connection.channel, ours_io::GET_BACKING_MEMORY, request->txid,
                      Status::NotSupported);
        return loop::Action::Rearm;
    }

    // A method this connection does not serve: the channel closes, as
    // ioservice's server closes it.
    forget(index);
    return loop::Action::Close;
}
} // namespace

auto main(int, char **) -> int {
    auto const directory_request = ort::take_startup_handle(ours::HandleType::DirectoryRequest, 0);
    if (directory_request == Uandle::Invalid) {
        auto _ = ort::debug_line("memfs: started without a DirectoryRequest handle");
        return 1;
    }

    STORAGE.init();
    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        auto _ = ort::debug_line({"memfs: cannot make its port: ", ours::to_string(status)});
        auto _ = ours::handle_close(directory_request);
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
    LOOP = &loop;

    // The outgoing directory: the root of the file pool, as writable as the
    // pool is.
    auto const root = allocate(directory_request, SERVED_RIGHTS);
    if (root == MAX_CONNECTIONS) {
        auto _ = ort::debug_line("memfs: no row for its outgoing directory");
        auto _ = ours::handle_close(directory_request);
        return 1;
    }
    if (auto const armed = watch(directory_request, root); armed != Status::Ok) {
        auto _ = ort::debug_line(
            {"memfs: cannot wait on its outgoing directory: ", ours::to_string(armed)});
        auto _ = ours::handle_close(directory_request);
        return 1;
    }

    // Stop ends the program from inside an on_signals(); run() comes back
    // only when the port itself fails.
    auto _ = ort::debug_line(
        {"memfs: its Loop stopped: ", ours::to_string(loop.run(ours::INSTANT_INFINITE))});
    return 1;
}
