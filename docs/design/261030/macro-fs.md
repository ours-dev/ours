# macro-fs: one served tree composing concrete filesystems at mount points

**Date**: 2026-10-30
**Status**: Implemented

## Problem

Userspace can see exactly one filesystem: init serves the BootFS as `/boot`, read-only, and that is
all any process can open. There is no way to present several concrete filesystems under one tree:
composition exists only client-side, in each process's `fsio` namespace, so a composite view cannot
be handed to a third process as one directory capability. The user asked for a 宏文件系统 (a macro
filesystem): one served tree in which different directories are backed by different concrete
filesystems.

This re-opens one settled point: `docs/design/261004/component-framework.md` lists "vfs remote
nodes" and "Directory capabilities provided by components" as non-goals. Both are reversed here.
Everything else from 2026-10-04 stands: no VFS process with a global mount table, each process keeps
its own namespace, and an access ceiling stays a connection's rights.

## Goals and non-goals

Goals:

- A composing server (`fshost`, a component like any other) serves a tree whose entries at chosen
  paths are other servers' directories — a mount point.
- Mounting is inserting an entry; there is no mount syscall, no mount table.
- Concrete filesystems are separate servers speaking ours.io. The first two: the BootFS directory
  init already serves, and memfs, a writable in-memory pool.
- Opens traverse across mount points by forwarding the unconsumed path remainder with the request's
  own server end; rights never grow across a hop.

Non-goals:

- No VFS process with a Linux-style mount table (kept from the 2026-10-04 decision).
- No block devices, partitions, or disk filesystems yet; memfs and bootfs need none.
- No symlinks, watch, attributes protocol, or per-op proxying beyond Open forwarding.
- No dynamic mount protocol in v1; the first mounts come from component routing (static). Its shape
  — a config file or an admin protocol — is deliberately still open.

## Design

Objects:

- `RemoteDir`: a fourth node kind in `ioservice::Tree`. Payload is one directory channel (the
  client end of a pair whose server end some other server owns). Counterpart: `fs::RemoteDir`
  (`src/storage/lib/vfs/cpp/remote_dir.h`), where "a remote directory is mounted at creation time".
- The walk in `Server::open`: on reaching a `RemoteDir`, stop and report `{remote channel, flags,
  remainder path, object (the request's server end)}` as `Action::Forward`. Counterpart:
  `Vfs::Traverse` stopping at `IsRemote()` and `DeprecatedOpenResult::Remote{vnode, path}` carrying
  the remainder (`src/storage/lib/vfs/cpp/vfs.cc:46-94, 120-204`).
- `LoopServer`: on `Forward`, re-encode `Directory.Open` with the remainder and the original flags
  and write it on the remote channel, fire-and-forget. The opened connection is served by the remote
  server directly; the composing server keeps no per-open state. A failed forward write tells the
  delegate through `write_failed`.
- The mount point appears in the parent's `ReadDirents` as an ordinary directory entry.
- What the mount points are comes from component routing: `fshost.cm` uses `boot` (the root's
  built-in) and `tmp` (memfs's exposed directory), and its tree holds one remote entry per use,
  named as the use's source capability. The exposed name is the provider's mount's root: init's
  forwarded Open carries it (`OutgoingConnector::send`'s rule), and a provider that serves a tree
  treats it as the root of what it exposes.
- A concrete filesystem with state of its own speaks ours.io directly, as memfs does — an
  `ioservice::Tree` is read-only and closed — keeping the server library's dispatch shape: one
  message per wait, the Open checks in their order, an epitaph on a refused Open, a per-connection
  Binding on a Loop.

Events, for `Open("tmp/fs/x", s)` arriving at the composing server:

1. The walk consumes "tmp", which is a `RemoteDir`;
2. `Server::open` returns `Forward{channel, flags, "fs/x", s}`;
3. `LoopServer` writes `Open("fs/x", flags, s)` on the remote channel;
4. The remote server walks its own tree; if it too hits a mount point it forwards again;
5. The client's channel `c` is now connected to whichever server ended up holding `s`.

Edge cases:

- Open landing exactly on a mount point (path consumed): forward `Open(".", ...)`, which the remote
  serves as a clone of its root.
- Remote channel `PeerClosed` (server died): the forward's write consumes the request's server end;
  the client sees its channel close with no epitaph, the way an Open routed into a dead provider
  already ends.
- Rights: forwarded Opens carry the original requested rights unchanged; each server in the chain
  applies its own subset check against its own ceiling, so rights only narrow. A ceiling missing a
  bit a use states kills the mount point at its first Open.
- Mount cycles (a tree that forwards back into itself): static routing makes this a config error,
  not a runtime one; no guard in v1, recorded as a known risk.

## Alternatives considered

- **B: a Linux-style VFS** — open `ioservice::Tree` into a vnode interface, mount table of path to
  in-process filesystem instance, concrete filesystems linked into one process (the 2026-09-26
  design, `.agents/tasks/vfs/DESIGN.md`). Rejected: it reverses the whole 2026-10-04 "Follow
  Fuchsia" decision instead of two non-goals, and rewrites ioservice's closed tree. The user
  delegated the choice after both were taught (2026-10-30).
- **C: client-side composition only** — each process binds directories in its own `fsio` namespace
  (the state before this design). Rejected: a composite view cannot be transferred as one
  capability.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Composition mechanism | A: remote entries + remainder forwarding; B: in-process vnode interface | A | Declined ("我没有办法做出决策", then "直接开始") | A, delegated to the agent (2026-10-30) |
| What this reverses | Reverse the two component-framework non-goals only; reverse the whole Fuchsia model | Former | — | Remote nodes and component-provided directories allowed; the rest kept (2026-10-30) |
| The composing server's name | fshost; fsroot; fsmount | fshost | — | fshost (2026-10-30; the user asked why, and kept it) |
| The first writable filesystem | memfs speaking ours.io directly; extend ioservice to be writable | Direct | — | Direct; ioservice stays a read-only tree (2026-10-30) |

## Testing and verification

- Host tests in `system/lib/ioservice/test`: forwarding of the remainder (multi-segment, exact
  mount point, "." on a mount point), flags passed through unchanged, `PeerClosed` remote answered
  with a consumed server end, mount point listed by the parent's `ReadDirents`. 148 host tests pass.
- On target (dev rel image): routing_checks reads its own binary through `/vfs/boot` and lists
  `/vfs` and `/vfs/boot`; check_vfs lists `/vfs`, reads `/vfs/boot/bin/shell`, and creates, writes
  and reads back `/vfs/tmp/hello`. Both images pass `ours-verify`.

## Open questions

- The shape of a dynamic mount table beyond static routing — a config file, or an admin protocol
  with unmount (which needs tree removal, and ioservice's nodes are permanent). Deliberately
  undecided; nothing can mount dynamically until there is a block device to mount.
- Which directory-mutating ops (unlink, rename) memfs needs beyond file create/write.
