/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef INIT_POOLS_HPP
#define INIT_POOLS_HPP 1

#include <ours/handle.hpp>
#include <ours/port.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

/// The root's fixed pools, as design D27 settles them
/// (docs/design/261004/component-framework.md), with the arithmetic that
/// keeps them inside what the kernel gives one process.
///
/// Fixed because a user program has no heap
/// (system/lib/ort/src/cpp_dependencies.cpp), and sized at build time so that
/// the two limits the root can hit -- the port's observers and the process's
/// handle quota -- are checked here, by the compiler, rather than found at
/// boot. A request past a pool answers NoResource: an Open past the
/// connections on its server end, a resolve past the instances to whoever
/// asked (the router among them, whose termination rests on exactly that).
namespace init {
/// Component instances, the root's own among them (D27: 8 components; dev
/// has outgrown the design's count with the filesystem component, and the
/// next component bumps this with its own).
inline constexpr auto MAX_INSTANCES = usize{10};

/// /boot connections: namespace entries the root binds, and every Open and
/// Clone through them (D27: 128).
inline constexpr auto MAX_CONNECTIONS = usize{128};

/// ours.process.Launcher connections (D27: 4, about 5 KiB of state
/// each, D13): launcher_server.hpp. A connection past them is refused with
/// a NoResource epitaph.
inline constexpr auto MAX_LAUNCHER_CONNECTIONS = usize{4};

/// The protocols the root serves with a ResourceServer each
/// (resource_server.hpp): ours.resource's Debug, Irq, Ioport, Mmio, Iommu and
/// Sampling, and ours.kernel.RootJob (init.cpp builds one per protocol). A server
/// added there is a server counted here, or LOOP_SLOTS falls short by its
/// connections.
inline constexpr auto RESOURCE_SERVERS = usize{8};

/// Connections of one ResourceServer. D27 does not size them; a connection
/// holds nothing but its channel, so this costs a Loop slot each, and as many
/// as the Launcher's are enough: a client asks for its resource once, on a
/// connection it closes after the reply, as the shell and routing_checks do.
/// A connection past them is refused with a NoResource epitaph.
inline constexpr auto MAX_RESOURCE_CONNECTIONS = usize{4};

/// Nodes of /boot: one per BootFS file and one per directory on the way.
/// Each file also costs the root one handle, its slice, so this bounds that
/// charge too.
inline constexpr auto MAX_BOOT_NODES = usize{256};

/// Protocol uses one component may have: each is a service node of its
/// /svc. The design does not size them; 15 matches MAX_DIRECTORY_USES.
inline constexpr auto MAX_PROTOCOL_USES = usize{15};

/// Nodes of every component's /svc, which lie in /boot's tree (one ioservice tree
/// and one connection table serve both, BootServer): a directory per
/// component and a service node per protocol use. A component starts at
/// most once (D23), so its /svc is made at most once. Service nodes hold no
/// handle.
inline constexpr auto MAX_SVC_NODES = MAX_INSTANCES * (1 + MAX_PROTOCOL_USES);

/// Directory uses one component may have. Each becomes a namespace entry,
/// and /svc takes one more, which together fill fsio's table
/// (fsio::MAX_BINDINGS, 16).
inline constexpr auto MAX_DIRECTORY_USES = usize{15};

/// Loop slots (D27): three per component -- its outgoing directory's client
/// end, its process, and its /svc server end -- and one per connection:
/// /boot's and /svc's, the Launcher's and each ResourceServer's. Since step
/// 15 the root waits on nothing else: the shell is a component like any
/// other.
inline constexpr auto LOOP_SLOTS = 3 * MAX_INSTANCES + MAX_CONNECTIONS + MAX_LAUNCHER_CONNECTIONS +
                                   RESOURCE_SERVERS * MAX_RESOURCE_CONNECTIONS;

// Each slot in use holds one live wait on the port, armed or with its packet
// queued, and the port refuses an observer past its limit
// (<loop/loop.hpp>, Loop::Loop()).
static_assert(LOOP_SLOTS <= ours::PORT_MAX_OBSERVERS);

/// The handles the root pays for whatever it starts (the design's "Handle
/// charges"): the port and a slice per /boot file. The startup handles
/// userboot hands it are not counted: they are charged to whoever made them,
/// not to the process that holds them.
inline constexpr auto FIXED_HANDLES = usize{1} + MAX_BOOT_NODES;

/// What one running component with k directory uses costs: 8 + 2k -- the
/// JobDefault duplicate, the outgoing pair, the /svc pair, the process, its
/// root region and the vDSO duplicate, and a channel pair per namespace
/// entry. All of it is charged to the root, which made it, wherever the
/// handles are now (<ours/syscall.hpp>, HANDLE_MAX_PER_PROCESS).
inline constexpr auto HANDLES_PER_COMPONENT = usize{8} + 2 * MAX_DIRECTORY_USES;

/// What is left for what clients ask for at run time, which no pool bounds:
/// a GetBackingMemory or resource Get duplicate each, until the client
/// closes it, and three handles per running Launch (the process, its root
/// region and its vDSO duplicate), of which the process handle stays for as
/// long as the client keeps it. The autorun program, which the root starts
/// itself, takes one from here too, its root region, while it runs.
/// Exhausting it is design D18's repair item, not a bug these sums could rule
/// out.
inline constexpr auto HANDLES_FOR_CLIENTS =
    usize{ours::HANDLE_MAX_PER_PROCESS} - FIXED_HANDLES - MAX_INSTANCES * HANDLES_PER_COMPONENT;

// The pools alone must fit the quota with room to spare, so that a full set
// of components can always start before any client asks for anything.
static_assert(FIXED_HANDLES + MAX_INSTANCES * HANDLES_PER_COMPONENT <
              usize{ours::HANDLE_MAX_PER_PROCESS});
static_assert(HANDLES_FOR_CLIENTS >= usize{256});
} // namespace init

#endif // #ifndef INIT_POOLS_HPP
