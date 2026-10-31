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
#include <ioservice/loop_server.hpp>
#include <ioservice/server.hpp>
#include <ioservice/tree.hpp>
#include <loop/loop.hpp>
#include <ours_io/ours_io.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <wire/wire.hpp>

namespace init {
/// What ingest() made of the BootFS, for the log line that says so.
struct Ingested {
    u32 files;
    u32 executable;
};

/// /boot: the BootFS as an ioservice tree, served over ours.io on the root's Loop
/// (component-framework design, "BootServer", and "Events: boot", step 3).
///
/// The same tree also holds the /svc directory of every component the root
/// starts, and the same server serves both, sharing D27's connection table:
/// an ioservice tree is a set of trees (ioservice::Tree::add_root()), and a /svc
/// connection is an ours.io directory connection like any other. Opening
/// a service node hands the request to the Delegate, which routes it
/// (design "Events: a component connecting to a protocol through /svc").
///
/// The objects:
///   - the tree, one node per directory and per file, each file a VmoFile
///     holding a slice of the read-only BootFS VMO at the file's
///     page-aligned range, made executable under bin/ (D15, S3); and beside
///     it one directory per component's /svc, with a service node per
///     protocol it uses;
///   - the ioservice::LoopServer that serves the tree on the Loop, with the
///     connection table and a Loop slot per connection.
///
/// The root itself reaches the tree by direct call (file()), never through a
/// connection of its own (I2): its resolver reads manifests and its runner
/// takes programs that way.
class BootServer final {
  public:
    /// Everything the server keeps, sized by the pools (pools.hpp). A
    /// constinit global of the program's: about 42 KiB, too much for the
    /// stack, and constant-initialised, since nothing runs a user program's
    /// dynamic initialisers.
    struct Storage {
        std::array<ioservice::Node, MAX_BOOT_NODES + MAX_SVC_NODES> nodes;
        std::array<ioservice::Connection, MAX_CONNECTIONS> connections;
        std::array<ioservice::LoopServer::Binding, MAX_CONNECTIONS> bindings;
        /// Where a connection's callback reads the one message it handles,
        /// and where the server builds its answer. One of each serves every
        /// callback, since one callback runs at a time (D16).
        std::array<u8, wire::MAX_MESSAGE_BYTES> request_bytes;
        std::array<ours::Uandle, wire::MAX_MESSAGE_HANDLES> request_handles;
        ioservice::ReplyBuffer reply;
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
    /// valid ours.io path, or a file where a directory has to be -- is
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
    auto file(std::string_view path) const -> ustl::Result<ioservice::VmoFile, ours::Status>;

    /// Serves the directory |subdir| of /boot ("" for /boot itself) on
    /// |channel|, with |rights|, as if a client had opened it, and waits on
    /// it: a namespace entry the root gives a component (design "Events:
    /// starting a component", step 2). On Ok the channel is the server's,
    /// its slot's to close. On failure nothing changed and the channel is
    /// still the caller's, to close with an epitaph: the lookup's refusals,
    /// NotDir, or NoResource when the connections or the Loop are full.
    [[nodiscard]]
    auto serve(ours::Uandle channel, std::string_view subdir, ours_io::Operations rights)
        -> ours::Status;

    /// Serves |directory|, any directory of the tree, on |channel| with
    /// |rights|: serve() for a node already found, such as a component's
    /// /svc. The same answers.
    [[nodiscard]]
    auto serve(ours::Uandle channel, ioservice::NodeId directory, ours_io::Operations rights)
        -> ours::Status;

    /// Makes an empty directory beside /boot, no part of it: one
    /// component's /svc (design "Events: starting a component", step 3).
    /// NoResource once MAX_SVC_NODES are in use.
    [[nodiscard]]
    auto add_svc_directory() -> ustl::Result<ioservice::NodeId, ours::Status>;

    /// Adds the service |name| to |directory|, one add_svc_directory()
    /// made; an Open of it reaches the Connector with |service|. The tree's
    /// refusals (InvArgs for a name that is not a valid ours.io name,
    /// AlreadyExists), and NoResource once MAX_SVC_NODES are in use.
    [[nodiscard]]
    auto add_service(ioservice::NodeId directory, std::string_view name, u64 service)
        -> ours::Status;

    /// The value of the first service in |directory|; none when it has no
    /// child or its first child is no service. This is how the root tells a
    /// component's /svc from /boot: every child of a /svc is a service, whose
    /// value says which instance's it is (add_service()), and /boot and its
    /// subdirectories, the namespace entries, hold no service at all.
    [[nodiscard]]
    auto first_service(ioservice::NodeId directory) const -> ustl::Option<u64>;

    /// Sets where Opens of service nodes go, and who is told of a reply that
    /// cannot be written or an Open that was refused: the root's Realm. Until
    /// it is set, such an Open's channel is closed.
    auto set_delegate(ioservice::LoopServer::Delegate *delegate) -> void {
        m_server.set_delegate(delegate);
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

    ioservice::Tree m_tree;
    ioservice::LoopServer m_server;
    ioservice::NodeId m_root = {};
    /// How many nodes each share has taken.
    usize m_boot_nodes = 0;
    usize m_svc_nodes = 0;
};
} // namespace init

#endif // #ifndef INIT_BOOT_SERVER_HPP
