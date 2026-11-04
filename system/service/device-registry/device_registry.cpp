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
///   - the registry protocol, RegistryServer, and the dev tree, DevSubtree,
///     both of the core this main() lays out (registry.hpp, where the grant
///     rule and the forwarding live, host-tested);
///   - the Loop, on which the outgoing directory and every client connection
///     wait.
///
/// The events: a driver connects to ours.dev.Registry and sends
/// Publish(class, name, protocol, directory); the registry grants the name
/// -- the proposal, or the class's lowest free three-digit number when it
/// was taken -- adds the service node and answers the granted name. A
/// client's Open of dev/ comes to DevSubtree::open(), which binds the
/// channel to the node the rest of the path names; from then on the
/// consumer's Opens of the leaves are the tree server's to walk, and a
/// leaf's lands on the mount's handler, which writes Open("svc/<protocol>")
/// into the publisher's directory.
#include <ours/process_args.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>

#include <array>

#include "registry.hpp"
#include <ddk/ddk.hpp>
#include <ioservice/outgoing.hpp>
#include <loop/loop.hpp>
#include <ort/debug.hpp>
#include <ort/main.hpp>
#include <prot/dev.hpp>
#include <ustl/lazy_init.hpp>

namespace {
using ours::Status;
using ours::Uandle;

/// Where the dev tree hangs in the outgoing directory: the path the
/// manifest's declaration states.
constexpr auto DEV_PATH = std::string_view{"/dev"};

/// The Loop's slots: one per connection to the outgoing directory, one per
/// registry client, one per dev-tree connection.
constexpr auto SLOTS = ioservice::Outgoing::MAX_CONNECTIONS + device_registry::MAX_CLIENTS +
                       device_registry::MAX_TREE_CONNECTIONS;

// Each slot in use holds one live wait on the port (<loop/loop.hpp>).
static_assert(SLOTS <= ours::PORT_MAX_OBSERVERS);

// What the servers keep, outside the stack: constinit, because nothing runs a
// user program's dynamic initialisers. The storages hold types with
// destructors (ustl::Option, in the ioservice connections), which as a plain
// global would register them with __cxa_atexit, which the runtime does not
// have; ustl::LazyInit is raw storage that main() builds them in instead, as
// init's are (system/service/init/init.cpp).
constinit ustl::LazyInit<ioservice::Outgoing::Storage> OUTGOING_STORAGE;
constinit ustl::LazyInit<device_registry::RegistryServer::Storage> REGISTRY_STORAGE;
constinit ustl::LazyInit<device_registry::DevStorage> DEV_STORAGE;
constinit ustl::LazyInit<device_registry::Core> CORE_STORAGE;

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
        device_registry::report("cannot make its port", status);
        return 1;
    }
    loop::Loop loop(port, SLOT_STORAGE);
    ioservice::Outgoing outgoing(*OUTGOING_STORAGE.init(), loop);
    device_registry::Core &core = *CORE_STORAGE.init();
    device_registry::DevSubtree dev(*DEV_STORAGE.init(), loop, core);
    if (auto const status = dev.build(); status != Status::Ok) {
        device_registry::report("cannot lay its tree out", status);
        return 1;
    }
    device_registry::RegistryServer registry(*REGISTRY_STORAGE.init(), loop, core, dev.tree(),
                                             dev.class_node());

    if (auto const status = outgoing.add_protocol(prot::dev::REGISTRY_NAME, registry);
        status != Status::Ok) {
        device_registry::report("cannot add its protocol", status);
        return 1;
    }
    if (auto const status = outgoing.add_subtree(DEV_PATH, dev); status.is_err()) {
        device_registry::report("cannot add its tree", status.unwrap_err());
        return 1;
    }
    if (auto const status = outgoing.serve(directory_request.unwrap()); status != Status::Ok) {
        device_registry::report("cannot serve its outgoing directory", status);
        auto _ = ours::handle_close(directory_request.unwrap());
        return 1;
    }

    // run() comes back only when the port itself fails.
    auto _ = ort::debug_line("device-registry: serving the device tree");
    device_registry::report("its Loop stopped", loop.run(ours::INSTANT_INFINITE));
    return 1;
}
