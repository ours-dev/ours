/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// device-registry -- the device tree (the device-namespace round,
/// .agents/tasks/device-namespace/DESIGN.md): one directory where every
/// device answers, whatever driver drives it. A client enumerates the tree
/// and opens any leaf; the open is forwarded into the directory the device's
/// driver published, and the channel that comes back speaks the device's
/// protocol. The leaves sit at class/<class>/<name> -- class/block/000,
/// class/block/001, ... -- publish order, not driver identity, naming the
/// nodes; the top level outside class/ is unallocated ground no publisher
/// can claim.
///
/// The objects:
///   - the outgoing directory, ioservice::Outgoing, served on the
///     DirectoryRequest handle the root hands every component with a program,
///     holding svc/ours.dev.Registry and the Subtree node dev/;
///   - the registry protocol, RegistryServer: the publisher side, one
///     Publish per device, each answered with the name the leaf was granted;
///   - the dev tree, ioservice::Tree: the root this program serves at dev/,
///     its class/ directory, a class directory per published kind and a
///     service node per published device;
///   - the mounts: one per published device, the strings the tree's nodes
///     view, the directory handle the forward goes into, and the scratch the
///     forward's Open is built in. A driver's death closes its handle, and
///     the leaf's next forward then reads the peer gone.
///
/// The events: a driver connects to ours.dev.Registry and sends
/// Publish(class, name, protocol, directory); the registry grants the name
/// -- the proposal, or the class's lowest free three-digit number when it
/// was taken -- adds the service node and answers the granted name. A
/// client's Open of dev/ comes to DevSubtree::open(), which binds the
/// channel to the node the rest of the path names, with the rights the Open
/// asks for; from then on the client's Opens of the leaves are the tree
/// server's to walk. An Open of a leaf reaches a service node and comes to
/// the mount's handler, which writes Open("svc/<protocol>") on the published
/// directory, whose server -- the driver's own outgoing -- hands the channel
/// to the protocol's server, exactly as an Open of any /svc entry is handed
/// on.
#include <ours/port.hpp>
#include <ours/process_args.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>
#include <new>
#include <span>
#include <string_view>
#include <utility>

#include <cstring>

#include <ddk/ddk.hpp>
#include <ioservice/outgoing.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <prot/dev.hpp>
#include <prot/io.hpp>
#include <server/server.hpp>
#include <ustl/lazy_init.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;

/// How many drivers may hold a registry connection at once. A driver past
/// them is refused with NoResource.
constexpr auto MAX_CLIENTS = usize{8};

/// How many devices may be mounted at once: the mounts and the service nodes
/// that name them are one pool, one slot each.
constexpr auto MAX_MOUNTS = usize{8};

/// How many connections the dev tree serves at once: one per consumer the
/// root routed the directory capability to, whole or through a use's subdir.
constexpr auto MAX_TREE_CONNECTIONS = usize{8};

/// The dev tree's nodes: the root, class/, the class directories and a
/// service node per mount. Three plus the mounts leave five over for class
/// directories the mounts make on demand; every add answers NoResource past
/// them rather than overflowing.
constexpr auto TREE_NODES = usize{16};
static_assert(TREE_NODES >= 3 + MAX_MOUNTS);

/// Where the dev tree hangs in the outgoing directory: the path the
/// manifest's declaration states.
constexpr auto DEV_PATH = std::string_view{"/dev"};

/// The path of a leaf's forward, inside the publisher's directory: its
/// protocols live under svc/, as any component's do.
constexpr auto FORWARD_PREFIX = std::string_view{"svc/"};

/// The Loop's slots: one per connection to the outgoing directory, one per
/// registry client, one per dev-tree connection.
constexpr auto SLOTS = ioservice::Outgoing::MAX_CONNECTIONS + MAX_CLIENTS + MAX_TREE_CONNECTIONS;

// Each slot in use holds one live wait on the port (<loop/loop.hpp>).
static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS);

/// Says |what| failed with |status|, on the debug console.
auto report(std::string_view const what, Status const status) -> void {
    auto _ = ort::debug_line({"device-registry: ", what, ": ", ours::to_string(status)});
}

/// Writes |message| on |channel|; what channel_write answers.
auto write(Uandle const channel, wire::Message const message) -> Status {
    return ours::channel_write(channel, ours::ChannelWriteOptions::None, message.bytes.data(),
                               static_cast<u32>(message.bytes.size()), message.handles.data(),
                               static_cast<u32>(message.handles.size()));
}

/// The rights an Open of the tree asks for, as the connection they become:
/// the permission bits of Flags share Operations' values, and the rest --
/// the inherit bits, which are not rights a connection can hold -- is
/// dropped, the io server reading a request's flags the same way.
constexpr auto to_operations(prot::io::Flags const flags) -> prot::io::Operations {
    return static_cast<prot::io::Operations>(
        std::to_underlying(flags) & (std::to_underlying(prot::io::MASK_KNOWN_PERMISSIONS) &
                                     ~(std::to_underlying(prot::io::Flags::PermInheritWrite) |
                                       std::to_underlying(prot::io::Flags::PermInheritExecute))));
}
static_assert(to_operations(prot::io::PERM_READABLE) == prot::io::R_STAR_DIR);

/// One mounted device: what the tree's nodes view, what the leaf's forward
/// needs, and where the forward goes. The views point into this mount's own
/// arrays, which are static storage and never move -- the pool is never
/// copied, and a mount, once taken, holds its slot for the program's life.
struct Mount {
    std::array<char, prot::dev::MAX_CLASS_LENGTH> klass_storage{};
    std::array<char, prot::dev::MAX_NODE_NAME> name_storage{};
    std::array<char, prot::dev::MAX_PROTOCOL_NAME> protocol_storage{};
    /// The forward's path, "svc/" plus the protocol, built once at mount
    /// time into the room both need.
    std::array<char, FORWARD_PREFIX.size() + prot::dev::MAX_PROTOCOL_NAME> forward_storage{};
    /// Where the leaf's forward is encoded: the Open's fixed fields (32
    /// bytes, through the path size at 28) plus the path. One of these per
    /// mount, so that no forward borrows a buffer another callback reads.
    static constexpr auto SCRATCH_SIZE =
        usize{32} + FORWARD_PREFIX.size() + prot::dev::MAX_PROTOCOL_NAME;
    std::array<u8, SCRATCH_SIZE> scratch{};
    std::string_view klass;
    std::string_view name;
    std::string_view protocol;
    std::string_view forward;
    /// The server end of the publisher's outgoing, or of the part of it the
    /// driver published. Invalid while the slot is free.
    Uandle directory = Uandle::Invalid;
    bool used = false;
};

constinit std::array<Mount, MAX_MOUNTS> MOUNTS{};

/// One mount's forward: what an Open of the mount's service node comes to.
/// The tree's delegate indexes the handlers by the value the node was added
/// with, the mount's index plus one.
class MountHandler final: public ioservice::Handler {
  public:
    constexpr MountHandler() = default;
    explicit MountHandler(Mount *target) : mount(target) {}

    /// The mount this handler forwards for, in place by publish time.
    Mount *mount = nullptr;

    /// Forwards |channel|, an Open of the leaf, into the publisher's
    /// directory as an Open of svc/<protocol>: the publisher's outgoing
    /// walks to its protocol's node and hands the channel to its server. The
    /// channel is consumed either way -- the Open carries it, and the kernel
    /// takes a message's handles whatever the write answers -- so only an
    /// Open that was never written leaves a handle to close.
    auto connect(Uandle const channel) -> void override {
        auto handles = std::array{channel};
        auto const encoded =
            prot::io::encode_open(mount->scratch, handles,
                                  prot::io::OpenRequest{.flags = prot::io::Flags::ProtocolService,
                                                        .path = mount->forward,
                                                        .object = channel});
        if (encoded.is_err()) {
            // The path is this mount's own, built to fit the scratch at
            // mount time, so this is the buffer's own bug.
            report("cannot encode a leaf's Open", encoded.unwrap_err());
            auto _ = ours::handle_close(channel);
            return;
        }
        if (auto const written = write(mount->directory, encoded.unwrap()); written != Status::Ok) {
            // The publisher is gone; the mount stays -- the tree's nodes are
            // never removed -- and its next Open answers the same way. The
            // client reads PEER_CLOSED on its entry.
            report("cannot forward a leaf's Open", written);
        }
    }
};

constinit std::array<MountHandler, MAX_MOUNTS> MOUNT_HANDLERS{};

/// Copies |text| into |storage| and returns it as a view of the copy: the
/// tree keeps a node's name as a view, and a request's bytes end with the
/// call that proposed them.
template <usize N>
auto stored(std::array<char, N> &storage, std::string_view const text) -> std::string_view {
    std::memcpy(storage.data(), text.data(), text.size());
    return {storage.data(), text.size()};
}

/// Whether a live mount of |klass| already holds |name|.
auto name_taken(std::string_view const klass, std::string_view const name) -> bool {
    for (auto const &mount: MOUNTS) {
        if (mount.used && mount.klass == klass && mount.name == name) {
            return true;
        }
    }
    return false;
}

/// Grants the mount its name: the proposal when no live mount of the class
/// holds it, otherwise the class's lowest free three-digit number ("000",
/// "001", ...), written into |mount|'s own storage -- the tree views it
/// from here on. False when no number is free, which the mount bound (eight
/// slots) reaches long before a thousand names do.
auto grant_name(Mount &mount, std::string_view const klass, std::string_view const proposal)
    -> bool {
    if (!name_taken(klass, proposal)) {
        mount.name = stored(mount.name_storage, proposal);
        return true;
    }
    for (auto value = u32{0}; value <= 999; ++value) {
        auto const number = std::array<char, 3>{{static_cast<char>('0' + value / 100),
                                                 static_cast<char>('0' + (value / 10) % 10),
                                                 static_cast<char>('0' + value % 10)}};
        auto const candidate = std::string_view{number.data(), number.size()};
        if (!name_taken(klass, candidate)) {
            mount.name = stored(mount.name_storage, candidate);
            return true;
        }
    }
    return false;
}

/// The registry protocol's server: the publisher side of the tree. One
/// Publish per device, answered inline -- the name granted, the node added,
/// the mount taken, the granted name answered -- on the protocol server
/// base's pump, one client per channel an Open of svc/ours.dev.Registry
/// brought.
class RegistryServer final: public server::ProtocolServer<RegistryServer, MAX_CLIENTS> {
    using Base = server::ProtocolServer<RegistryServer, MAX_CLIENTS>;

  public:
    using Storage = Base::Storage;

    /// A server with no client, keeping its state in |storage|, waiting on
    /// |loop|, and mounting into |tree| under |class_node|, the dev tree's
    /// class/ directory. All must outlive it.
    RegistryServer(Storage &storage, loop::Loop &loop, ioservice::Tree &tree,
                   ioservice::NodeId const class_node)
        : Base(storage, loop), m_tree(&tree), m_class(class_node) {}

    RegistryServer(RegistryServer const &) = delete;
    auto operator=(RegistryServer const &) -> RegistryServer & = delete;

    /// The pump's dispatch, which the base calls. A decoder refusal or an
    /// unknown ordinal closes the connection without an epitaph, the handles
    /// it carried closed with it, as <prot/dev.hpp>'s contract states.
    /// Publish is the one method and its directory handle the one handle:
    /// once a decode passes, the handle is this program's, and the message's
    /// handles are never closed wholesale again.
    auto on_message(Uandle const channel, wire::Header const header, wire::Message &&message)
        -> loop::Action {
        if (header.ordinal == prot::dev::PUBLISH.ordinal) {
            auto request = prot::dev::decode_publish(message);
            if (request.is_err()) {
                Base::close_handles(message);
                return Base::close_client(channel);
            }
            return publish(channel, *request);
        }
        Base::close_handles(message);
        return Base::close_client(channel);
    }

  private:
    /// Publish: the name granted, the class directory found or made, the
    /// node added, the directory taken into the mount, the granted name
    /// answered. Every refusal below closes the request's directory handle,
    /// which nothing took; a mounted publish keeps it.
    auto publish(Uandle const channel, prot::dev::PublishRequest const &request) -> loop::Action {
        auto index = usize{0};
        while (index < MAX_MOUNTS && MOUNTS[index].used) {
            index += 1;
        }
        if (index == MAX_MOUNTS) {
            auto _ = ours::handle_close(request.directory);
            return answer(channel, request.txid, {.status = Status::NoResource, .name = {}});
        }
        auto &mount = MOUNTS[index];
        if (!grant_name(mount, request.klass, request.name)) {
            auto _ = ours::handle_close(request.directory);
            return answer(channel, request.txid, {.status = Status::NoResource, .name = {}});
        }
        mount.klass = stored(mount.klass_storage, request.klass);
        mount.protocol = stored(mount.protocol_storage, request.protocol);
        std::memcpy(mount.forward_storage.data(), FORWARD_PREFIX.data(), FORWARD_PREFIX.size());
        std::memcpy(mount.forward_storage.data() + FORWARD_PREFIX.size(), mount.protocol.data(),
                    mount.protocol.size());
        mount.forward = std::string_view{mount.forward_storage.data(),
                                         FORWARD_PREFIX.size() + mount.protocol.size()};

        // The class directory, made on the first mount of its kind: block/
        // is laid out at startup, the rest grow here, named out of the
        // mount's own storage.
        auto class_dir = m_class;
        auto const found = m_tree->lookup(m_class, mount.klass);
        if (found.is_ok()) {
            class_dir = found.unwrap();
        } else {
            auto made = m_tree->add_directory(m_class, mount.klass);
            if (made.is_err()) {
                reclaim(mount);
                auto _ = ours::handle_close(request.directory);
                return answer(channel, request.txid, {.status = made.unwrap_err(), .name = {}});
            }
            class_dir = made.unwrap();
        }

        // The node views the mount's own strings, which outlive the request;
        // its value is the mount's index plus one, what the tree's delegate
        // indexes the handlers by.
        auto const node = m_tree->add_service(class_dir, mount.name, index + 1);
        if (node.is_err()) {
            // A class directory made above stays: a node, once added, is
            // never removed, and an empty class is harmless.
            reclaim(mount);
            auto _ = ours::handle_close(request.directory);
            return answer(channel, request.txid, {.status = node.unwrap_err(), .name = {}});
        }

        // Mounted: the directory is the mount's and the handler takes its
        // place in the pool, by the index the node was added with. A publish
        // mounted but unanswerable below keeps the mount -- the device
        // answers either way, and the attempt is spent.
        mount.directory = request.directory;
        mount.used = true;
        new (&MOUNT_HANDLERS[index]) MountHandler(&mount);
        auto _ = ort::debug_line(
            {"device-registry: ", mount.klass, "/", mount.name, " mounted from a publisher"});
        return answer(channel, request.txid, {.status = Status::Ok, .name = mount.name});
    }

    /// Answers |channel|'s Publish with |reply|: the granted name on Ok, the
    /// bare status form on a failure, the codec's two forms. A reply that
    /// cannot be built or written ends the connection.
    auto answer(Uandle const channel, u32 const txid, prot::dev::PublishReply const &reply)
        -> loop::Action {
        auto const encoded = prot::dev::encode_publish_reply(Base::reply_buffer(), txid, reply);
        if (encoded.is_err() || Base::reply(channel, encoded.unwrap()) != Status::Ok) {
            return Base::close_client(channel);
        }
        return loop::Action::Rearm;
    }

    /// Gives the mount |mount| back, a publish that never reached its node:
    /// its slot's strings are the next publish's to overwrite.
    static auto reclaim(Mount &mount) -> void {
        mount.directory = Uandle::Invalid;
        mount.used = false;
    }

    ioservice::Tree *m_tree;
    ioservice::NodeId m_class;
};

/// What the dev tree keeps, outside the stack (the note at the storages
/// below): the tree's nodes, one connection per consumer the root routed the
/// directory capability to, and the two message buffers, as large as a
/// channel message.
struct DevStorage {
    std::array<ioservice::Node, TREE_NODES> nodes;
    std::array<ioservice::Connection, MAX_TREE_CONNECTIONS> connections;
    std::array<ioservice::LoopServer::Binding, MAX_TREE_CONNECTIONS> bindings;
    std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
    ioservice::ReplyBuffer reply;
};

/// The dev tree and the program's side of the dev/ Subtree node: the tree
/// the mounts go into, served on the same Loop as everything else here, and
/// the handler the outgoing directory hands every Open of dev/ to. The
/// delegate base is where an Open of a leaf lands, a service node's channel
/// -- the two halves of the tree's work, on one object.
class DevSubtree final: public ioservice::SubtreeHandler, private ioservice::LoopServer::Delegate {
  public:
    /// A tree with nothing in it yet, whose state lives in |storage| and
    /// whose connections wait on |loop|. Both must outlive it.
    DevSubtree(DevStorage &storage, loop::Loop &loop)
        : m_storage(&storage)
        , m_tree(storage.nodes)
        , m_server(m_tree, loop, ioservice::Syscalls{.handle_duplicate = ours::handle_duplicate},
                   ioservice::LoopServer::Memory{
                       .connections = storage.connections,
                       .bindings = storage.bindings,
                       .request_bytes = storage.request_bytes,
                       .request_handles = storage.request_handles,
                       .reply = &storage.reply,
                   },
                   this) {}

    DevSubtree(DevSubtree const &) = delete;
    auto operator=(DevSubtree const &) -> DevSubtree & = delete;

    /// Lays the tree out: the root, class/ in it, and block/ in class/, the
    /// class the block drivers publish into. Every other class is made on
    /// demand by the mount that names it first. Ok, or what failed.
    [[nodiscard]]
    auto build() -> Status {
        auto root = m_tree.add_root();
        if (root.is_err()) {
            return root.unwrap_err();
        }
        m_root = root.unwrap();
        auto class_dir = m_tree.add_directory(m_root, "class");
        if (class_dir.is_err()) {
            return class_dir.unwrap_err();
        }
        m_class = class_dir.unwrap();
        auto block = m_tree.add_directory(m_class, "block");
        if (block.is_err()) {
            return block.unwrap_err();
        }
        m_block = block.unwrap();
        return Status::Ok;
    }

    /// The tree the registry's mounts go into, and the class/ directory the
    /// registry mounts under.
    auto tree() -> ioservice::Tree & {
        return m_tree;
    }

    [[nodiscard]]
    auto class_node() const -> ioservice::NodeId {
        return m_class;
    }

    /// An Open the outgoing directory routed to dev/: |rest| is the part of
    /// the path after the node, "." when the Open names the node itself, and
    /// |flags| the Open's own. The root's routing of the directory
    /// capability arrives here with "." (a consumer's namespace entry, the
    /// whole tree), or with "class" or "class/block" when its use names a
    /// subdir of it. Each binds the channel to the node it names, with the
    /// rights the flags ask for; from then on the consumer's Opens of the
    /// leaves are the tree server's to walk. Any other rest is a path no use
    /// of the capability can name: refused NotFound, the channel with it.
    auto open(std::string_view const rest, prot::io::Flags const flags, Uandle const channel)
        -> void override {
        if (rest == ".") {
            bind(channel, flags, m_root);
        } else if (rest == "class") {
            bind(channel, flags, m_class);
        } else if (rest == "class/block") {
            bind(channel, flags, m_block);
        } else {
            refuse(channel, Status::NotFound);
        }
    }

  private:
    /// Where an Open of a leaf lands: |service| is the value the service
    /// node was added with, the mount's index plus one, and |channel| the
    /// Open's, which the mount's handler takes -- forwarding it into the
    /// publisher's directory. A value that names no live mount closes the
    /// channel, which no client that walked the tree can bring about.
    auto connect(u64 const service, Uandle const channel) -> void override {
        if (service == 0 || service > MAX_MOUNTS || !MOUNTS[service - 1].used) {
            auto _ = ours::handle_close(channel);
            return;
        }
        MOUNT_HANDLERS[service - 1].connect(channel);
    }

    /// Serves |node| on |channel| with the rights |flags| ask for: the
    /// connection is the LoopServer's from here, its slot armed, and the
    /// channel with it. A bind the server refuses -- its connection table
    /// full, the likeliest -- is answered with the status and closed.
    auto bind(Uandle const channel, prot::io::Flags const flags, ioservice::NodeId const node)
        -> void {
        if (auto const served = m_server.serve(channel, node, to_operations(flags));
            served != Status::Ok) {
            refuse(channel, served);
        }
    }

    /// Closes |channel| with an epitaph carrying |status|: an Open this
    /// subtree does not take. The buffer is the dev tree server's own, and
    /// no callback of its runs while this one does -- one Loop, one callback
    /// at a time -- so borrowing it borrows nothing in use.
    auto refuse(Uandle const channel, Status const status) -> void {
        auto epitaph = wire::encode_epitaph(m_storage->reply.bytes, status);
        if (epitaph.is_ok()) {
            auto _ = write(channel, epitaph.unwrap());
        }
        auto _ = ours::handle_close(channel);
    }

    DevStorage *m_storage;
    ioservice::Tree m_tree;
    ioservice::LoopServer m_server;
    ioservice::NodeId m_root = ioservice::detail::NO_NODE;
    ioservice::NodeId m_class = ioservice::detail::NO_NODE;
    ioservice::NodeId m_block = ioservice::detail::NO_NODE;
};

// What the servers keep, outside the stack: constinit, because nothing runs a
// user program's dynamic initialisers. The storages hold types with
// destructors (ustl::Option, in the ioservice connections), which as a plain
// global would register them with __cxa_atexit, which the runtime does not
// have; ustl::LazyInit is raw storage that main() builds them in instead, as
// init's are (system/service/init/init.cpp).
constinit ustl::LazyInit<ioservice::Outgoing::Storage> OUTGOING_STORAGE;
constinit ustl::LazyInit<RegistryServer::Storage> REGISTRY_STORAGE;
constinit ustl::LazyInit<DevStorage> DEV_STORAGE;

// The Loop's slots, one per wait: the outgoing directory's connections, the
// registry's clients, the dev tree's.
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};
} // namespace

auto main(int, char **) -> int {
    // The server end of the outgoing directory, which the root made when it
    // resolved this component and hands every component with a program.
    auto const directory_request =
        ddk::startup_handle(ours::HandleType::DirectoryRequest, "a DirectoryRequest handle");
    if (directory_request.is_err()) {
        return 1;
    }

    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        report("cannot make its port", status);
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
    ioservice::Outgoing outgoing(*OUTGOING_STORAGE.init(), loop);
    DevSubtree dev(*DEV_STORAGE.init(), loop);
    if (auto const status = dev.build(); status != Status::Ok) {
        report("cannot lay its tree out", status);
        return 1;
    }
    RegistryServer registry(*REGISTRY_STORAGE.init(), loop, dev.tree(), dev.class_node());

    if (auto const status = outgoing.add_protocol(prot::dev::REGISTRY_NAME, registry);
        status != Status::Ok) {
        report("cannot add its protocol", status);
        return 1;
    }
    if (auto const status = outgoing.add_subtree(DEV_PATH, dev); status.is_err()) {
        report("cannot add its tree", status.unwrap_err());
        return 1;
    }
    if (auto const status = outgoing.serve(directory_request.unwrap()); status != Status::Ok) {
        report("cannot serve its outgoing directory", status);
        auto _ = ours::handle_close(directory_request.unwrap());
        return 1;
    }

    // run() comes back only when the port itself fails.
    auto _ = ort::debug_line("device-registry: serving the device tree");
    report("its Loop stopped", loop.run(ours::INSTANT_INFINITE));
    return 1;
}
