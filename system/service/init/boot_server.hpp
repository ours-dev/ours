/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef INIT_BOOT_SERVER_HPP
#define INIT_BOOT_SERVER_HPP 1

#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

#include <array>
#include <span>
#include <string_view>

#include "pools.hpp"
#include <fuchsia_io/fuchsia_io.hpp>
#include <loop/loop.hpp>
#include <ustl/result.hpp>
#include <vfs/server.hpp>
#include <vfs/tree.hpp>
#include <wire/wire.hpp>

namespace init {
/// What ingest() made of the BootFS, for the log line that says so.
struct Ingested {
    u32 files;
    u32 executable;
};

/// /boot: the BootFS as a vfs tree, served over fuchsia.io on the root's
/// Loop (component-framework design, "BootServer", and "Events: boot", step
/// 3). The counterpart of component_manager's BootfsSvc
/// (src/sys/component_manager/src/bootfs.rs).
///
/// The same tree also holds the /svc directory of every component the root
/// starts, and the same server serves both, sharing D27's connection table:
/// a vfs tree is a set of trees (vfs::Tree::add_root()), and a /svc
/// connection is a fuchsia.io directory connection like any other. Opening
/// a service node hands the request to the Connector, which routes it
/// (design "Events: a component connecting to a protocol through /svc").
///
/// The objects:
///   - the tree, one node per directory and per file, each file a VmoFile
///     holding a slice of the read-only BootFS VMO at the file's
///     page-aligned range, made executable under bin/ (D15, S3); and beside
///     it one directory per component's /svc, with a service node per
///     protocol it uses;
///   - the vfs server and its connection table;
///   - one Loop slot per connection, whose callback hands one message to the
///     server and does what its Outcome says.
///
/// The root itself reaches the tree by direct call (file()), never through a
/// connection of its own (I2): its resolver reads manifests and its runner
/// takes programs that way.
class BootServer final {
  public:
    /// Where an Open of a service node goes: a function and its argument,
    /// as vfs::Syscalls and launch::LauncherOps are. |service| is the value
    /// the node was added with, and |channel| the request's, which the hook
    /// owns from the call on: it binds it to a protocol server, forwards it,
    /// or closes it with an epitaph.
    struct Connector {
        auto (*connect)(void *context, u64 service, ours::Uandle channel) -> void = nullptr;
        void *context = nullptr;
    };

  private:
    /// What a connection's Loop slot is called with: the server, and the
    /// connection the slot serves.
    struct Binding {
        BootServer *server = nullptr;
        vfs::ConnectionId connection = {};
    };

  public:
    /// Everything the server keeps, sized by the pools (pools.hpp). A
    /// constinit global of the program's: about 42 KiB, too much for the
    /// stack, and constant-initialised, since nothing runs a user program's
    /// dynamic initialisers.
    struct Storage {
        std::array<vfs::Node, MAX_BOOT_NODES + MAX_SVC_NODES> nodes;
        std::array<vfs::Connection, MAX_CONNECTIONS> connections;
        std::array<Binding, MAX_CONNECTIONS> bindings;
        /// Where a connection's callback reads the one message it handles,
        /// and where the server builds its answer. One of each serves every
        /// callback, since one callback runs at a time (D16).
        std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
        std::array<ours::Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
        vfs::ReplyBuffer reply;
    };

    /// A server whose tree and connections live in |storage|, and whose
    /// connections wait on |loop|. Both must outlive it. The tree is empty
    /// until ingest().
    BootServer(Storage &storage, loop::Loop &loop);

    BootServer(BootServer const &) = delete;
    auto operator=(BootServer const &) -> BootServer & = delete;

    /// Builds the tree from |mapping|, the BootFS mapped read-only into this
    /// process, which must stay mapped for good: Read copies from it, and
    /// the tree's names are views of it.
    ///
    /// Every entry's flat name is split on '/', the directories on the way
    /// are made, and the file gets a slice of |bootfs| over its bytes. Under
    /// bin/ (user_programs::DIRECTORY) the slice is replaced with
    /// vmo_replace_as_executable, one call per program, so that no handle
    /// with Execute covers the whole image (D15). The root keeps every
    /// slice for good. An entry the tree refuses -- a name that is not a
    /// valid fuchsia.io path, or a file where a directory has to be -- is
    /// logged and left out.
    ///
    /// Fails, with nothing served, when bootfs::View refuses the BootFS, or
    /// when the tree runs out of nodes (NoResource).
    [[nodiscard]]
    auto ingest(std::span<u8 const> mapping, ours::Uandle bootfs, ours::Uandle vmex)
        -> ustl::Result<Ingested, ours::Status>;

    /// The file at |path| below /boot ("bin/shell", "meta/root.cm"), by
    /// direct call: what the resolver and the runner read (I2). NotFile when
    /// it names a directory; the tree's lookup refusals otherwise.
    [[nodiscard]]
    auto file(std::string_view path) const -> ustl::Result<vfs::VmoFile, ours::Status>;

    /// Serves the directory |subdir| of /boot ("" for /boot itself) on
    /// |channel|, with |rights|, as if a client had opened it, and waits on
    /// it: a namespace entry the root gives a component (design "Events:
    /// starting a component", step 2). On Ok the channel is the server's,
    /// its slot's to close. On failure nothing changed and the channel is
    /// still the caller's, to close with an epitaph: the lookup's refusals,
    /// NotDir, or NoResource when the connections or the Loop are full.
    [[nodiscard]]
    auto serve(ours::Uandle channel, std::string_view subdir, fuchsia_io::Operations rights)
        -> ours::Status;

    /// Serves |directory|, any directory of the tree, on |channel| with
    /// |rights|: serve() for a node already found, such as a component's
    /// /svc. The same answers.
    [[nodiscard]]
    auto serve(ours::Uandle channel, vfs::NodeId directory, fuchsia_io::Operations rights)
        -> ours::Status;

    /// Makes an empty directory beside /boot, no part of it: one
    /// component's /svc (design "Events: starting a component", step 3).
    /// NoResource once MAX_SVC_NODES are in use.
    [[nodiscard]]
    auto add_svc_directory() -> ustl::Result<vfs::NodeId, ours::Status>;

    /// Adds the service |name| to |directory|, one add_svc_directory()
    /// made; an Open of it reaches the Connector with |service|. The tree's
    /// refusals (InvArgs for a name that is not a valid fuchsia.io Name,
    /// AlreadyExists), and NoResource once MAX_SVC_NODES are in use.
    [[nodiscard]]
    auto add_service(vfs::NodeId directory, std::string_view name, u64 service) -> ours::Status;

    /// Sets where Opens of service nodes go. Until it is set, such an Open's
    /// channel is closed.
    auto set_connector(Connector connector) -> void {
        m_connector = connector;
    }

  private:
    /// Whether the tree has room for one more node of /boot (MAX_BOOT_NODES)
    /// or of /svc (MAX_SVC_NODES). The two share the tree's storage, and
    /// each is held to its own share, so that /boot cannot take /svc's nodes
    /// and the root's handle sums, which count a slice per /boot node, hold.
    enum class Share : u8 {
        Boot,
        Svc,
    };

    [[nodiscard]]
    auto has_room(Share share) const -> bool;

    /// Counts one node added to |share|.
    auto took(Share share) -> void;

    /// A connection slot's callback: |context| is its Binding.
    static auto on_signals(void *context, ours::Signals observed) -> loop::Action;

    /// Handles what |observed| says happened on |connection|: one message,
    /// or its client gone.
    [[nodiscard]]
    auto handle(vfs::ConnectionId connection, ours::Signals observed) -> loop::Action;

    /// Waits on |channel|, the channel of the new connection |connection|.
    /// On failure the connection is released and the channel left to the
    /// caller.
    [[nodiscard]]
    auto watch(ours::Uandle channel, vfs::ConnectionId connection) -> ours::Status;

    /// Writes |message| on |channel|; what channel_write answers. The
    /// message's handles are consumed either way.
    [[nodiscard]]
    static auto write(ours::Uandle channel, wire::Message message) -> ours::Status;

    Storage *m_storage;
    loop::Loop *m_loop;
    vfs::Tree m_tree;
    vfs::Server m_server;
    vfs::NodeId m_root = {};
    Connector m_connector;
    /// How many nodes each share has taken.
    usize m_boot_nodes = 0;
    usize m_svc_nodes = 0;
};
} // namespace init

#endif // #ifndef INIT_BOOT_SERVER_HPP
