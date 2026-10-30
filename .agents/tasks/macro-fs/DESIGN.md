# macro-fs: one served tree composing concrete filesystems at mount points

**Date**: 2026-10-30
**Status**: Implemented

## Problem

Userspace can see exactly one filesystem today: init serves the BootFS as `/boot`, read-only, and
that is all any process can open. There is no way to present several concrete filesystems under one
tree: composition exists only client-side, in each process's `fsio` namespace, so a composite view
cannot be handed to a third process as one directory capability. The user asked for a 宏文件系统
(a macro filesystem): one served tree in which different directories are backed by different
concrete filesystems.

This re-opens one settled point: `docs/design/261004/component-framework.md` lists "vfs remote
nodes" and "no remote node splicing another server into a served tree" as a non-goal. That
non-goal is reversed here. Everything else from 2026-10-04 stands: no VFS process with a global
mount table, each process keeps its own namespace, and an access ceiling stays a connection's
rights.

## Goals and non-goals

Goals:

- A composing server (working name `fshost`, a component like any other) serves a tree whose
  entries at chosen paths are other servers' directories — a mount point.
- Mounting is inserting an entry; unmounting is removing it. No mount syscall, no global table.
- Concrete filesystems are separate servers speaking ours.io. First two: the BootFS directory
  init already serves, and a new writable memfs component.
- Opens traverse across mount points by forwarding the unconsumed path remainder with the
  request's own server end; rights never grow across a hop.

Non-goals:

- No VFS process with a Linux-style mount table (kept from the 2026-10-04 decision).
- No block devices, partitions, or disk filesystems yet; memfs and bootfs need none.
- No symlinks, watch, attributes protocol, or per-op proxying beyond Open forwarding.
- No dynamic mount protocol in v1; the first mounts come from the component routing (static).

## Design

Objects:

- `RemoteDir`: a fourth node kind in `ioservice::Tree`. Payload is one directory channel (the
  client end of a pair whose server end some other server owns). Counterpart: `fs::RemoteDir`
  (`src/storage/lib/vfs/cpp/remote_dir.h`), where "a remote directory is mounted at creation
  time".
- The walk in `Server::open`: on reaching a `RemoteDir`, stop and report `{remote channel,
  flags, remainder path, object (the request's server end)}` as a new `Action::Forward`.
  Counterpart: `Vfs::Traverse` stopping at `IsRemote()` and `DeprecatedOpenResult::Remote{vnode,
  path}` carrying the remainder (`src/storage/lib/vfs/cpp/vfs.cc:46-94, 120-204`).
- `LoopServer`: on `Forward`, re-encode `Directory.Open` with the remainder and the original
  flags and write it on the remote channel, fire-and-forget. The opened connection is served by
  the remote server directly; the composing server keeps no per-open state.
- The mount point appears in the parent's `ReadDirents` as an ordinary directory entry.

Events, for `Open("tmp/fs/x", s)` arriving at the composing server:

1. The walk consumes "tmp", which is a `RemoteDir`;
2. `Server::open` returns `Forward{channel, flags, "fs/x", s}`;
3. `LoopServer` writes `Open("fs/x", flags, s)` on the remote channel;
4. The remote server walks its own tree; if it too hits a mount point it forwards again;
5. The client's channel `c` is now connected to whichever server ended up holding `s`.

Edge cases:

- Open landing exactly on a mount point (path consumed): forward `Open(".", ...)`, which the
  remote serves as a clone of its root.
- Remote channel `PeerClosed` (server died): epitaph `ConnectionRefused` on the request's server
  end, then close, the same way a dead protocol provider is reported today.
- Rights: forwarded Opens carry the original requested rights unchanged; each server in the
  chain applies its own subset check, so rights only narrow. No new bitmask logic here.
- Mount cycles (a tree that forwards back into itself): a static routing config makes this a
  config error, not a runtime one; no guard in v1, recorded as a known risk.

## Alternatives considered

- **B: a Linux-style VFS** — open `ioservice::Tree` into a vnode interface, mount table of path
  to in-process filesystem instance, concrete filesystems linked into one process (the
  2026-09-26 design, `.agents/tasks/vfs/DESIGN.md`). Rejected: it reverses the whole
  2026-10-04 "Follow Fuchsia" decision instead of one non-goal, and rewrites ioservice's closed
  tree. The user delegated the choice after both were taught (2026-10-30).
- **C: client-side composition only** — each process binds directories in its own `fsio`
  namespace (today's state). Rejected: a composite view cannot be transferred as one capability.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Composition mechanism | A: remote entries + remainder forwarding; B: in-process vnode interface | A | Declined ("我没有办法做出决策", then "直接开始") | A, delegated to the agent (2026-10-30) |
| What this reverses | Reverse the remote-node non-goal only; reverse the whole Fuchsia model | Former | — | Remote-node non-goal of component-framework.md reversed; the rest kept (2026-10-30) |

## Testing and verification

- Host tests in `system/lib/ioservice/test`: forwarding of the remainder (multi-segment, exact
  mount point, "." on a mount point), flags passed through unchanged, `PeerClosed` remote
  answered with an epitaph, mount point listed by the parent's `ReadDirents`.
- On target: a shell session opens a file through the composing server (two hops: composer →
  init's /boot server), and later writes and reads back a memfs file.
- `ours-verify` after every commit, both images.

## Open questions

- Shape of the eventual dynamic mount protocol, and who may call it (nothing can mount dynamically
  before a block device exists).
- Which directory-mutating ops (unlink, rename) memfs needs beyond file create/write.

The accepted design lives at `docs/design/261030/macro-fs.md`; RESULT.md has what landed.
