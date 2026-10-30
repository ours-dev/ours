/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
///
/// fshost -- the composing server (macro-fs design, .agents/tasks/macro-fs):
/// one served tree whose entries at chosen paths are other servers'
/// directories, a mount point each.
///
/// dev's meta/root.cm starts it as the eager child `fshost`, offers it the
/// built-in `boot` directory, and offers its own `vfs` directory -- this
/// program's whole served tree -- to the shell and to routing_checks. A use
/// of `vfs` therefore reaches the root's /boot through two servers: the user
/// opens /vfs/boot/... on the tree served here, the walk here stops at the
/// `boot` entry, and the rest of the path is forwarded to the root's /boot
/// server, which answers from the BootFS (macro-fs design "Events").
///
/// The objects:
///   - the Loop, one port and a slot per connection, on which everything
///     here waits;
///   - the Tree: a root, `vfs` under it, and `boot` under that -- a remote
///     entry holding the one NsDir handle whose name is /boot, which the
///     root served before this program started;
///   - the LoopServer serving the root of that tree on the DirectoryRequest
///     handle, so that an Open of `vfs` on it binds the requester to the
///     composed directory.
///
/// The events: the root writes Open("vfs", rights, server) into the outgoing
/// directory -- this program's DirectoryRequest -- when a component whose
/// manifest uses `vfs` starts; the server binds |server| to the `vfs` node.
/// From then on that component's Opens walk the tree, and one reaching
/// `boot` is forwarded with the rest of its path. Nothing here is kept per
/// open: the forwarded Open carries the requester's own server end.
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
#include <ustl/lazy_init.hpp>
#include <wire/wire.hpp>

namespace {
using ours::Status;
using ours::Uandle;

/// The name the routed directory is bound at, matched as fsio matches its
/// namespace entries: an NsDir handle's arg is its name's index.
constexpr auto BOOT_PATH = std::string_view{"/boot"};
constexpr auto TMP_PATH = std::string_view{"/tmp"};

/// The name of the tree in the outgoing directory, which the manifest's
/// expose names too (system/service/fshost/BUILD.gn).
constexpr auto VFS_NAME = std::string_view{"vfs"};

/// The mount points under it: `boot` holds the root's built-in BootFS
/// directory, `tmp` memfs's writable pool, each the name its manifest
/// exposes and root.cm offers by.
constexpr auto BOOT_NAME = std::string_view{"boot"};
constexpr auto TMP_NAME = std::string_view{"tmp"};

/// What the tree is served with: the ceiling of what an Open of `vfs` may
/// ask, the rights root.cm offers it with -- rx* for the BootFS mount, w*
/// for the writable one. A wider offer needs a wider ceiling here first, and
/// check 3 of the Open refuses the difference.
constexpr auto SERVED_RIGHTS = ours_io::RX_STAR_DIR | ours_io::W_STAR_DIR;

/// How many nodes the tree has room for: the root, `vfs`, the two mount
/// points, and room for the mounts to come.
constexpr auto NODES = usize{8};

/// How many connections may be live at once: the outgoing directory itself,
/// plus every `vfs` connection and every connection made through it.
constexpr auto CONNECTIONS = usize{8};

/// The Loop's slots: one per connection, and one spare for the port's own
/// bookkeeping.
constexpr auto SLOTS = CONNECTIONS + 1;

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
};

constinit ustl::LazyInit<Storage> STORAGE;
constinit std::array<loop::Slot, SLOTS> SLOT_STORAGE{};

/// Says what the tree could not deliver: a reply, or a forwarded Open. The
/// tree holds no service node, so an Open of one has nowhere to go and its
/// channel closes.
class Report final: public ioservice::LoopServer::Delegate {
  public:
    auto connect(u64, Uandle const channel) -> void override {
        auto _ = ours::handle_close(channel);
    }

    auto write_failed(Status const status) -> void override {
        auto _ = ort::debug_line({"fshost: a write failed: ", ours::to_string(status)});
    }
};

constinit Report REPORT;

/// The routed directories, taken the way fsio binds its namespace (an NsDir
/// handle's arg is its name's index): |boot| from /boot, |tmp| from /tmp.
/// Every other NsDir is closed: this program's manifest uses only these two.
auto take_mounts(Uandle *boot, Uandle *tmp) -> void {
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
    take_mounts(&boot, &tmp);
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
    auto const vfs = tree.add_directory(root, VFS_NAME).unwrap();
    auto mounted = tree.add_remote(vfs, BOOT_NAME, boot);
    if (mounted.is_ok()) {
        mounted = tree.add_remote(vfs, TMP_NAME, tmp);
    }
    if (mounted.is_err()) {
        auto _ = ort::debug_line(
            {"fshost: a mount point was refused: ", ours::to_string(mounted.unwrap_err())});
        auto _ = ours::handle_close(boot);
        auto _ = ours::handle_close(tmp);
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
