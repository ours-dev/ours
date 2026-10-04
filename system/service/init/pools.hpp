/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef INIT_POOLS_HPP
#define INIT_POOLS_HPP 1

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
/// Component instances, the root's own among them (D27: 8 components).
inline constexpr auto MAX_INSTANCES = usize{8};

/// /boot connections: namespace entries the root binds, and every Open and
/// Clone through them (D27: 128).
inline constexpr auto MAX_CONNECTIONS = usize{128};

/// fuchsia.process.Launcher connections (D27: 4, about 4.6 KiB of state
/// each, D13). Served from step 14 of the plan; counted here already, so
/// that the sums below are the ones that step will live with.
inline constexpr auto MAX_LAUNCHER_CONNECTIONS = usize{4};

/// Nodes of /boot: one per BootFS file and one per directory on the way.
/// Each file also costs the root one handle, its slice, so this bounds that
/// charge too.
inline constexpr auto MAX_BOOT_NODES = usize{256};

/// Directory uses one component may have. Each becomes a namespace entry,
/// and /svc takes one more, which together fill fsio's table
/// (fsio::MAX_BINDINGS, 16).
inline constexpr auto MAX_DIRECTORY_USES = usize{15};

/// The slots init still keeps for itself until the shell becomes a
/// component (step 15): the channel the shell answers its ping on, and the
/// shell's process.
inline constexpr auto INIT_OWN_SLOTS = usize{2};

/// Loop slots (D27): three per component -- its outgoing directory's client
/// end, its process, and its /svc server end from step 14 -- and one per
/// connection, /boot's and the Launcher's.
inline constexpr auto LOOP_SLOTS =
    3 * MAX_INSTANCES + MAX_CONNECTIONS + MAX_LAUNCHER_CONNECTIONS + INIT_OWN_SLOTS;

// Each slot in use holds one live wait on the port, armed or with its packet
// queued, and the port refuses an observer past its limit
// (<loop/loop.hpp>, Loop::Loop()).
static_assert(LOOP_SLOTS <= ours::PORT_MAX_OBSERVERS);

/// The handles init makes for the standalone launcher and the shell it still
/// starts itself, until steps 15 and 16 remove both, each charged to init,
/// which made it, wherever it is now:
///   - the launcher: init's connection to it (a channel pair), its
///     duplicates of the BootFS, VMEX and the vDSO's VMO, and its root
///     region, which launch::start() makes: 6;
///   - the shell: the ping channel pair, its duplicates of the job, the
///     BootFS and the vDSO's VMO, and the launcher connection made for it
///     (launch::Client::connect(), a channel pair): 7.
/// The shell's process handle is not among them: the launcher made it.
inline constexpr auto INIT_OWN_HANDLES = usize{6} + 7;

/// The handles the root pays for whatever it starts (the design's "Handle
/// charges"): the port, a slice per /boot file, and INIT_OWN_HANDLES. The
/// six startup handles userboot hands it are not counted: they are charged
/// to whoever made them, not to the process that holds them.
inline constexpr auto FIXED_HANDLES = usize{1} + MAX_BOOT_NODES + INIT_OWN_HANDLES;

/// What one running component with k directory uses costs: 8 + 2k -- the
/// JobDefault duplicate, the outgoing pair, the /svc pair, the process, its
/// root region and the vDSO duplicate, and a channel pair per namespace
/// entry. All of it is charged to the root, which made it, wherever the
/// handles are now (<ours/syscall.hpp>, HANDLE_MAX_PER_PROCESS).
inline constexpr auto HANDLES_PER_COMPONENT = usize{8} + 2 * MAX_DIRECTORY_USES;

/// What is left for what clients ask for at run time, which no pool bounds:
/// a GetBackingMemory duplicate each, until the client closes it, and from
/// step 14 three handles per running Launch. Exhausting it is design D18's
/// repair item, not a bug these sums could rule out.
inline constexpr auto HANDLES_FOR_CLIENTS =
    usize{ours::HANDLE_MAX_PER_PROCESS} - FIXED_HANDLES - MAX_INSTANCES * HANDLES_PER_COMPONENT;

// The pools alone must fit the quota with room to spare, so that a full set
// of components can always start before any client asks for anything.
static_assert(FIXED_HANDLES + MAX_INSTANCES * HANDLES_PER_COMPONENT <
              usize{ours::HANDLE_MAX_PER_PROCESS});
static_assert(HANDLES_FOR_CLIENTS >= usize{256});
} // namespace init

#endif // #ifndef INIT_POOLS_HPP
