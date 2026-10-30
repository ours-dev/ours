# ioservice changes

## Title: an Open that reaches another server's subtree is forwarded to it (2026-10-30)

**Files**
- `system/lib/ioservice/include/ioservice/tree.hpp` — a fourth node kind, `Remote`, holding one channel; `Tree::add_remote()`, `Tree::remote()` and `Tree::walk()`, which stops at a remote entry with the unconsumed rest of the path; `lookup()` reimplemented on `walk()`.
- `system/lib/ioservice/include/ioservice/server.hpp` — `Action::Forward` and `Outcome::forward_channel`; `Server::open()` takes the reply buffer, to encode the forwarded Open itself.
- `system/lib/ioservice/src/tree.cpp` — the walk: one segment at a time as before, but a remote node ends it where it stands.
- `system/lib/ioservice/src/server.cpp` — the Open walk runs before the creation and cut-flag checks; a remote target forwards instead. A mount point lists as a directory.
- `system/lib/ioservice/src/loop_server.cpp` — `Action::Forward`: write the re-encoded Open on the remote entry's channel, fire and forget.
- `system/lib/ioservice/test/{tree,server,loop_server}_test.cpp` — nine tests: the remainder, the mount point opened as ".", flags passed through, rights checked first, local creation still refused beside the mount, listing, and forwarding over a real fake-kernel channel pair, dead remote included.

**Symptom**
ioservice could serve only what one process's own Tree held. A served tree could not present another server's directory at a path, so no composite tree existed: composition was client-side only, in each process's fsio namespace, and a composite view could not be handed to a third process as one directory capability.

**Cause**
Not an oversight but a decision: the component-framework design listed remote nodes as a non-goal ("no remote node splicing another server into a served tree", `docs/design/261004/component-framework.md`), and ioservice's closed set of three node kinds follows it. The macro-filesystem task (`.agents/tasks/macro-fs`) reverses that one non-goal: no global mount table and per-process namespaces stay, but a served tree may contain an entry that stands for another server's directory — a mount point. Without forwarding, every composite would have to copy the remote's contents or route clients to each filesystem separately, which is the client-side composition that cannot be transferred.

**Change**
A remote entry is a node like any other, so mounting is `add_remote(parent, name, channel)` and nothing more — no mount table, no mount step. The walk stops at it:

```cpp
// before: lookup() walked to the end or failed
auto const found = m_tree->lookup(connection.m_node, request.path);
...
// after: the walk reports where another server's subtree begins
auto const walked = m_tree->walk(connection.m_node, request.path);
if (walked.is_ok() && *m_tree->kind(walked->node) == NodeKind::Remote) {
    auto path = walked->remainder;
    if (path.empty()) {
        path = ".";
    }
    ...encode_open(out.bytes, out.handles, {.flags = flags, .path = path, .object = request.object});
    return ...Action::Forward...;
}
```

`Action::Forward` carries the Open already re-encoded, with the request's server end among its handles; LoopServer writes it on the remote's channel and keeps nothing: the opened connection is served by the remote directly, because Open itself carries the new channel end. The connection the request came on stays and answers the next message.

The walk runs before checks 6 (creation) and 7 (cut flags), because whether the target is this tree's decides whether those are this server's to judge: a writable filesystem mounted behind an entry could never create through it if the composer refused creation flags first. A local target never sees the difference — 6 and 7 still run before the walk's own error is reported, so the error order a client observes is the order the design fixed.

Rejected: binding a connection to the remote node and proxying every method (ReadDirents and Read re-encoded per call). Rejected because Open's one-way shape already delivers the new server end to whoever should serve it, so per-open state in the composer would only add a copy of every message and a second thing to keep alive.

**Reference**
Fuchsia `fs::RemoteDir` (`src/storage/lib/vfs/cpp/remote_dir.h`: "a remote directory is 'mounted' at creation time"), `FuchsiaVfs::Traverse` stopping at `IsRemote()` with the path remainder (`src/storage/lib/vfs/cpp/vfs.cc:46-94`), `DeprecatedOpenResult::Remote{.vnode, .path}` (`vfs.cc:130-133`) and `ForwardRequestToRemote` (`src/storage/lib/vfs/cpp/connection/directory_connection.cc:78-96`), which also strips inherit-write/execute rights the parent lacks. ours has no inherit bits (design D21 cut them), so the forwarded Open keeps its flags verbatim and every server in the chain applies its own subset check.

**Verification**
`boot_check.py`: build ok, dbg PASS (44 passed, 0 failed), rel PASS (41 passed, 6/6 required), host-tests PASS 148 (nine new: the tree walk, six server cases, two loop-server cases over the fake kernel, including a forward to a dead remote). clang-format clean on the changed lines; clang-tidy reports nothing on the changed lines (the test files' remaining warnings are the vendored googletest headers'). Not measured: forwarding latency and the cost of the re-encode.

**Deliberately not done**
- No unmount: tree entries are permanent, so a mount lasts the tree's life (as RemoteDir's comment says of Fuchsia: "It is not possible for the client to detach the remote directory").
- No watch on the remote channel: a dead remote is learned when a write fails, and that write has already consumed the request's server end, so the client sees its channel close with no epitaph — the same end an Open routed into a dead provider already meets.
- No mount-cycle guard: the first mounts come from static component routing, so a cycle is a config error; recorded as a known risk in `.agents/tasks/macro-fs/DESIGN.md`.
- No change to `Outgoing`: a provider's outgoing directory has no use for a mount point yet.

**How it was found**
None.

**The user's prediction**
None. The choice between remote forwarding and an in-process VFS was put to the user; the user declined to predict ("我没有办法做出决策") and delegated it ("直接开始实现整个文件系统吧", 2026-10-30). Option A, the remote entry, was taken; the reasoning is in `.agents/tasks/macro-fs/DESIGN.md`.

**Three questions you should be able to answer**
- Why does an Open that reaches a mount point leave no per-open state in the serving tree, and what property of the Open message makes that possible?
- Why do the creation-flag checks run only for local targets, and why does no local Open observe a different error order than before?
- Why does the forwarded Open carry the request's flags unchanged, and what still stops a client from gaining rights by opening through a mount point?
