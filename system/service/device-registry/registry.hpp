/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef DEVICE_REGISTRY_REGISTRY_HPP
#define DEVICE_REGISTRY_REGISTRY_HPP 1

// The device tree's core, split from the component's main() so that the
// grant rule and the forwarding are host-testable (test/registry_test.cpp
// drives them over the fake kernel): the mounts, the grant rule, the
// registry protocol's server, and the dev tree's subtree handler. The
// component's main() lays the same objects out and serves them; nothing
// here knows a startup handle exists.

#include <ours/port.hpp>
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

#include <ioservice/outgoing.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <prot/dev.hpp>
#include <prot/io.hpp>
#include <server/server.hpp>
#include <wire/wire.hpp>

namespace device_registry {
using ours::Status;
using ours::Uandle;

/// How many drivers may hold a registry connection at once. A driver past
/// them is refused with NoResource.
inline constexpr auto MAX_CLIENTS = usize{8};

/// How many devices may be mounted at once: the mounts and the service nodes
/// that name them are one pool, one slot each.
inline constexpr auto MAX_MOUNTS = usize{8};

/// How many connections the dev tree serves at once: one per consumer the
/// root routed the directory capability to, whole or through a use's subdir.
inline constexpr auto MAX_TREE_CONNECTIONS = usize{8};

/// The dev tree's nodes: the root, class/, the class directories and a
/// service node per mount. Three plus the mounts leave five over for class
/// directories the mounts make on demand; every add answers NoResource past
/// them rather than overflowing.
inline constexpr auto TREE_NODES = usize{16};
static_assert(TREE_NODES >= 3 + MAX_MOUNTS);

/// The path of a leaf's forward, inside the publisher's directory: its
/// protocols live under svc/, as any component's do.
inline constexpr auto FORWARD_PREFIX = std::string_view{"svc/"};

/// Says |what| failed with |status|, on the debug console.
inline auto report(std::string_view const what, Status const status) -> void {
    auto _ = ort::debug_line({"device-registry: ", what, ": ", ours::to_string(status)});
}

/// Writes |message| on |channel|; what channel_write answers.
inline auto write(Uandle const channel, wire::Message const message) -> Status {
    return ours::channel_write(channel, ours::ChannelWriteOptions::None, message.bytes.data(),
                               static_cast<u32>(message.bytes.size()), message.handles.data(),
                               static_cast<u32>(message.handles.size()));
}

/// The rights an Open of the tree asks for, as the connection they become:
/// the permission bits of Flags share Operations' values, and the rest --
/// the inherit bits, which are not rights a connection can hold -- is
/// dropped, the io server reading a request's flags the same way.
inline constexpr auto to_operations(prot::io::Flags const flags) -> prot::io::Operations {
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
            // client reads PEER_CLOSED on its entry: the Open's handle went
            // with the refused write, consumed by the kernel either way.
            report("cannot forward a leaf's Open", written);
        }
    }
};

/// Copies |text| into |storage| and returns it as a view of the copy: the
/// tree keeps a node's name as a view, and a request's bytes end with the
/// call that proposed them.
template <usize N>
auto stored(std::array<char, N> &storage, std::string_view const text) -> std::string_view {
    std::memcpy(storage.data(), text.data(), text.size());
    return {storage.data(), text.size()};
}

/// The mounts and their handlers, one pool: what the registry server mounts
/// into and what the tree's delegate forwards through. A separate object so
/// that the two servers share one pool and the test can watch it.
class Core final {
  public:
    Core() = default;
    Core(Core const &) = delete;
    auto operator=(Core const &) -> Core & = delete;

    /// Whether a live mount of |klass| already holds |name|.
    [[nodiscard]]
    auto name_taken(std::string_view const klass, std::string_view const name) const -> bool {
        for (auto const &mount: m_mounts) {
            if (mount.used && mount.klass == klass && mount.name == name) {
                return true;
            }
        }
        return false;
    }

    /// The first free mount slot, or none.
    [[nodiscard]]
    auto free_slot() -> ustl::Option<usize> {
        for (auto index = usize{0}; index < MAX_MOUNTS; ++index) {
            if (!m_mounts[index].used) {
                return ustl::some(index);
            }
        }
        return ustl::NONE;
    }

    /// Grants the mount its name: the proposal when no live mount of the
    /// class holds it, otherwise the class's lowest free three-digit number
    /// ("000", "001", ...), written into |mount|'s own storage -- the tree
    /// views it from here on. Always grants: a free number exists whenever
    /// a free slot does, because eight mounts hold at most eight names and
    /// the scan reaches nine.
    auto grant_name(Mount &mount, std::string_view const klass, std::string_view const proposal)
        -> void {
        if (!name_taken(klass, proposal)) {
            mount.name = stored(mount.name_storage, proposal);
            return;
        }
        for (auto value = u32{0}; value < MAX_MOUNTS + 1; ++value) {
            auto const number = std::array<char, 3>{{static_cast<char>('0' + value / 100),
                                                     static_cast<char>('0' + (value / 10) % 10),
                                                     static_cast<char>('0' + value % 10)}};
            if (!name_taken(klass, std::string_view{number.data(), number.size()})) {
                mount.name = stored(mount.name_storage, std::string_view{number.data(), 3});
                return;
            }
        }
        // Structurally unreachable -- a free slot means a free number -- and
        // the next line says what a bug here would mount as.
        mount.name = stored(mount.name_storage, proposal);
    }

    /// The mount |index|'s handler, placed for a mount taken at |index|: the
    /// handler pool rides with the mount pool, placement-new'd at mount time
    /// (a polymorphic member the constinit storage leaves raw).
    auto place_handler(usize const index, Mount &mount) -> void {
        new (&m_handlers[index]) MountHandler(&mount);
    }

    /// Where an Open of a leaf lands: |service| is the value the service
    /// node was added with, the mount's index plus one. A value that names
    /// no live mount closes the channel, which no client that walked the
    /// tree can bring about.
    auto connect(u64 const service, Uandle const channel) -> void {
        if (service == 0 || service > MAX_MOUNTS || !m_mounts[service - 1].used) {
            auto _ = ours::handle_close(channel);
            return;
        }
        m_handlers[service - 1].connect(channel);
    }

    /// The mounts, for the tests to watch and the server to mount into.
    [[nodiscard]]
    auto mounts() -> std::array<Mount, MAX_MOUNTS> & {
        return m_mounts;
    }

  private:
    std::array<Mount, MAX_MOUNTS> m_mounts{};
    std::array<MountHandler, MAX_MOUNTS> m_handlers{};
};

/// What the dev tree keeps: the tree's nodes, one connection per consumer
/// the root routed the directory capability to, and the two message buffers,
/// as large as a channel message.
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
    /// whose leaves forward through |core|. Both must outlive it.
    DevSubtree(DevStorage &storage, loop::Loop &loop, Core &core)
        : m_storage(&storage)
        , m_core(&core)
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
    /// Where an Open of a leaf lands: the core decides by the node's value,
    /// the mount's index plus one.
    auto connect(u64 const service, Uandle const channel) -> void override {
        m_core->connect(service, channel);
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
    Core *m_core;
    ioservice::Tree m_tree;
    ioservice::LoopServer m_server;
    ioservice::NodeId m_root = ioservice::detail::NO_NODE;
    ioservice::NodeId m_class = ioservice::detail::NO_NODE;
    ioservice::NodeId m_block = ioservice::detail::NO_NODE;
};

/// The registry protocol's server: the publisher side of the tree. One
/// Publish per device, answered inline -- the name granted, the node added,
/// the mount taken, the granted name answered -- on the protocol server
/// base's pump, one client per channel an Open of svc/ours.dev.Registry
/// brought.
class RegistryServer final: public server::ProtocolServer<RegistryServer, MAX_CLIENTS> {
  public:
    using Storage = typename server::ProtocolServer<RegistryServer, MAX_CLIENTS>::Storage;

    /// A server with no client, keeping its state in |storage|, waiting on
    /// |loop|, mounting through |core| into |tree| under |class_node|, the
    /// dev tree's class/ directory. All must outlive it.
    RegistryServer(Storage &storage, loop::Loop &loop, Core &core, ioservice::Tree &tree,
                   ioservice::NodeId const class_node)
        : server::ProtocolServer<RegistryServer, MAX_CLIENTS>(storage, loop)
        , m_core(&core)
        , m_tree(&tree)
        , m_class(class_node) {}

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
    using Base = server::ProtocolServer<RegistryServer, MAX_CLIENTS>;

    /// Publish: the name granted, the class directory found or made, the
    /// node added, the directory taken into the mount, the granted name
    /// answered. Every refusal below closes the request's directory handle,
    /// which nothing took; a mounted publish keeps it.
    auto publish(Uandle const channel, prot::dev::PublishRequest const &request) -> loop::Action {
        auto const slot = m_core->free_slot();
        if (slot.is_none()) {
            auto _ = ours::handle_close(request.directory);
            return answer(channel, request.txid, {.status = Status::NoResource, .name = {}});
        }
        auto &mount = m_core->mounts()[*slot];
        m_core->grant_name(mount, request.klass, request.name);
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
        auto const node = m_tree->add_service(class_dir, mount.name, *slot + 1);
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
        m_core->place_handler(*slot, mount);
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

    Core *m_core;
    ioservice::Tree *m_tree;
    ioservice::NodeId m_class;
};
} // namespace device_registry

#endif // #ifndef DEVICE_REGISTRY_REGISTRY_HPP
