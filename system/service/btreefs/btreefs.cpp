/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// btreefs -- the filesystem the ours-fs partition carries, served over
/// ours.io on a Loop (btreefs design, .agents/tasks/btreefs/DESIGN.md): a
/// flat directory of files in a B+tree on the disk, a client creating,
/// writing and reading them back.
///
/// dev's meta/root.cm starts it as the eager child `btreefs`, offers it the
/// block device's ours.block.Block, and offers its directory `disk` to
/// fshost -- which mounts it at the entry `disk` -- and to routing_checks,
/// which exercises create, write and read-back on it directly. A client of
/// /vfs reaches the files at /vfs/disk/... through fshost's forwarded Open.
///
/// The connection to the disk is ours.block's; the volume is the disk's
/// ours-fs partition, found through ours.gpt, and formatted by this program
/// when it carries none yet -- a blank partition is a filesystem waiting to
/// be made, not an error. Without a disk, or without the partition, this
/// program exits having served nothing, which is an answer and not a
/// failure: the images boot_check boots carry no disk.
///
/// The objects:
///   - the Client, ours.block's, one call at a time on its channel;
///   - the adapter that makes ours.btreefs's 4 KiB blocks out of the
///     client's 512-byte sectors, offset to the partition's first LBA;
///   - the Volume, the tree and the allocator's state, kept current with
///     the superblock;
///   - the connection table, one row per client channel -- the outgoing
///     directory, directories, and files -- each with its rights, the file
///     it is to, its seek offset and its listing cursor;
///   - one Binding per connection, which reads one message and answers it.
///
/// The events: fshost writes Open("disk", rights, server) into the outgoing
/// directory when a component whose manifest uses it starts; the root
/// connection binds |server| to the root directory. An Open of a name finds
/// the file or, with MAYBE_CREATE and MODIFY_DIRECTORY, makes one -- a
/// DIRENT item for the name, an INODE item for the object; Read and Write
/// move the connection's seek offset and go through the file's DATA items,
/// one per 4 KiB of the file; ReadDirents walks the root's DIRENT items
/// along the leaves' chain. Close ends the connection and frees its row.
/// The files last as long as the disk does.
#include <ours/port.hpp>
#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

#include <cstring>

#include <fsio/fsio.hpp>
#include <ioservice/tree.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
#include <ours_block/client.hpp>
#include <ours_btreefs/ours_btreefs.hpp>
#include <ours_gpt/ours_gpt.hpp>
#include <ours_io/ours_io.hpp>
#include <ustl/lazy_init.hpp>
#include <wire/wire.hpp>

namespace {

using ours::Signals;
using ours::Status;
using ours::Uandle;
using ours_btreefs::ItemType;
using ours_btreefs::ObjectKind;
using ours_io::Flags;
using ours_io::Operations;

/// How many connections may be live at once: the outgoing directory, plus
/// every directory and file connection clients hold.
constexpr auto MAX_CONNECTIONS = usize{8};

/// The Loop's slots: one per connection, one spare.
constexpr auto SLOTS = MAX_CONNECTIONS + 1;

static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS);

/// The name root.cm routes by, which fshost's forwarded Open carries: an
/// Open of it is an Open of the volume's root, as "." is a clone of it.
constexpr auto EXPOSED_NAME = std::string_view{"disk"};

/// What the outgoing root is served with: the ceiling of what a use of
/// `disk` may ask. root.cm offers it rw*, whose every bit a use states has
/// to be below this ceiling -- a missing one is AccessDenied on the
/// forwarded Open, and the mount point dies with it.
constexpr auto SERVED_RIGHTS = ours_io::RW_STAR_DIR;

/// Every right the protocol defines, what a request's rights are read
/// against.
constexpr auto ALL_OPERATIONS =
    Operations::Connect | Operations::ReadBytes | Operations::WriteBytes | Operations::Execute |
    Operations::GetAttributes | Operations::UpdateAttributes | Operations::Enumerate |
    Operations::Traverse | Operations::ModifyDirectory;

/// The flags that ask Open to create what it names.
constexpr auto CREATION_FLAGS = Flags::FlagMaybeCreate | Flags::FlagMustCreate;

/// The object id of the root directory: the first alloc_object() of a fresh
/// volume hands out.
constexpr auto ROOT_OBJECT = u64{1};

auto rights_of(Flags const flags) -> Operations {
    return static_cast<Operations>(std::to_underlying(flags) & std::to_underlying(ALL_OPERATIONS));
}

auto holds(Operations const rights, Operations const bits) -> bool {
    return (rights & bits) == bits;
}

/// The disk through ours.block, as ours.btreefs sees it: the partition's
/// 4 KiB block |block| is the 8 sectors from the partition's first LBA,
/// read and written in the chunks of 7 the block codec carries per call.
class BlockAdapter final {
  public:
    BlockAdapter(ours_block::Client &client, u64 const first_lba)
        : m_client(client), m_first_lba(first_lba) {}

    auto read_block(u64 const block, std::span<u8> bytes) -> Status {
        if (bytes.size() != ours_btreefs::BLOCK_BYTES) {
            return Status::InvArgs;
        }
        auto done = usize{0};
        while (done < ours_btreefs::BLOCK_BYTES) {
            auto const lbas = std::min<usize>(7, (ours_btreefs::BLOCK_BYTES - done) / 512);
            if (auto const status =
                    m_client.read(sector(block) + done / 512, static_cast<u32>(lbas),
                                  bytes.subspan(done, lbas * 512));
                status != Status::Ok) {
                return status;
            }
            done += lbas * 512;
        }
        return Status::Ok;
    }

    auto write_block(u64 const block, std::span<u8 const> bytes) -> Status {
        if (bytes.size() != ours_btreefs::BLOCK_BYTES) {
            return Status::InvArgs;
        }
        auto done = usize{0};
        while (done < ours_btreefs::BLOCK_BYTES) {
            auto const lbas = std::min<usize>(7, (ours_btreefs::BLOCK_BYTES - done) / 512);
            if (auto const status =
                    m_client.write(sector(block) + done / 512, bytes.subspan(done, lbas * 512));
                status != Status::Ok) {
                return status;
            }
            done += lbas * 512;
        }
        return Status::Ok;
    }

  private:
    /// The first sector of the volume's block |block|.
    [[nodiscard]]
    auto sector(u64 const block) const -> u64 {
        return m_first_lba + block * (ours_btreefs::BLOCK_BYTES / 512);
    }

    ours_block::Client &m_client;
    u64 m_first_lba = 0;
};

/// What a connection is to: a directory at an object id (the volume's root
/// among them), or a file at one.
enum class Target : u8 {
    Directory,
    File,
};

/// One client channel and its state. |channel| Invalid marks a free row.
struct Connection {
    Uandle channel = Uandle::Invalid;
    Target target = Target::Directory;
    u64 object = 0;
    Operations rights = Operations::None;
    /// A file's: where Read and Write go on from.
    u64 seek = 0;
    /// The root's: whether "." has been listed, and the DIRENT offset one
    /// past the last entry listed -- entries at or past that only, the
    /// item that failed to fit coming again. (One past, not at: an entry
    /// whose folded offset is 0 must not be swallowed by the initial
    /// cursor of 0.)
    bool listed_dot = false;
    u64 next_offset = 0;
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
    std::array<Connection, MAX_CONNECTIONS> connections;
    std::array<Binding, MAX_CONNECTIONS> bindings;
    std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
    std::array<u8, wire::MAX_MESSAGE_BYTES> reply_bytes;
};

constinit ustl::LazyInit<Storage> STORAGE;
constinit ustl::LazyInit<ours_block::Client> CLIENT;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};

/// The Loop the connections are armed on, set before any message can
/// arrive.
loop::Loop *LOOP = nullptr;

/// The filesystem over the volume: the tree and the allocator's state, the
/// device they run through, and the one scratch block the operations share.
/// What a client's channel does is this object's; what watches the channel
/// is Storage's, the Loop's own plumbing.
class Filesystem final {
  public:
    explicit Filesystem(ours_block::Client &client) : m_client(client) {}

    /// Opens the volume on the disk's ours-fs partition -- found through the
    /// GPT, formatted when it carries none -- and makes the root directory.
    /// False when there is no volume to serve, an answer and not a failure.
    auto mount() -> bool;

    auto inode_of(u64 object) -> ustl::Result<ours_btreefs::InodeValue, Status>;
    auto find_child(u64 directory, std::string_view name)
        -> ustl::Result<ours_btreefs::DirentValue, Status>;
    auto resolve(u64 directory, std::string_view path)
        -> ustl::Result<std::pair<u64, ObjectKind>, Status>;
    auto create_object(u64 directory, std::string_view name, ObjectKind kind)
        -> ustl::Result<u64, Status>;
    auto read_file(Connection &connection, ours_io::ReadRequest const &request,
                   std::span<u8> reply_bytes) -> void;
    auto write_file(Connection &connection, ours_io::WriteRequest const &request,
                    std::span<u8> reply_bytes) -> void;
    auto read_dirents(Connection &connection, ours_io::ReadDirentsRequest const &request,
                      std::span<u8> reply_bytes) -> void;
    auto unlink_at(u64 directory, std::string_view name) -> Status;

    /// Empties the file |object|: every DATA item's block back, the INODE's
    /// size to zero, one transaction.
    auto truncate_file(u64 object) -> Status;

  private:
    /// The device the tree operations run through, made once at mount: the
    /// templates take it by reference, and a fresh temporary per call would
    /// not bind.
    auto device() -> BlockAdapter & {
        return *m_adapter;
    }

    /// The operation's transaction, begun on the storage this object holds.
    auto tx_begin() -> ours_btreefs::Transaction<BlockAdapter> & {
        auto &tx = *m_tx;
        tx.begin();
        return tx;
    }

    ours_block::Client &m_client;
    ustl::LazyInit<BlockAdapter> m_adapter;
    /// The transaction's own storage, outside the stack: its cache of
    /// copied blocks is ~200 KiB, far past this thread's stack to carry.
    ustl::LazyInit<ours_btreefs::Transaction<BlockAdapter>> m_tx;
    ours_btreefs::Volume m_volume{};
    /// One 4 KiB node or data block, patched and written back: the tree's
    /// operations run one at a time on the Loop's thread.
    std::array<u8, ours_btreefs::BLOCK_BYTES> m_block{};
    /// Scratch for one item's value, a lookup's out and an update's in.
    ours_btreefs::Value m_item{};
};

constinit ustl::LazyInit<Filesystem> FILESYSTEM;

/// A free row, filled in as far as |channel| and |rights|; MAX_CONNECTIONS
/// when every row is in use. The caller sets the target and the cursors.
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

// --- The filesystem over the tree. ---

/// The object's INODE item, or the failure of the tree.
auto Filesystem::inode_of(u64 const object) -> ustl::Result<ours_btreefs::InodeValue, Status> {
    auto const looked = ours_btreefs::lookup(
        device(), m_volume, {.object = object, .type = ItemType::Inode, .offset = 0}, m_item);
    if (looked != Status::Ok) {
        return ustl::err(looked == Status::NotFound ? Status::BadState : looked);
    }
    return ours_btreefs::decode_inode_value(m_item);
}

/// What one name under |directory| answers: the child's object and kind, or
/// NotFound. The DIRENT's offset is the name's hash and its value carries
/// the name, so a match is checked against the name itself -- a hash the
/// tree keeps unique says where to look, the decoded name says what was
/// found.
auto Filesystem::find_child(u64 const directory, std::string_view const name)
    -> ustl::Result<ours_btreefs::DirentValue, Status> {
    auto const key = ours_btreefs::Key{
        .object = directory, .type = ItemType::Dirent, .offset = ours_btreefs::dirent_offset(name)};
    auto const looked = ours_btreefs::lookup(device(), m_volume, key, m_item);
    if (looked != Status::Ok) {
        return ustl::err(looked);
    }
    auto const decoded = ours_btreefs::decode_dirent_value(m_item);
    if (decoded.is_err() || decoded->name != name) {
        // A hash collision refused at insert time keeps this a dead end, not
        // a wrong entry.
        return ustl::err(Status::NotFound);
    }
    return decoded;
}

/// Walks |path| from |directory|, one component at a time: each name's
/// DIRENT must name a directory but the path's last, whose object and kind
/// are the answer. NotFound names what stopped the walk.
auto Filesystem::resolve(u64 const directory, std::string_view const path)
    -> ustl::Result<std::pair<u64, ObjectKind>, Status> {
    auto object = directory;
    auto rest = path;
    while (!rest.empty()) {
        auto const slash = rest.find('/');
        auto const component =
            rest.substr(0, slash == std::string_view::npos ? rest.size() : slash);
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
        auto const last = rest.empty();
        auto const found = find_child(object, component);
        if (found.is_err()) {
            return ustl::err(found.unwrap_err());
        }
        if (!last && found->kind != ObjectKind::Directory) {
            return ustl::err(Status::NotDir);
        }
        object = found->child;
        if (last) {
            return ustl::ok(std::pair{object, found->kind});
        }
    }
    return ustl::ok(std::pair{object, ObjectKind::Directory});
}

/// Makes an object of |kind| named |name| under |directory|: a DIRENT item
/// for it there, an INODE item of size 0 for the object. AlreadyExists when
/// the name is taken.
auto Filesystem::create_object(u64 const directory, std::string_view const name,
                               ObjectKind const kind) -> ustl::Result<u64, Status> {
    // One transaction: the DIRENT item and the INODE item land together or
    // not at all, the commit swapping the root once.
    auto &tx = tx_begin();
    auto object = tx.alloc_object();
    auto value = ours_btreefs::Value{};
    if (auto const status = ours_btreefs::encode_dirent_value(value, object, kind, name);
        status != Status::Ok) {
        return ustl::err(status);
    }
    // The dirent first: a name taken answers AlreadyExists here, before any
    // object id was made to matter.
    auto const inserted = ours_btreefs::insert(tx,
                                               {.object = directory,
                                                .type = ItemType::Dirent,
                                                .offset = ours_btreefs::dirent_offset(name)},
                                               value);
    if (inserted != Status::Ok) {
        return ustl::err(inserted);
    }
    // An object whose INODE never landed is a dirent to nothing; the
    // transaction closes that window -- both items commit or neither does.
    ours_btreefs::encode_inode_value(value, 0, kind);
    if (auto const status = ours_btreefs::insert(
            tx, {.object = object, .type = ItemType::Inode, .offset = 0}, value);
        status != Status::Ok) {
        return ustl::err(status);
    }
    if (auto const status = tx.commit(); status != Status::Ok) {
        return ustl::err(status);
    }
    return ustl::ok(object);
}

/// Read on the file connection |index|: the file's DATA items name its
/// blocks, a block with no item yet reading as zeros.
auto Filesystem::read_file(Connection &connection, ours_io::ReadRequest const &request,
                           std::span<u8> reply_bytes) -> void {
    auto const metadata = inode_of(connection.object);
    if (metadata.is_err()) {
        answer_status(connection.channel, ours_io::READ, request.txid, metadata.unwrap_err());
        return;
    }
    auto const offset = std::min<u64>(connection.seek, metadata->size);
    auto const size = std::min<u64>(std::min<u64>(request.count, metadata->size - offset),
                                    ours_io::MAX_TRANSFER_SIZE);
    auto const payload = reply_bytes.subspan(wire::STATUS_FORM_SIZE).first(size);
    auto done = usize{0};
    while (done < size) {
        auto const at = offset + done;
        auto const index = at / ours_btreefs::BLOCK_BYTES;
        auto const within = usize{at % ours_btreefs::BLOCK_BYTES};
        auto const chunk = std::min<usize>(ours_btreefs::BLOCK_BYTES - within, size - done);
        auto const looked = ours_btreefs::lookup(
            device(), m_volume,
            {.object = connection.object, .type = ItemType::Data, .offset = index}, m_item);
        if (looked != Status::Ok && looked != Status::NotFound) {
            answer_status(connection.channel, ours_io::READ, request.txid, looked);
            return;
        }
        if (looked == Status::NotFound) {
            // A hole: the bytes were never written.
            std::memset(payload.data() + done, 0, chunk);
        } else {
            if (auto const status = device().read_block(ours_btreefs::decode_data_value(m_item),
                                                        std::span{m_block});
                status != Status::Ok) {
                answer_status(connection.channel, ours_io::READ, request.txid, status);
                return;
            }
            std::memcpy(payload.data() + done, m_block.data() + within, chunk);
        }
        done += chunk;
    }
    connection.seek = offset + size;
    auto const encoded = ours_io::encode_read_reply(reply_bytes, request.txid,
                                                    {.status = Status::Ok, .data = payload});
    if (encoded.is_ok()) {
        auto _ = write(connection.channel, encoded.unwrap());
    }
}

/// Write on the file connection |index|: each 4 KiB of the file the write
/// touches gets its DATA item -- made, with a fresh block, when it is the
/// first -- and its block patched and written back whole.
auto Filesystem::write_file(Connection &connection, ours_io::WriteRequest const &request,
                            std::span<u8> reply_bytes) -> void {
    auto const metadata = inode_of(connection.object);
    auto &tx = tx_begin();
    if (metadata.is_err()) {
        answer_status(connection.channel, ours_io::WRITE, request.txid, metadata.unwrap_err());
        return;
    }
    auto const offset = connection.seek;
    auto const size = request.data.size();
    auto done = usize{0};
    while (done < size) {
        auto const at = offset + done;
        auto const index = at / ours_btreefs::BLOCK_BYTES;
        auto const within = usize{at % ours_btreefs::BLOCK_BYTES};
        auto const chunk = std::min<usize>(ours_btreefs::BLOCK_BYTES - within, size - done);
        auto looked = ours_btreefs::lookup(
            device(), m_volume,
            {.object = connection.object, .type = ItemType::Data, .offset = index}, m_item);
        if (looked != Status::Ok && looked != Status::NotFound) {
            answer_status(connection.channel, ours_io::WRITE, request.txid, looked);
            return;
        }
        if (looked == Status::NotFound) {
            auto const block = tx.alloc_block();
            if (block.is_err()) {
                answer_status(connection.channel, ours_io::WRITE, request.txid, block.unwrap_err());
                return;
            }
            ours_btreefs::encode_data_value(m_item, block.unwrap());
            auto const inserted = ours_btreefs::insert(
                tx, {.object = connection.object, .type = ItemType::Data, .offset = index}, m_item);
            if (inserted != Status::Ok) {
                answer_status(connection.channel, ours_io::WRITE, request.txid, inserted);
                return;
            }
        }
        // A block written whole skips the read; a patch starts from what is
        // there, so the bytes outside the write stay the file's -- and from
        // zeros for a block just made, which holds on the device whatever it
        // held before.
        if (within != 0 || chunk != ours_btreefs::BLOCK_BYTES) {
            if (auto const status = ours_btreefs::data_block_to_patch(
                    device(), ours_btreefs::decode_data_value(m_item), Status::NotFound == looked,
                    std::span{m_block});
                status != Status::Ok) {
                answer_status(connection.channel, ours_io::WRITE, request.txid, status);
                return;
            }
        }
        std::memcpy(m_block.data() + within, request.data.data() + done, chunk);
        if (auto const status =
                device().write_block(ours_btreefs::decode_data_value(m_item), std::span{m_block});
            status != Status::Ok) {
            answer_status(connection.channel, ours_io::WRITE, request.txid, status);
            return;
        }
        done += chunk;
    }

    // A write past the size grows it: the INODE item's value is replaced
    // where it lies, nothing else about the object changing.
    auto const end = offset + size;
    if (end > metadata->size) {
        auto value = ours_btreefs::Value{};
        ours_btreefs::encode_inode_value(value, end, ObjectKind::File);
        if (auto const status = ours_btreefs::update(
                tx, {.object = connection.object, .type = ItemType::Inode, .offset = 0}, value);
            status != Status::Ok) {
            answer_status(connection.channel, ours_io::WRITE, request.txid, status);
            return;
        }
    }
    if (auto const status = tx.commit(); status != Status::Ok) {
        answer_status(connection.channel, ours_io::WRITE, request.txid, status);
        return;
    }
    connection.seek = end;
    auto const encoded = ours_io::encode_write_reply(
        reply_bytes, request.txid, {.status = Status::Ok, .actual = static_cast<u32>(size)});
    if (encoded.is_ok()) {
        auto _ = write(connection.channel, encoded.unwrap());
    }
}

/// ReadDirents on the root connection: "." first, then the root's DIRENT
/// items along the leaves' chain, in name-hash order, the listing resuming
/// after the last entry that fit.
auto Filesystem::read_dirents(Connection &connection, ours_io::ReadDirentsRequest const &request,
                              std::span<u8> reply_bytes) -> void {
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
    auto const from = ours_btreefs::Key{
        .object = connection.object, .type = ItemType::Dirent, .offset = connection.next_offset};
    auto const walked = ours_btreefs::scan(
        device(), m_volume, from,
        [&](ours_btreefs::Key const &key, ours_btreefs::Value const &value) -> bool {
            // The listing resumes after what it named -- the entry it stopped
            // on is in the cursor already -- and names this directory's
            // children only: the scan walks the one tree past this
            // directory's key range, and other objects' items are not
            // entries here.
            // The cursor names the last entry listed, and the listing
            // resumes strictly after it -- hence the strict <, and the +1
            // on advance: an entry whose folded offset is exactly the
            // cursor must never list again, and one whose folded offset is
            // 0 must not be swallowed by the initial cursor of 0 (which is
            // why the skip is strict, not <=).
            if (key.object != connection.object || key.offset < connection.next_offset) {
                return true;
            }
            auto const decoded = ours_btreefs::decode_dirent_value(value);
            if (decoded.is_err()) {
                return true;
            }
            auto const type = decoded->kind == ObjectKind::Directory
                                  ? ours_io::DirentType::Directory
                                  : ours_io::DirentType::File;
            if (writer.append({.ino = ours_io::INO_UNKNOWN, .type = type, .name = decoded->name}) !=
                Status::Ok) {
                // It did not fit: it comes again on the next call, the
                // cursor not having moved.
                return false;
            }
            connection.next_offset = key.offset + 1;
            return true;
        });
    if (walked != Status::Ok) {
        answer_status(connection.channel, ours_io::READ_DIRENTS, request.txid, walked);
        return;
    }
    auto const encoded = ours_io::encode_read_dirents_reply(
        reply_bytes, request.txid, {.status = Status::Ok, .dirents = writer.written()});
    if (encoded.is_ok()) {
        auto _ = write(connection.channel, encoded.unwrap());
    }
}

/// Binds |open|'s server end to a connection of its own: the root when
/// |object| is 0, the file at that object otherwise.
auto open_below_dir_bind(ours_io::OpenRequest const &open, Operations const requested,
                         u64 const object, bool const directory) -> void {
    auto &storage = *STORAGE;
    auto const made = allocate(open.object, requested);
    if (made == MAX_CONNECTIONS) {
        refuse(open.object, Status::NoResource);
        return;
    }
    auto &made_connection = storage.connections[made];
    made_connection.target = directory ? Target::Directory : Target::File;
    made_connection.object = object;
    {
        char t[10];
        char u[10];
        auto _ = ort::debug_line({"btreefs: BIND obj ", ort::decimal(static_cast<u32>(object), t),
                                  " dir ", ort::decimal(static_cast<u32>(directory), u)});
    }
    if (watch(open.object, made) != Status::Ok) {
        auto _ = ort::debug_line("btreefs: OPEN bind watch FAILED");
        refuse(open.object, Status::NoResource);
    }
}

/// Open on a directory connection: walk what |open| names from it, or make
/// it, and bind its server end to a connection of its own. The checks are
/// ioservice's, in its order.
auto open_below_dir(Connection &connection, ours_io::OpenRequest const &open) -> void {
    auto const flags = open.flags;
    if (!ioservice::is_valid_path(open.path)) {
        refuse(open.object, Status::BadPath);
        return;
    }
    auto const at_self = open.path == "." || open.path.empty();
    auto const at_root = connection.object == ROOT_OBJECT && (at_self || open.path == EXPOSED_NAME);
    {
        char f[18];
        auto _ = ort::debug_line({"btreefs: DIAG rights ",
                                  ort::hex(std::to_underlying(connection.rights), f), " flags ",
                                  ort::hex(std::to_underlying(flags), f), " path ", open.path});
    }
    {
        char f[18];
        auto _ = ort::debug_line({"btreefs: DIAG open flags ",
                                  ort::hex(std::to_underlying(flags), f), " path ", open.path});
    }
    if (!at_self && !at_root && !holds(connection.rights, Operations::Traverse)) {
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

    // "." is this directory and the exposed name the volume's root; the
    // rest of the path walks from where it lands -- the components before
    // the last must be directories (NotFound or NotDir for what isn't),
    // and the last is the one a creation may make, ProtocolDirectory
    // saying which kind.
    auto start = connection.object;
    if (open.path == EXPOSED_NAME) {
        start = ROOT_OBJECT;
    }
    auto const slash = open.path.rfind('/');
    auto const parent_path =
        slash == std::string_view::npos ? std::string_view{} : open.path.substr(0, slash);
    auto const name = slash == std::string_view::npos ? open.path : open.path.substr(slash + 1);
    auto parent = start;
    if (!parent_path.empty()) {
        auto const walked = FILESYSTEM->resolve(start, parent_path);
        if (walked.is_err()) {
            refuse(open.object, walked.unwrap_err());
            return;
        }
        parent = walked->first;
    }
    if (at_self || at_root || name == "." || name.empty()) {
        // The walk's own end: this directory, as "." always was -- and a
        // root connection's open of the exposed name is the root itself,
        // the name its mount point answers to, not a child's.
        if ((flags & Flags::FlagMustCreate) != Flags::None) {
            refuse(open.object, Status::AlreadyExists);
            return;
        }
        return open_below_dir_bind(open, requested, parent, true);
    }
    auto const found = FILESYSTEM->find_child(parent, name);
    if (found.is_ok()) {
        if ((flags & Flags::FlagMustCreate) != Flags::None) {
            refuse(open.object, Status::AlreadyExists);
            return;
        }
        auto const is_dir = found->kind == ObjectKind::Directory;
        if (!is_dir && (flags & Flags::FileTruncate) != Flags::None) {
            // Truncating destroys bytes, and costs the right that names
            // writing them, in the open's own ask: the flag is not one of
            // Operations, so an ask of r* | FileTruncate requests no Write
            // right at all, and this gate refuses it the way Zircon's
            // flag-to-right table does. (The connection's ceiling is the
            // wrong place: an rw* view holds WriteBytes whatever the client
            // meant.)
            if (!holds(requested, Operations::WriteBytes)) {
                refuse(open.object, Status::AccessDenied);
                return;
            }
            // O_TRUNC: the file opens empty. The blocks go back inside one
            // transaction with the size change, so a crash leaves the file
            // either whole or empty, never half.
            if (auto const status = FILESYSTEM->truncate_file(found->child); status != Status::Ok) {
                refuse(open.object, status);
                return;
            }
        }
        return open_below_dir_bind(open, requested, found->child, is_dir);
    }
    if (found.unwrap_err() != Status::NotFound) {
        refuse(open.object, found.unwrap_err());
        return;
    }
    if ((flags & CREATION_FLAGS) == Flags::None) {
        refuse(open.object, Status::NotFound);
        return;
    }
    auto const kind = (flags & Flags::ProtocolDirectory) != Flags::None ? ObjectKind::Directory
                                                                        : ObjectKind::File;
    auto const made = FILESYSTEM->create_object(parent, name, kind);
    if (made.is_err()) {
        refuse(open.object, made.unwrap_err());
        return;
    }
    return open_below_dir_bind(open, requested, made.unwrap(), kind == ObjectKind::Directory);
}

/// Unlinks the entry |name| under |directory|: a file's DATA items go
/// first -- each block back to the bitmap as its item leaves, a crash in
/// the middle leaking a block but never dangling a name -- then the INODE,
/// and the DIRENT last, the name gone only when everything under it is. A
/// directory must be empty (NotEmpty), carrying nothing but its INODE.
auto Filesystem::unlink_at(u64 const directory, std::string_view const name) -> Status {
    auto const found = find_child(directory, name);
    if (found.is_err()) {
        return found.unwrap_err();
    }
    auto const object = found->child;
    auto &tx = tx_begin();
    if (found->kind == ObjectKind::Directory) {
        // An empty directory: no DIRENT item of its own, whatever its INODE
        // says. The walk stops at the next object's items.
        auto empty = true;
        auto const walked = ours_btreefs::scan(
            tx, tx.volume(), {.object = object, .type = ItemType::Dirent, .offset = 0},
            [&](ours_btreefs::Key const &key, ours_btreefs::Value const &) -> bool {
                if (key.object != object) {
                    return false;
                }
                if (key.type == ItemType::Dirent) {
                    empty = false;
                }
                return key.type == ItemType::Inode;
            });
        if (walked != Status::Ok) {
            return walked;
        }
        if (!empty) {
            return Status::NotEmpty;
        }
    }
    while (found->kind == ObjectKind::File) {
        // Each turn's copies go to the device before the next turn fills the
        // cache again: an unbounded loop over one transaction otherwise
        // exhausts it partway through a big file (docs/repairs/261103/btreefs.md).
        if (auto const status = tx.checkpoint(); status != ours::Status::Ok) {
            return status;
        }
        // The file's first DATA item, one per turn: the walk starts at the
        // file's first block, skips the INODE item that sorts before the
        // DATA ones, and stops at the next object's items.
        auto block_key = ours_btreefs::Key{};
        auto seen = false;
        auto const walked = ours_btreefs::scan(
            tx, tx.volume(), {.object = object, .type = ItemType::Data, .offset = 0},
            [&](ours_btreefs::Key const &key, ours_btreefs::Value const &) -> bool {
                if (key.object != object) {
                    return false;
                }
                if (key.type != ItemType::Data) {
                    return true;
                }
                block_key = key;
                seen = true;
                return false;
            });
        if (walked != Status::Ok) {
            return walked;
        }
        if (!seen) {
            break;
        }
        auto item = ours_btreefs::Value{};
        if (auto const status = ours_btreefs::lookup(tx, tx.volume(), block_key, item);
            status != Status::Ok) {
            return status;
        }
        if (auto const status = ours_btreefs::remove(tx, block_key); status != Status::Ok) {
            return status;
        }
        if (auto const status = tx.free_block(ours_btreefs::decode_data_value(item));
            status != Status::Ok) {
            return status;
        }
    }
    // The INODE goes next, the DIRENT last: the name survives until nothing
    // is left under it.
    if (auto const status =
            ours_btreefs::remove(tx, {.object = object, .type = ItemType::Inode, .offset = 0});
        status != Status::Ok) {
        return status;
    }
    if (auto const status = ours_btreefs::remove(tx, {.object = directory,
                                                      .type = ItemType::Dirent,
                                                      .offset = ours_btreefs::dirent_offset(name)});
        status != Status::Ok) {
        return status;
    }
    return tx.commit();
}

/// Serves one message that arrived on the connection |index|'s channel: the
/// dispatch, one message per wait, as ioservice's Server::dispatch is. The
/// replies go through the shared reply buffer, which one callback at a time
/// fills and drains.
/// Empties the file |object|: every DATA item's block back, the INODE's
/// size to zero, one transaction -- a crash leaves the file whole or empty.
auto Filesystem::truncate_file(u64 const object) -> Status {
    auto &tx = tx_begin();
    if (auto const status = ours_btreefs::truncate(tx, object, 0); status != Status::Ok) {
        return status;
    }
    auto value = ours_btreefs::Value{};
    ours_btreefs::encode_inode_value(value, 0, ObjectKind::File);
    if (auto const status = ours_btreefs::update(
            tx, {.object = object, .type = ItemType::Inode, .offset = 0}, value);
        status != Status::Ok) {
        return status;
    }
    return tx.commit();
}

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

    // TEMPORARY: the dispatch's witness.

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
        made_connection.object = connection.object;
        if (watch(request->request, made) != Status::Ok) {
            refuse(request->request, Status::NoResource);
        }
        return loop::Action::Rearm;
    }

    // Directory.Open, on the root only.
    if (ordinal == ours_io::OPEN.ordinal) {
        if (connection.target != Target::Directory) {
            forget(index);
            return loop::Action::Close;
        }
        auto const request = ours_io::decode_open(message);
        if (request.is_err()) {
            forget(index);
            return loop::Action::Close;
        }
        open_below_dir(connection, request.unwrap());
        return loop::Action::Rearm;
    }

    // Directory.ReadDirents, on the root only.
    if (ordinal == ours_io::READ_DIRENTS.ordinal) {
        auto const request = ours_io::decode_read_dirents(message);
        if (request.is_err() || connection.target != Target::Directory) {
            forget(index);
            return loop::Action::Close;
        }
        FILESYSTEM->read_dirents(connection, request.unwrap(), storage.reply_bytes);
        return loop::Action::Rearm;
    }

    // Directory.Unlink, on the root only: the file leaves the volume.
    if (ordinal == ours_io::UNLINK.ordinal) {
        auto const request = ours_io::decode_unlink(message);
        if (request.is_err() || connection.target != Target::Directory) {
            forget(index);
            return loop::Action::Close;
        }
        // Unlinking mutates the directory's contents, and costs the same
        // right creating in it does.
        if (!holds(connection.rights, Operations::ModifyDirectory)) {
            answer_status(connection.channel, ours_io::UNLINK, request->txid, Status::AccessDenied);
            return loop::Action::Rearm;
        }
        // The path is relative to this directory: its components walk, the
        // last names what goes.
        auto const slash = request->path.rfind('/');
        auto const parent_path =
            slash == std::string_view::npos ? std::string_view{} : request->path.substr(0, slash);
        auto const name =
            slash == std::string_view::npos ? request->path : request->path.substr(slash + 1);
        auto parent = connection.object;
        if (!parent_path.empty()) {
            auto const walked = FILESYSTEM->resolve(parent, parent_path);
            if (walked.is_err()) {
                answer_status(connection.channel, ours_io::UNLINK, request->txid,
                              walked.unwrap_err());
                return loop::Action::Rearm;
            }
            parent = walked->first;
        }
        auto const status = FILESYSTEM->unlink_at(parent, name);
        answer_status(connection.channel, ours_io::UNLINK, request->txid, status);
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
            FILESYSTEM->read_file(connection, request.unwrap(), storage.reply_bytes);
        } else {
            auto const request = ours_io::decode_write(message);
            if (request.is_err()) {
                forget(index);
                return loop::Action::Close;
            }
            FILESYSTEM->write_file(connection, request.unwrap(), storage.reply_bytes);
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

/// The disk's ours-fs partition: the volume is opened on it or laid on it
/// fresh, and the root directory is made when the volume is empty. Answers
/// false when there is no disk to serve, which is an answer and not a
/// failure.
auto Filesystem::mount() -> bool {
    // The partition table, through the client: a Client is a Reader.
    auto partitions = std::array<ours_gpt::Partition, 16>{};
    auto const table = ours_gpt::read(m_client, 512, std::span{partitions});
    if (table.is_err()) {
        auto _ =
            ort::debug_line({"btreefs: no partition table: ", ours::to_string(table.unwrap_err())});
        return false;
    }
    auto const partition = std::ranges::find_if(
        partitions.begin(), partitions.begin() + table.unwrap(),
        [](auto const &candidate) { return candidate.type.bytes == ours_gpt::OURS_FS_TYPE_BYTES; });
    if (partition == partitions.begin() + table.unwrap()) {
        auto _ = ort::debug_line("btreefs: no ours-fs partition on the disk");
        return false;
    }
    auto const first_lba = partition->first_lba;
    // The device the volume's operations run through, from here on, and
    // the transaction storage beside it.
    m_adapter.init(m_client, first_lba);
    m_tx.init(*m_adapter, m_volume);
    auto const blocks =
        (partition->last_lba - partition->first_lba + 1) * 512 / ours_btreefs::BLOCK_BYTES;
    auto const bitmap_blocks = static_cast<u32>((blocks + 8 * ours_btreefs::BLOCK_BYTES - 1) /
                                                (8 * ours_btreefs::BLOCK_BYTES));

    auto opened = ours_btreefs::open(device());
    if (opened.is_err() && opened.unwrap_err() == Status::NotFound) {
        // A blank partition is laid out fresh; the root directory is made
        // below, where either path meets.
        if (auto const status = ours_btreefs::format(device(), blocks, bitmap_blocks);
            status != Status::Ok) {
            auto _ = ort::debug_line({"btreefs: cannot format: ", ours::to_string(status)});
            return false;
        }
        opened = ours_btreefs::open(device());
    }
    if (opened.is_err()) {
        auto _ = ort::debug_line(
            {"btreefs: cannot open the volume: ", ours::to_string(opened.unwrap_err())});
        return false;
    }
    m_volume = opened.unwrap();
    if (m_volume.height == 0) {
        auto &tx = tx_begin();
        auto const root = tx.alloc_object();
        auto value = ours_btreefs::Value{};
        ours_btreefs::encode_inode_value(value, 0, ObjectKind::Directory);
        if (auto const status = ours_btreefs::insert(
                tx, {.object = root, .type = ItemType::Inode, .offset = 0}, value);
            status != Status::Ok) {
            auto _ = ort::debug_line({"btreefs: cannot make the root: ", ours::to_string(status)});
            return false;
        }
        if (auto const status = tx.commit(); status != Status::Ok) {
            auto _ = ort::debug_line({"btreefs: cannot make the root: ", ours::to_string(status)});
            return false;
        }
    }
    return true;
}
} // namespace

auto main(int, char **) -> int {
    auto const directory_request = ort::take_startup_handle(ours::HandleType::DirectoryRequest, 0);
    if (directory_request == Uandle::Invalid) {
        auto _ = ort::debug_line("btreefs: started without a DirectoryRequest handle");
        return 1;
    }

    STORAGE.init();

    // The block service through the one route the manifest states; the
    // client owns the channel from here on.
    auto client_end = Uandle::Invalid;
    auto server_end = Uandle::Invalid;
    if (auto const status =
            ours::channel_create(ours::ChannelCreateOptions::None, &client_end, &server_end);
        status != Status::Ok) {
        auto _ = ort::debug_line({"btreefs: cannot make its channel: ", ours::to_string(status)});
        auto _ = ours::handle_close(directory_request);
        return 1;
    }
    if (auto const status = fsio::connect("/svc/ours.block.Block", server_end);
        status != Status::Ok) {
        auto _ = ort::debug_line({"btreefs: no block service: ", ours::to_string(status)});
        auto _ = ours::handle_close(client_end);
        auto _ = ours::handle_close(directory_request);
        return 0;
    }
    auto &filesystem = *FILESYSTEM.init(*CLIENT.init(client_end));
    if (!filesystem.mount()) {
        // No disk, or no partition on it: an answer, not a failure. The
        // routes to this directory answer NotFound while it is gone.
        auto _ = ort::debug_line("btreefs: no volume to serve");
        auto _ = ours::handle_close(directory_request);
        return 0;
    }

    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        auto _ = ort::debug_line({"btreefs: cannot make its port: ", ours::to_string(status)});
        auto _ = ours::handle_close(directory_request);
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
    LOOP = &loop;

    // The outgoing directory: the root of the volume, as writable as the
    // volume is.
    auto const root = allocate(directory_request, SERVED_RIGHTS);
    if (root == MAX_CONNECTIONS) {
        auto _ = ort::debug_line("btreefs: no row for its outgoing directory");
        auto _ = ours::handle_close(directory_request);
        return 1;
    }
    STORAGE->connections[root].target = Target::Directory;
    STORAGE->connections[root].object = ROOT_OBJECT;
    if (auto const armed = watch(directory_request, root); armed != Status::Ok) {
        auto _ = ort::debug_line(
            {"btreefs: cannot wait on its outgoing directory: ", ours::to_string(armed)});
        auto _ = ours::handle_close(directory_request);
        return 1;
    }

    // Stop ends the program from inside an on_signals(); run() comes back
    // only when the port itself fails.
    auto _ = ort::debug_line({"btreefs: serving the volume at /", EXPOSED_NAME});
    auto _ = ort::debug_line(
        {"btreefs: its Loop stopped: ", ours::to_string(loop.run(ours::INSTANT_INFINITE))});
    return 1;
}
