# macro-fs: plan for the composing server and its first two filesystems

**Date**: 2026-10-30
**Status**: Accepted

## Request

The user asked (2026-10-30) to implement a macro filesystem service, modeled on the existing
service and ioservice patterns, that allows different concrete filesystems to be mounted under
different directories. Scope ends before any block device, partition, or disk filesystem.

## Decisions

See `DESIGN.md`; option A (remote entries + Open remainder forwarding) was delegated to the
agent on 2026-10-30.

## Steps

1. `ioservice`: add the `RemoteDir` node kind; `Server::open` stops at it and returns
   `Action::Forward`; `LoopServer` re-encodes and writes the Open on the remote channel. Host
   tests cover remainder, exact-mount-point, flags passthrough, dead remote, parent listing.
   One commit; verified by host tests plus `ours-verify` (no behavior change on target yet).
2. `fshost` component (working name): a served tree with the routed boot directory mounted at
   `boot`; wired into `root.cm` with a directory use/expose; shell opens a file through two
   hops. Commits: the component, then the image wiring.
3. `ours.io` + `fsio`: `File.Write` (two-way) and the `MaybeCreate`/`MustCreate`/`Truncate` bits
   of Open, with codec and host tests. One commit.
4. `memfs` component: writable in-memory tree, VMO-backed files, mounted by `fshost` at `tmp`;
   shell writes then reads back a file across the mount. Commits: memfs server, then mount.
5. Mount table shape beyond static routing (config file or admin protocol): decide with the
   user first; not started yet.

## Files

- `system/lib/ioservice/include/ioservice/tree.hpp` — fourth node kind.
- `system/lib/ioservice/include/ioservice/server.hpp` — `Action::Forward`.
- `system/lib/ioservice/src/{tree,server,loop_server}.cpp` — walk, forward, listing.
- `system/lib/ioservice/test/*` — host tests for the above.
- `system/service/fshost/*` — new component (step 2).
- `kernel/products/image/dev/BUILD.gn` — root.cm wiring (step 2).
- `system/lib/ours.io/*`, `system/lib/fsio/*` — Write and create flags (step 3).
- `system/service/memfs/*` — new component (step 4).

## Test plan

- Host: `ioservice` tests as listed per step; ours.io codec round-trip tests for `Write`.
- Target (dev image): `shell` opens `/vfs/boot/bin/...`-style path through the composing
  server and reads the ELF header; later writes and reads back `/vfs/tmp/...`. Both images boot
  clean under `ours-verify`.
