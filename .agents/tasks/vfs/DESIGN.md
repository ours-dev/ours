# vfs: a user-space VFS service that mounts the BootFS at /boot

**Date**: 2026-10-04
**Status**: Superseded by `docs/design/261004/component-framework.md` (2026-10-04)

## Problem
The launcher service finds a program by name in a BootFS it maps itself (`launch::launch_from_bootfs`, `system/lib/launch/src/bootfs.cpp`). The user's intent since 2026-09-26 is different: file systems follow Linux's VFS structure, a VFS service is started during boot, the BootFS is mounted in it at /boot as one adapter among later ones, and the launcher opens programs through that service. The launcher-service recommendation of 2026-10-03 bypassed that intent; this design restores it. It supersedes the parts of stage 4 in `docs/design/260924/userspace-filesystem.md` that have init serve /boot (BootServer, bootfs::Tree, GetBackingMemory with Execute in M2).

## Goals and non-goals
Goals:
- A VFS process with a Linux-style mount table, the BootFS mounted at /boot through a bootfs adapter.
- Clients speak a subset of fuchsia.io over channels.
- The VFS hands out read-only VMOs only; the launcher, the only VMEX holder, makes program VMOs executable.
- Every program except the bootstrap ones is found through the VFS.

Non-goals:
- A component framework: manifests, capability routing, per-component namespaces (Fuchsia component_manager's model, see Alternatives).
- Writable file systems.
- Performance work; candidates go to the user item by item.

## Design
**Revised 2026-10-04:** the user chose the full Fuchsia model (see Decisions). The Problem, Goals, Design and Alternatives sections still describe the split it replaced, and will be rewritten once the next decisions are made.

Draft: only what the decisions below settle.

Processes stay split: init supervises, the launcher alone holds VMEX, the VFS serves files and holds no VMEX.

Bootstrap follows Fuchsia's pattern, one step longer. In Fuchsia, userboot is the only load that bypasses every file system: it loads component_manager straight from the BootFS, and component_manager then serves /boot in-process (BootfsSvc, `src/sys/component_manager/src/bootfs.rs`). In ours:

1. userboot starts init from the BootFS.
2. init starts the launcher from the BootFS with the launch library: init's one use of VMEX, after which it closes VMEX.
3. init asks the launcher to start the VFS; the launcher keeps its BootFS path for this request alone.
4. The VFS mounts the BootFS at /boot; from then on the launcher opens every program through its VFS connection.

The /boot server is single-threaded, as BootfsSvc serves /boot on a single-threaded executor (`bootfs.rs:394-406`).

## Alternatives considered
- Follow component_manager wholesale: one root process that starts programs, serves /boot and routes capabilities, with file servers holding VMEX (BootfsSvc and pkg-cache both do; `src/storage/docs/paging.md:68-73`). Rejected: it reverses the 2026-10-03 decision (the launcher is a separate process and the only VMEX holder) and the 2026-09-26 one (init does not serve /boot), and per-component namespaces replace the Linux-style mount table with a component framework far beyond "basically usable". All it would save is the bootstrap order, which costs the launcher one restricted code path it already has.
- Merge only the /boot server into the launcher, BootfsSvc-style: the bootstrap problem disappears, but /boot is no longer in the VFS's mount table, against the user's request to mount the BootFS in the VFS.
- On security, keeping VMEX out of the VFS buys less than it sounds: the launcher makes whatever bytes the VFS serves executable, so the VFS is part of what decides which code runs either way, as a file system is in Linux. What the split buys is one small, auditable VMEX user with a narrow protocol, and a VFS that is only a file server.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Where the VFS lives | User-space process with a Linux-style mount table; in the kernel | User-space process | Not asked | User-space process started at boot, Linux-style mount table, BootFS at /boot (2026-10-04); reversed the same day by the full Fuchsia model: no VFS process, a server library for fuchsia.io, and a namespace per process |
| Client protocol | Subset of fuchsia.io over channels; a protocol of ours | fuchsia.io subset | Not asked | fuchsia.io subset (2026-10-04) |
| Who makes program VMOs executable | VFS holding VMEX, Fuchsia-style; the launcher alone | The launcher alone | Not asked | VFS hands out read-only VMOs, the launcher converts them (2026-10-04); reversed the same day: the /boot server holds VMEX and marks executables at ingest, as BootfsSvc does, and the launcher takes VMOs that are already executable |
| Process structure | Merge everything, component_manager-style; merge the /boot server into the launcher; keep split and borrow the bootstrap order and the single-threaded /boot server | Keep split | Not asked | Keep split, tentatively (2026-10-04); replaced the same day by the full Fuchsia model: a component_manager-style root process, a namespace per process, executability as a handle right. Reverses the 2026-09-26 Linux-VFS decision and the 2026-10-03 launcher decision |
| Who starts the VFS | (a) the launcher, through a BootFS path kept for the VFS alone; (b) init, with the library; (c) userboot | (a) | Not asked | (a), as part of keeping the split (2026-10-04); moot under the full Fuchsia model: the root process serves /boot itself |
| What makes a file executable | (a) a BootFS Executable entry flag (D6), with the launcher opening with the Execute IoRight; (b) a /boot/bin prefix; (c) no check | (a) | Not asked | Fuchsia's rule (2026-10-04): the node's executable attribute plus the Execute right of the connection; the /boot server marks files under bin/ executable, lib/ once shared libraries exist |
| Where the launcher resolves names | (a) its own VFS connection rooted at /boot, an explicit exception to the confused-deputy rule; (b) a directory the client sends | (a) | Not asked | Neither (2026-10-04): as fuchsia.process.Launcher, the launcher resolves no names and takes an executable VMO the client opened in its own namespace |
| Which process becomes the root | (a) init grows into it; (b) a new process | (a) | Not asked | (a) init grows into the component_manager-style root (2026-10-04) |
| How a process gets its directories | (a) a table fixed in the root's code for the first version; (b) manifests and capability routing from the start | (a) | Not asked | (b) manifests and routing from the start (2026-10-04) |
| Protocol encoding | (a) hand-written codecs for the fuchsia.io subset and the Launcher; (b) the generator first (G3, G4) | (a) | Not asked | (a) hand-written (2026-10-04) |

## Testing and verification
Planned with PLAN.md once the open decisions are settled. At least: host tests for the bootfs adapter and the path walk, shell tests that open /boot files and start programs through the VFS, and both images booting under `ours-verify`.

## Open questions
- Adapters as Linux-style operation tables or as a closed variant.
- The protocol generator (G3, G4) first, or a hand-written fuchsia.io subset.
- How the launcher reads a program's bytes: a `vma_unmap` system call, or start() reading from the VMO; the same for the vDSO file.
- How each child gets a grant of its own, so per-grant quotas separate clients.
- GRANT_MAX_BACKING_VMOS against launcher-side caching (a performance item).
- A slice of the BootFS VMO or a VMO per file (a performance item).
- Directories from flat BootFS names: synthesized on lookup or built at mount.
- Namespace directories only, or a '/' root.
- Whether the VFS's death is fatal in the first milestone.
- Where /svc lives.
- What the dbg_1 image, which has no init, does.
- A short read or OutOfRange at end of file.
- The order of the Open checks.
