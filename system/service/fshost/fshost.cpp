/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// fshost -- the composing server (macro-fs design, .agents/tasks/macro-fs):
/// one served tree whose entries at chosen paths are other servers'
/// directories, a mount point each.
///
/// dev's meta/root.cm starts it as the eager child `fshost`, offers it the
/// built-in `boot` directory and memfs's `tmp` directory, and offers this
/// program's tree to the shell and to routing_checks -- not as one capability
/// but as two views: `all`, with every mount point, and `ro`, with the
/// read-only one only. Each is offered under the name `vfs`, so a component's
/// manifest says only /vfs and cannot tell which view it was handed. A use of
/// `vfs` therefore reaches the root's /boot through two servers: the user
/// opens /vfs/boot/... on the tree served here, the walk here stops at the
/// mount point, and the rest of the path is forwarded to the server that owns
/// it (macro-fs design "Events").
///
/// The objects:
///   - the Loop, one port and a slot per connection, on which everything
///     here waits;
///   - the Tree: a root, the two view directories under it, and under `all`
///     the remote entries -- `boot` holding the NsDir handle named /boot,
///     `tmp` the one named /tmp, `ro` sharing the boot channel through a
///     second node;
///   - the LoopServer serving the root of that tree on the DirectoryRequest
///     handle, so that an Open of `all` or `ro` binds the requester to that
///     view.
///
/// The events: the root writes Open("all" | "ro", rights, server) into the
/// outgoing directory -- this program's DirectoryRequest -- when a component
/// whose manifest uses `vfs` starts; the server binds |server| to the view's
/// node. From then on that component's Opens walk its view, and one reaching
/// a mount point is forwarded with the rest of its path. Nothing here is
/// kept per open: the forwarded Open carries the requester's own server end.
#include <ours/port.hpp>
#include <ours/process_args.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>

#include <ioservice/loop_server.hpp>
#include <ioservice/tree.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <ort/startup.hpp>
#include <prot/fshost.hpp>
#include <ustl/lazy_init.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;

/// The name the routed directory is bound at, matched as fsio matches its
/// namespace entries: an NsDir handle's arg is its name's index.
constexpr auto BOOT_PATH = std::string_view{"/boot"};
constexpr auto TMP_PATH = std::string_view{"/tmp"};
/// The disk's mount, which only exists when the image carries one: btreefs
/// exits having served nothing without a disk, so no NsDir for this path
/// arrives, and no mount point is made.
constexpr auto DISK_PATH = std::string_view{"/disk"};

/// The two views of the mounts, each exposed as its own directory capability
/// (system/service/fshost/BUILD.gn): `all` carries every mount point, `ro`
/// only the read-only one. root.cm offers each component the view it should
/// see, renamed with `as` to the path its manifest uses.
constexpr auto ALL_NAME = std::string_view{"all"};
constexpr auto RO_NAME = std::string_view{"ro"};

/// The mount points under them: `boot` holds the root's built-in BootFS
/// directory, `tmp` memfs's writable pool, each the name its manifest
/// exposes and root.cm offers by.
constexpr auto BOOT_NAME = std::string_view{"boot"};
constexpr auto TMP_NAME = std::string_view{"tmp"};
constexpr auto DISK_NAME = std::string_view{"disk"};

/// The tree's own svc directory, where the admin protocol's node lives for
/// the outgoing directory to expose.
constexpr auto SVC_NAME = std::string_view{"svc"};

/// What the tree is served with: the ceiling of what an Open of either view
/// may ask, the rights root.cm offers them with -- rx* for the BootFS mount,
/// w* for the writable one. A wider offer needs a wider ceiling here first,
/// and check 3 of the Open refuses the difference.
constexpr auto SERVED_RIGHTS = prot::io::RX_STAR_DIR | prot::io::W_STAR_DIR;

/// How many nodes the tree has room for: the root, the two views, the two
/// mount points of `all`, `ro`'s boot, the disk's mount when it exists, and
/// room for the mounts to come.
constexpr auto NODES = usize{16};

/// How many connections may be live at once: the outgoing directory itself,
/// plus every view connection and every connection made through them.
constexpr auto CONNECTIONS = usize{8};

/// The Loop's slots: one per connection, and one spare for the port's own
/// bookkeeping.
/// The admin connection's own slot, beside the tree connections'.
constexpr auto ADMIN_SLOTS = usize{1};

constexpr auto SLOTS = CONNECTIONS + ADMIN_SLOTS + 1;

static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS);

/// What the server keeps, outside the stack: constinit and built in main(),
/// as echo_server's storage is -- a user program runs no dynamic
/// initialisers.
struct Storage {
    std::array<ioservice::Node, NODES> nodes;
    std::array<ioservice::Connection, CONNECTIONS> connections;
    std::array<ioservice::LoopServer::Binding, CONNECTIONS> bindings;
    std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
    std::array<Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
    ioservice::ReplyBuffer reply;
    /// The mounted-at names, owned here: the tree keeps names as views, and
    /// the Mount request's name points at the shared message buffer, which
    /// the next read overwrites.
    std::array<std::array<char, 256>, 8> mount_names{};
    usize mount_names_used = 0;
};

constinit ustl::LazyInit<Storage> STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};

/// Says what the tree could not deliver: a reply, or a forwarded Open. The
/// tree holds no service node, so an Open of one has nowhere to go and its
/// channel closes.
/// The service-node id the admin protocol's node was added with.
constexpr auto ADMIN_SERVICE = u64{1};

/// The admin connection's channel while it is served, Invalid otherwise:
/// a second connect while one is served is closed, one admin client being
/// all a composing server needs.
Uandle ADMIN_CHANNEL = Uandle::Invalid;
loop::Loop *ADMIN_LOOP = nullptr;
ioservice::Tree *ADMIN_TREE = nullptr;
ioservice::NodeId ALL_NODE_FOR_ADMIN = ioservice::NodeId{0};

auto serve_admin() -> loop::Action;

/// The admin connection's Loop slot: one connection at a time, whose
/// messages are Mount requests.
class AdminBinding final: public loop::Waiter {
  public:
    auto on_signals(ours::Signals const observed) -> loop::Action override {
        if (!(observed & ours::Signals::Readable)) {
            ADMIN_CHANNEL = Uandle::Invalid;
            return loop::Action::Close;
        }
        return serve_admin();
    }
};

AdminBinding ADMIN_BINDING;

/// Arms the admin |channel| on its binding.
auto arm_admin(Uandle const channel) -> void {
    if (ADMIN_CHANNEL != Uandle::Invalid) {
        auto _ = ours::handle_close(channel);
        return;
    }
    auto const armed = ADMIN_LOOP->add(channel, ours::Signals::Readable | ours::Signals::PeerClosed,
                                       ADMIN_BINDING);
    if (armed.is_err()) {
        auto _ = ours::handle_close(channel);
        return;
    }
    ADMIN_CHANNEL = channel;
}

class Report final: public ioservice::LoopServer::Delegate {
  public:
    auto connect(u64 const service, Uandle const channel) -> void override {
        if (service == ADMIN_SERVICE) {
            arm_admin(channel);
            return;
        }
        auto _ = ours::handle_close(channel);
    }

    auto write_failed(Status const status) -> void override {
        auto _ = ort::debug_line({"fshost: a write failed: ", ours::to_string(status)});
    }
};

constinit Report REPORT;

/// Answers |status| for |txid| on the admin channel, the status form.
auto answer_admin(u32 const txid, Status const status) -> void {
    std::array<u8, wire::STATUS_FORM_SIZE> bytes{};
    auto const encoded = prot::fshost::encode_mount_reply(bytes, txid, status);
    if (encoded.is_ok()) {
        auto _ = ours::channel_write(ADMIN_CHANNEL, ours::ChannelWriteOptions::None,
                                     encoded->bytes.data(), static_cast<u32>(encoded->bytes.size()),
                                     nullptr, 0);
    }
}

/// Serves one message on the admin connection: a Mount, whose directory
/// handle joins the tree under the writable view at the asked name.
auto serve_admin() -> loop::Action {
    auto &storage = *STORAGE;
    auto num_bytes = u32{0};
    auto num_handles = u32{0};
    if (auto const read = ours::channel_read(
            ADMIN_CHANNEL, ours::ChannelReadOptions::None, storage.request_bytes.data(),
            storage.request_handles.data(), static_cast<u32>(storage.request_bytes.size()),
            static_cast<u32>(storage.request_handles.size()), &num_bytes, &num_handles);
        read != Status::Ok) {
        ADMIN_CHANNEL = Uandle::Invalid;
        return loop::Action::Close;
    }
    auto const message = wire::Message{
        .bytes = std::span{storage.request_bytes}.first(num_bytes),
        .handles = std::span{storage.request_handles}.first(num_handles),
    };
    auto const header = wire::decode_header(message.bytes, wire::Direction::ToServer);
    if (header.is_err()) {
        // Not speaking the protocol: the connection ends, nothing to answer.
        ADMIN_CHANNEL = Uandle::Invalid;
        return loop::Action::Close;
    }
    auto const request = prot::fshost::decode_mount(message);
    if (request.is_err()) {
        answer_admin(header->txid, request.unwrap_err());
        return loop::Action::Rearm;
    }
    auto const &asked = request.unwrap();

    // A mount point is one name directly under the writable view: no
    // separators, and nothing the tree would answer by shape rather than
    // by name.
    if (asked.name.empty() || asked.name.find('/') != std::string_view::npos || asked.name == "." ||
        asked.name == "..") {
        answer_admin(asked.txid, Status::InvArgs);
        return loop::Action::Rearm;
    }
    if (auto const taken = ADMIN_TREE->lookup(ALL_NODE_FOR_ADMIN, asked.name); taken.is_ok()) {
        answer_admin(asked.txid, Status::AlreadyExists);
        return loop::Action::Rearm;
    }
    if (storage.mount_names_used == storage.mount_names.size()) {
        answer_admin(asked.txid, Status::NoResource);
        return loop::Action::Rearm;
    }
    // The name moves into this program's own storage: the tree keeps names
    // as views, and the request's bytes are the shared buffer the next read
    // overwrites.
    auto &owned = storage.mount_names[storage.mount_names_used];
    asked.name.copy(owned.data(), asked.name.size());
    auto const owned_name = std::string_view{owned.data(), asked.name.size()};
    storage.mount_names_used += 1;
    // The handle rides the message; the tree owns it from here.
    auto const mounted =
        ADMIN_TREE->add_remote(ALL_NODE_FOR_ADMIN, owned_name, message.handles.front());
    if (mounted.is_err()) {
        answer_admin(asked.txid, mounted.unwrap_err());
        return loop::Action::Rearm;
    }
    answer_admin(asked.txid, Status::Ok);
    return loop::Action::Rearm;
}

/// The routed directories, taken the way fsio binds its namespace (an NsDir
/// handle's arg is its name's index): |boot| from /boot, |tmp| from /tmp.
/// Every other NsDir is closed: this program's manifest uses only these two.
auto take_mounts(Uandle *boot, Uandle *tmp, Uandle *disk) -> void {
    auto index = u32{0};
    for (auto const name: ort::process_args().names()) {
        if (index > u16{0xFFFF}) {
            break;
        }
        auto const directory =
            ort::take_startup_handle(ours::HandleType::NsDir, static_cast<u16>(index));
        index += 1;
        if (directory == Uandle::Invalid) {
            continue;
        }
        if (name == BOOT_PATH) {
            *boot = directory;
        } else if (name == TMP_PATH) {
            *tmp = directory;
        } else if (name == DISK_PATH) {
            *disk = directory;
        } else {
            auto _ = ours::handle_close(directory);
        }
    }
}
} // namespace

auto main(int, char **) -> int {
    auto const directory_request = ort::take_startup_handle(ours::HandleType::DirectoryRequest, 0);
    if (directory_request == Uandle::Invalid) {
        auto _ = ort::debug_line("fshost: started without a DirectoryRequest handle");
        return 1;
    }
    auto boot = Uandle::Invalid;
    auto tmp = Uandle::Invalid;
    auto disk = Uandle::Invalid;
    take_mounts(&boot, &tmp, &disk);
    if (boot == Uandle::Invalid || tmp == Uandle::Invalid) {
        auto _ = ort::debug_line("fshost: started without the boot or the tmp directory");
        auto _ = ours::handle_close(boot);
        auto _ = ours::handle_close(tmp);
        auto _ = ours::handle_close(directory_request);
        return 1;
    }

    auto &storage = *STORAGE.init();
    ioservice::Tree tree(storage.nodes);
    auto const root = tree.add_root().unwrap();

    // The admin protocol lives in the tree's own svc directory, a service
    // node whose Opens come to the delegate with ADMIN_SERVICE.
    auto const svc = tree.add_directory(root, SVC_NAME).unwrap();
    auto const admin_node = tree.add_service(svc, prot::fshost::ADMIN_NAME, ADMIN_SERVICE);
    if (admin_node.is_err()) {
        auto _ = ort::debug_line(
            {"fshost: cannot add the admin node: ", ours::to_string(admin_node.unwrap_err())});
        auto _ = ours::handle_close(directory_request);
        return 1;
    }

    // Two views of the same mounts, exposed as two directory capabilities:
    // `all` carries every mount point, `ro` carries only the read-only one.
    // Routing hands each component the view it should see, renamed to the
    // path its manifest uses; a component of the `ro` view cannot even tell
    // that `tmp` exists. A view's mount node is a second node over the same
    // channel -- the channel is a number, and the remote server serves every
    // forwarded connection alike.
    auto const all = tree.add_directory(root, ALL_NAME).unwrap();
    auto const ro = tree.add_directory(root, RO_NAME).unwrap();
    auto mounted = tree.add_remote(all, BOOT_NAME, boot);
    if (mounted.is_ok()) {
        mounted = tree.add_remote(all, TMP_NAME, tmp);
    }
    // The disk's mount is optional: no handle, no entry, and the `all` view
    // simply carries one mount point fewer.
    if (mounted.is_ok() && disk != Uandle::Invalid) {
        mounted = tree.add_remote(all, DISK_NAME, disk);
    }
    if (mounted.is_ok()) {
        mounted = tree.add_remote(ro, BOOT_NAME, boot);
    }
    if (mounted.is_err()) {
        auto _ = ort::debug_line(
            {"fshost: a mount point was refused: ", ours::to_string(mounted.unwrap_err())});
        auto _ = ours::handle_close(boot);
        auto _ = ours::handle_close(tmp);
        auto _ = ours::handle_close(disk);
        auto _ = ours::handle_close(directory_request);
        return 1;
    }

    auto port = Uandle::Invalid;
    if (auto const status = ours::port_create(ours::PortCreateOptions::None, &port);
        status != Status::Ok) {
        auto _ = ort::debug_line({"fshost: cannot make its port: ", ours::to_string(status)});
        auto _ = ours::handle_close(boot);
        auto _ = ours::handle_close(tmp);
        auto _ = ours::handle_close(directory_request);
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
    ADMIN_LOOP = &loop;
    ADMIN_TREE = &tree;
    ALL_NODE_FOR_ADMIN = all;
    ioservice::LoopServer server(tree, loop, {.handle_duplicate = ours::handle_duplicate},
                                 {.connections = storage.connections,
                                  .bindings = storage.bindings,
                                  .request_bytes = storage.request_bytes,
                                  .request_handles = storage.request_handles,
                                  .reply = &storage.reply},
                                 &REPORT);
    if (auto const served = server.serve(directory_request, root, SERVED_RIGHTS);
        served != Status::Ok) {
        auto _ = ort::debug_line({"fshost: cannot serve its tree: ", ours::to_string(served)});
        auto _ = ours::handle_close(boot);
        auto _ = ours::handle_close(tmp);
        auto _ = ours::handle_close(directory_request);
        return 1;
    }

    // Stop ends the program from inside an on_signals(); run() comes back
    // only when the port itself fails.
    auto _ = ort::debug_line(
        {"fshost: its Loop stopped: ", ours::to_string(loop.run(ours::INSTANT_INFINITE))});
    return 1;
}
