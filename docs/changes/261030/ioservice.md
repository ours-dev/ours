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


## Title: An outgoing directory holds a directory capability's tree at its declared path, and its storage grows to 32 connections and 32 nodes (2026-10-30)

**Files**
- `system/lib/ioservice/include/ioservice/outgoing.hpp` — `Outgoing::add_directory(path)` makes the directory a declared path ends at and every missing one on the way; `add_directory(parent, name)` and `add_file(parent, name, file)` fill the tree below it; `MAX_CONNECTIONS` 4 to 32, new `MAX_NODES` 32; `Storage` sizes its nodes by `MAX_NODES` and gains `directories`, the node at the end of each declared path; the comments say what a directory capability is here, what refusals mean and where the console line is said
- `system/lib/ioservice/src/outgoing.cpp` — the three adders; `is_declared()`; `open_refused()` returns early for a connection that is not at the outgoing root or at `svc/`
- `system/lib/ioservice/include/ioservice/tree.hpp`, `system/lib/ioservice/src/tree.cpp` — `Tree::available()`, the room left, which `allocate()` now asks too
- `system/lib/ioservice/test/outgoing_test.cpp` — the slot table is `MAX_CONNECTIONS` (it was 8), a fixture that can open through any connection and read a file, and 17 tests for the declared path (14 to 31 in the file)
- `system/lib/ioservice/test/tree_test.cpp` — `AvailableCountsTheRoomLeft`
- `system/lib/ioservice/README.md` — `add_directory`, the Open of a declared directory, the new sizes, the scope of the console line

**Symptom**
Latent, cannot be triggered today: no program calls `add_directory()`, no manifest in the dev image declares a directory, and the root still answers `NotSupported` for a directory use that routes to a provider. The change is step 3 of 9 of `.agents/tasks/directory-capability/DESIGN.md`. Without it a provider has no place to put a tree: `Outgoing` built `svc/` and nothing else, and its storage, four connections and ten nodes, could not serve the consumers of a directory (one connection for each entry, one for each directory or file a consumer opens, one for each clone `fsio::export_namespace()` makes of an entry).

**Cause**
The outgoing directory was built for protocols, and a protocol costs the directory almost nothing: an Open of `svc/<name>` hands the request's channel to the program and makes no connection, so four connections (the root's own and three clones) and `2 + MAX_PROTOCOLS` nodes were exact. A directory capability is the opposite. The root writes one Open for each consumer, and the server answers it by making a connection on the consumer's channel, which stays for as long as the consumer holds the entry; everything the consumer opens below it makes another. So the connection table that was sized for the root alone becomes the limit on how many consumers and how many open files a provider has, and it fails by the consumer's `Open` answering `NoResource` on its own channel.

The node pool has the same shape. A declared path is a node for each of its segments, and `/export/a` with `/export/b` must share `export`, or the second declaration meets a node that exists and cannot be made. And a node, once added, stays (`Tree` has no removal), so a declaration refused for room halfway would leave its first directories behind for good: the check for room has to come before the first node is made, which is why `Tree` gained `available()` and why the order of the checks matters more than it looks.

The overlap rule has a subtlety that a string comparison gets wrong. "Equal to, above or below a declared path" is a statement about segments: `/export/data2` is not below `/export/data`, and `/svcx` is not under `/svc`. A prefix test refuses both. Walking the tree instead of comparing strings makes the rule hold by construction, since `Tree::lookup()` compares a whole name at a time, and it keeps one owner of the truth about what exists (a second list of the declared strings could drift from the tree). What the walk needs beyond the tree is which nodes are the ends of declarations, because a node is "below a declared path" only if an ancestor is one. `Storage::directories` holds exactly those.

**Change**
```cpp
// before                                                  // after
static constexpr auto MAX_CONNECTIONS = usize{4};          static constexpr auto MAX_CONNECTIONS = usize{32};
                                                           static constexpr auto MAX_NODES = usize{32};
std::array<Node, 2 + MAX_PROTOCOLS> nodes;                 std::array<Node, MAX_NODES> nodes;
                                                           std::array<NodeId, MAX_NODES> directories;
```

`add_directory(path)` judges the call whole, in this order, and makes nothing until the last check has passed:

1. The syntax: `path` starts with `/`, and what follows is a valid ours.io path (`is_valid_path()`) that is not `.` (that one is valid as "the directory itself", which is not a place to put a directory). That refuses `/`, a trailing or doubled `/`, `.` and `..` segments, a NUL, a name or a path past its limit.
2. `svc`: the first segment is not `svc`. By segment, so `/svcx` and `/export/svc` are fine.
3. The overlap: walk the segments from the root with `Tree::lookup()`. A node that exists and is the end of a declaration means the path is equal to or below it (AlreadyExists). If every segment exists and none is an end, the path is above one (AlreadyExists): every node outside `svc/` is on the way to a declaration, is one, or is below one. The first segment that does not exist starts what has to be made.
4. Room: the segments left, counted by their `/`, against `Tree::available()`. NoResource.
5. Made: a node for each segment left. Nothing in it can fail, since the names are valid, none exists in its parent, the parents are directories and there is room; `unwrap()` traps if one ever does, as the constructor's two nodes do.

A declaration also needs a place in `directories`, which cannot be full: each takes a node of its own, so there are never more than `MAX_NODES`.

The program fills the tree with `add_directory(parent, name)` and `add_file(parent, name, file)`, which only forward to `Tree`. This is the choice the design left: `Outgoing` wraps the adders and does not hand out `tree()`. A reachable tree would let the program add a service node with a value that `Outgoing::connect()` takes for an index into its handler table, and a node at the root that no declaration accounts for, and each of those breaks an invariant `Outgoing` owns (service value, handler index; every node outside `svc/` is on a declaration's path). The wrappers cost two functions and are the shape step 5 needs anyway: a service node under a declared directory registers its handler in the table `add_protocol()` fills, which only `Outgoing` can do. The rejected option was `auto tree() -> Tree &`, one line and a hole.

The Open of a path below the directory needs no code. The root writes `Open("export/data/<subdir>", rights | ProtocolDirectory, S)` on the first connection of the directory, which holds every right; `Server::open()` walks the path from the connection's node, and a directory node at its end makes a connection on S with exactly the rights asked for. The tests show every case of the design: a file under it reads, an Open with rights the connection does not hold is AccessDenied, a file or a service opened with `ProtocolDirectory` is NotDir (the file at `export/data/x`; the service at `svc/<name>`, since the node at a declared path is always a directory), and the connections run out with NoResource on the Open's channel. Nothing in `server.cpp` changed.

The one behaviour that did need deciding, which the design left to this step, is the console line. `open_refused()` said a line for every refused Open of the outgoing directory. That was right while only the root held a connection to it. Now a consumer holds one to its directory, and a consumer that asks for a file that is not there, or writes for more than it holds, would print a line on the provider's console each time, and a consumer in a loop would fill it. The line exists for the root's Opens, where a manifest and a program that spell a name or a path differently show, so it is now said when the Open was refused through the outgoing root or `svc/`, which is where the root's arrive, and not through a connection of a declared directory: the consumer reads its own epitaph.

```cpp
// before                                                  // after
auto Outgoing::open_refused(NodeId, string_view path,      auto Outgoing::open_refused(NodeId directory, string_view path,
                            Status status) -> void {                                  Status status) -> void {
    ...                                                        if (directory != m_root && directory != m_svc) { return; }
                                                               ...
```

The constants, decision 8 (b): 32 connections, 32 nodes, with `MAX_NODES` beside `MAX_CONNECTIONS`. The nodes are one pool, not a share for each use: `2 + MAX_PROTOCOLS` of them are the root, `svc/` and the protocol nodes only once every protocol is added, so a program that wants all its protocols adds them before a large tree. A partition that reserved the protocols' share would have made `add_protocol()` never fail for room, at the price of 22 nodes for directories even in a provider with no protocol (the common case for a file server); the pool is the simpler of the two and can be partitioned later.

Sizes, measured on the target build (`llvm-nm -S`, `llvm-size -A` of `out/dc-step3/user-x86/obj/examples/echo/echo_server`), with the design's table beside them:

| | Before | After | Design said |
|---|---|---|---|
| `Outgoing::Storage` (`OUTGOING_STORAGE`) | 9,496 B | 13,000 B | about 12.9 KB (+3,376 B) |
| echo's Loop table (`SLOT_STORAGE`, 24 B a slot) | 8 slots, 192 B | 36 slots, 864 B | +672 B |
| echo `.bss` | 22,960 B | 27,136 B | about +4 KiB (+4,048 B) |
| hello `.bss` | 22,816 B | 26,992 B | |
| echo `.text` | 26,670 B | 27,006 B | |

The 128 bytes over the design are `Storage::directories`, which the design did not count: 32 node ids. Echo's and hello's sources do not change; their Loop tables are `MAX_CONNECTIONS + MAX_CLIENTS`, and the `static_assert` against `PORT_MAX_OBSERVERS` (1,024) holds at 36 and 33 slots. The extra text is the three adders, linked in even though neither example calls them.

**Reference**
Fuchsia `component::OutgoingDirectory::AddDirectoryAt()` (`sdk/lib/component/outgoing/cpp/outgoing_directory.cc:169`) puts a remote directory at a path inside the outgoing directory, creating the directories on the way, and refuses an empty name. This library puts a tree of its own there instead of a remote, and the path is the manifest's declared path as Fuchsia's `source_path` is (`sdk/fidl/fuchsia.component.decl/capability.fidl:64-76`), decision 2 (b) of the design. The overlap refusal has no counterpart in the outgoing directory there, whose own check is a name that exists (`ZX_ERR_ALREADY_EXISTS`, `outgoing_directory.h:283`); Fuchsia's validator holds apart the paths of a component's `use` declarations, as prefixes of each other (`validate_use_paths()`, `src/sys/lib/cm_fidl_validator/src/lib.rs:929`), not the source paths of its capabilities. The by-segment rule on declared paths is this project's, and the manifest compiler's `_is_prefix` (`system/component_manifest.py`) is its twin.

**Verification**
- Host tests, `outgoing` 31 (was 14) and `ioservice` 49 (was 48): all pass. `boot_check.py --out out/dc-step3`, before and after: dbg 44 passed, 4 skipped, 0 failed; rel 41 passed, 8 skipped, 0 failed, 6 of 6 required; host tests 151 passed, 0 failed. The same counts.
- Mutation checks, each a deliberate change to one source file, built, run and reverted (a script in the session scratchpad, not in the tree). All 29 are caught:
  - the refusals: a relative path accepted, `.` alone accepted, validity not checked, `svc` by string prefix (caught by the `/svcx` test), `svc` not refused, only `/svc` itself refused;
  - the overlap: a declaration never recognised, equal-or-above not refused, the declaration not recorded, the wrong node recorded, overlap decided by name prefix (a mutation of `Tree`'s name compare, caught by `/export/dat` after `/export/data`);
  - the order: no room check with creation that returns the error and so leaves `x` behind (caught by `AddDirectoryRefusedForRoomLeavesNothingBehind`), the room counted one short and counted from the whole path (caught by the shared `export` test, and by a trap in `unwrap()`);
  - the returned node: the root instead of the last;
  - the console line: said for every directory, said for the root only (misses `svc/`), said for nothing;
  - the constants: `MAX_CONNECTIONS` 4 and 31, `MAX_NODES` 10, 31 and 33, and `Tree::available()` off by one;
  - the core those tests rest on: a file and a service opened as a directory answering BadType, rights beyond the connection's not refused, a new connection given every right, and the table running out answering something other than NoResource.
  Two mutations first failed by crashing the test binary, not by failing a test (`unwrap_err()` traps on a call that succeeded); the tests now read a status through a helper that returns Ok for a success, and every one that can fail a test does.
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step3`) on `outgoing.cpp`, `tree.cpp`, `outgoing.hpp` and both test files: no diagnostic on any line of `system/lib/ioservice`; what remains is googletest's header. `format_cpp.py --check` is clean on all of them.
- Not measured: a provider on the target. No program calls `add_directory()` yet, so the target run is step 4's.

**Deliberately not done**
- A reserved share of the node pool for the protocols (see Change).
- Reading the declared path into a table of strings. The tree answers every question the overlap rule asks.
- A guard in the wrappers against a `parent` that is the root or `svc/`. A program only gets ids from `add_directory(path)` and the wrappers; one made up is the program's bug, as for `Tree`.
- `add_service(path, handler)` and `add_subtree()`: steps 5 and 6.
- Any change to the guide, which still describes a four-connection, ten-node `Outgoing` of about 9 KiB (the task note lists the passages).

**How it was found**
None. A new feature, from the design. The design's note that a typo between the manifest's path and the program's "reads as a NotFound with no line in any log unless `open_refused` is extended to a refused Open below a directory node" was already untrue for the root's Opens: the line is said for every refusal, whatever the depth, so `export/ghost` is logged. What the design did not see is the other side, a consumer's refusals, which the new scope removes.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why must `add_directory("/x/y")` ask whether two nodes fit before it makes `x`, when `Tree` could answer NoResource at `y`?
2. `/export/data2` and `/svcx` are legal beside `/export/data` and `/svc`. What makes the walk refuse neither, and what would a string-prefix test have done?
3. Why does the console line stop at the outgoing root and `svc/`, and what would a consumer be able to do to the provider's console if it did not?

## Title: A directory capability's tree can hold a service node, and its handler comes from the table the protocols use (2026-10-31)

**Files**
- `system/lib/ioservice/include/ioservice/outgoing.hpp` — `Outgoing::add_service(parent, name, handler)`; `in_declared_tree()`; the doc comments of `Handler`, `MAX_PROTOCOLS`, `Storage::handlers` and `add_protocol()` say that the handler table is shared; the private `m_protocols` is `m_handlers`
- `system/lib/ioservice/src/outgoing.cpp` — `add_service()`, `in_declared_tree()` and the walk under it (`at_or_below()`); `add_protocol()` and `connect()` use `m_handlers`
- `system/lib/ioservice/test/outgoing_test.cpp` — ten tests (31 to 41 in the file)
- `system/lib/ioservice/README.md` — `add_service`, and `MAX_PROTOCOLS` counting both

**Symptom**
Latent, cannot be triggered today: no program calls `add_service()`. The change is step 5 of 9 of `.agents/tasks/directory-capability/DESIGN.md` (decision 11 (c)). Without it the first provider, `examples/files`, has no way to be asked to exit, and the provider-exit case of the design's Testing section (a consumer holds a working entry, the provider exits, the consumer sees it) cannot be run at a moment the test chooses.

**Cause**
`Outgoing` could put one kind of service node in a tree, the protocols' under `svc/`, and `tree()` is not reachable (step 3). That is deliberate and it is why the call has to be here. A service node's value is an index into `Storage::handlers`: `Server` answers an Open of the node with `Action::Connect` and that value, the `LoopServer` calls the one `Delegate::connect(u64 service, channel)`, and `Outgoing::connect()` reads the value as a place in the table, closing the channel when it is past the end. A program that could call `Tree::add_service()` itself could write any value, including one that indexes a handler that was never added, so the node and the table have one owner: the `Outgoing` adds both.

That decides the shape. The handler goes in the table `add_protocol()` fills and the node's value is its index, so a directory capability's service node and a protocol are the same thing to `connect()`, and `MAX_PROTOCOLS` is a limit on the sum. A second table would need the node's value to say which table it indexes, in a `u64` that is now a plain index; one table costs only that a provider with eight protocols has no room for a service node. The constant keeps its name: it is public, and renaming it is a change to every user for a limit that is nowhere near being reached.

The order of the checks is the part to understand, because a node, once added, stays (`Tree` has no removal):

1. The parent is a node of a declared directory's tree, or InvArgs. The protocols' nodes are `add_protocol()`'s: a service node at the outgoing root or in `svc/` would be one that no declaration accounts for, and one in `svc/` could shadow or collide with a protocol. The directories on the way to a declared path (`export` for `/export/docs`) are refused too, because they belong to no capability. A `Tree` has no link from a node to its parent, so the test walks each declared directory's children (`at_or_below()`) and recurses once for each level, and a tree is never deeper than its storage (`MAX_NODES`, 32). The walk needs no storage of its own, where a flag per node would have put a "mark" in a generic tree, or an array in `Storage`, for the one question.
2. The table has room, or NoResource, before the tree is touched. Were it judged after `Tree::add_service()`, a full table would leave the node behind with a value one past the table, and every later Open of it would reach no handler: `connect()` would close the channel. That is the bug the order prevents, and the test for it opens the name after the refusal and adds a file of the same name.
3. `Tree::add_service()`'s own refusals (InvArgs for a bad name, NotDir, AlreadyExists, NoResource for the nodes), and only when the node is in is the handler stored and the count raised. The count is raised after, so that a refusal of the tree takes no slot: a test adds `MAX_PROTOCOLS - 1` services after a run of refusals and needs them all to fit.

**Change**
```cpp
// before                                              // after
usize m_protocols = 0;                                 usize m_handlers = 0;   // protocols and service nodes

auto add_protocol(name, handler)                       auto add_protocol(name, handler)   // unchanged, but for the name
                                                       auto add_service(NodeId parent, std::string_view name, Handler &handler)
                                                           -> ustl::Result<NodeId, Status>;
```
```cpp
auto Outgoing::add_service(NodeId const parent, std::string_view const name, Handler &handler)
    -> ustl::Result<NodeId, Status> {
    if (!in_declared_tree(parent)) {
        return ustl::err(Status::InvArgs);
    }
    if (m_handlers == MAX_PROTOCOLS) {
        return ustl::err(Status::NoResource);
    }
    auto const added = m_tree.add_service(parent, name, m_handlers);
    if (added.is_err()) {
        return added;
    }
    m_storage->handlers[m_handlers] = &handler;
    m_handlers += 1;
    return added;
}
```

The shape follows step 3's wrappers: `add_service(parent, name, handler)` takes a node `add_directory(path)` or `add_directory(parent, name)` returned and returns the new node, as `add_directory(parent, name)` and `add_file(parent, name, file)` do. The design wrote `add_service(path, handler)`. That form would walk a path a second time, to find a parent the program already holds, and would have to invent the rule for a path whose last segment is the node's name; the wrapper form needs no such rule and keeps the three adders alike. The cost is that a program passes a `NodeId` it kept. The Open of the node with `PROTOCOL_SERVICE` reaches `handler.connect()` through the server as before, a `PROTOCOL_DIRECTORY` Open of it is NotDir and a `PROTOCOL_FILE` one NotFile (the server's service branch, unchanged), and a connection without CONNECT is AccessDenied (check 4, and the last check of the service branch for a request that names no protocol). Nothing in `server.cpp` changed.

**Reference**
None for the shape: this project's own design (decision 11 (c)). The nearest piece in Fuchsia is a service entry added to a pseudo-directory, `vfs::PseudoDir::AddEntry()` with a `vfs::Service` (`sdk/lib/vfs/cpp/pseudo_dir.h`, `sdk/lib/vfs/cpp/service.h`), and `endpoint()` in the Rust vfs (`src/storage/lib/vfs/rust/src/service.rs:73`); both let the caller put the entry in any directory and hold the callback themselves. Ours restricts the parent because the node's value belongs to the `Outgoing`.

**Verification**
- Ten tests on the fake kernel, `outgoing` 31 to 41: a service node at a declared directory and below it reaches the handler it was added with and the Open's channel, before and after `serve()`; through a consumer's entry (a `r*` entry connects); a connection without CONNECT is AccessDenied for the service flag and for a request that names no protocol, and rights beyond the entry's are refused first; as a directory it is NotDir (from the root and through an entry) and as a file NotFile, never reaching the handler; the refused parents (the outgoing root, `svc/`, a protocol's node, the directory on the way to a declared path, an id of no node) with nothing made and no handler taken; the tree's refusals (bad names, a file, a directory and a service with the name, a file as parent) with the table intact; the nodes used up; eight protocols leave no room for a service node and the reverse, with the node not left behind; and eight handlers, alternately protocols and service nodes, each reached by its own path and no other.
- Mutations of the sources, each built and run and reverted (script in the session scratchpad, not in the tree); `outgoing`'s tests fail for every one except an equivalent:

  | Mutation | Caught by |
  |---|---|
  | the table check after the node is added | `EightProtocolsLeaveNoRoomForAServiceNode` |
  | no check of the parent | `AddServiceRefusesAParentThatIsNotInADeclaredTree` |
  | only `svc/` refused | the same |
  | the root and `svc/` refused, the way to a declared path not | the same |
  | a protocol's own node accepted as a parent | the same |
  | only the declared node itself accepted, nothing below it | `AServiceNodeInADeclaredTreeReachesItsHandler`, `AddServiceRefusesAsTheTreeDoesAndLeavesNothingBehind` |
  | `connect()` always reaches the first handler | four tests, among them the one that mixes protocols and service nodes and two older ones |
  | the node carries value 0 | `AServiceNodeInADeclaredTreeReachesItsHandler`, `ProtocolsAndServiceNodesTakeTheirHandlersFromOneTableInOrder` |
  | `add_service()` does not count its handler | six tests |
  | `add_service()` cannot use the last slot | four, among them both orders of the shared table |
  | a refused call takes a slot | `AddServiceRefusesAsTheTreeDoesAndLeavesNothingBehind` |
  | `add_protocol()` ignores the service nodes | `EightServiceNodesLeaveNoRoomForAProtocol` |
  | a service opened as a directory answers BadType | `AServiceNodeOpenedAsADirectoryOrAFileIsRefusedAndNotConnected` and three older tests |
  | the last CONNECT check of the service branch removed | `AServiceNodeNeedsAConnectionThatHoldsConnect` |
  | check 4 (CONNECT for the service flag) removed alone | not caught, and equivalent: the last check of the branch refuses the same request, so only the two removed together change an answer, and that is caught |

- `boot_check.py --out out/dc-step5`: dbg PASS, 44 passed, 4 skipped; rel PASS, 41 passed, 8 skipped, 7 of 7 required (see `routing-checks.md`); host tests PASS, 151 passed.
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step5`) on `outgoing.cpp` with its header and on `outgoing_test.cpp`: no diagnostic on any line of `system/lib/ioservice`; what the second prints is googletest's own header. `format_cpp.py --check` clean.

**Deliberately not done**
- `tree()` is still not reachable, and no `add_service(path, handler)`.
- `MAX_PROTOCOLS` is not renamed or raised, and the table is not split.
- A service node under `svc/`, or at the outgoing root: refused, not allowed with a flag.
- A check that the same `Handler` is not added twice: one handler may serve any number of nodes, which is how a program with one `stop` handler for several directories would do it.
- A removal: a node, once added, stays, as everywhere in `Tree`.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `add_service()` judges the table before it calls `Tree::add_service()`, and raises the count after. What would each of the two orders reversed leave behind, and how does a test tell?
2. Why can the call not accept a parent of `svc/`, though a service node would work there, and why is the directory on the way to `/export/docs` refused too?
3. `MAX_PROTOCOLS` is eight and `files` has one service node. What does a provider with eight protocols lose, and what would two tables have cost instead?

## Title: A Subtree node hands an Open and the rest of its path to the program (2026-10-31)

**Files**
- `system/lib/ioservice/include/ioservice/tree.hpp`, `src/tree.cpp` — `NodeKind::Subtree`; `Tree::add_subtree()`, `Tree::subtree()`; `Tree::resolve()` and `Resolved`, with `lookup()` now a client of `resolve()`; the node's private value is `m_value`, shared by Service and Subtree nodes
- `system/lib/ioservice/include/ioservice/server.hpp`, `src/server.cpp` — `Action::Subtree`; `Outcome::rest` and `Outcome::flags`; `Server::open()` walks with `resolve()` and answers `Action::Subtree` for a Subtree node; a Subtree node is listed as a directory
- `system/lib/ioservice/include/ioservice/loop_server.hpp`, `src/loop_server.cpp` — `Delegate::subtree(value, rest, flags, channel)`, which closes the channel unless overridden; the glue's `Action::Subtree` case
- `system/lib/ioservice/include/ioservice/outgoing.hpp`, `src/outgoing.cpp` — `SubtreeHandler`; `Outgoing::add_subtree(path, handler)`; the handler table's entry is a `HandlerEntry`; `judge()` and `make_parents()`, which `add_directory(path)` and `add_subtree()` share; the private `subtree()` override
- `system/lib/ioservice/test/{tree,server,loop_server,outgoing}_test.cpp` — 19, 9 and 22 tests more in `ioservice` (49 to 68, with 8 in `server_test.cpp` and 11 in `tree_test.cpp`), `loop_server` (21 to 30) and `outgoing` (41 to 63)
- `system/lib/ioservice/README.md` — the node kind, `Action::Subtree`, `Delegate::subtree()`, `add_subtree()`, the shared limit, the storage size

**Symptom**
Latent, cannot be triggered before `examples/files` uses it: a program could put directories, files and service nodes at a declared path, and nothing else. A filesystem cannot be written that way: it looks names up in its own code, so the Open has to leave the core with the part of the path the core does not know. The change is step 6 of 9 of `.agents/tasks/directory-capability/DESIGN.md` (decision 7, option A).

**Cause**
`Server::open()` ended every Open in a node of the tree: a connection for a directory or a file, `Action::Connect` for a service. A walk that met something the program serves itself had nowhere to stop. The design's answer is a node kind that stops the walk and gives the Open, whole, to the program. The decisions inside it, which the design left to this step:

- **The rest of a path that ends on the node is `.`, not empty.** An Open of the node itself leaves nothing of its path. `.` is a valid ours.io path, and is what an Open of a directory's own connection says for the directory itself, so the program can hand it on to a server of its own as it is, with no case for "empty". Empty is kept for "the walk met no Subtree node", so `Resolved::rest` is empty exactly when the node is not a Subtree node, and `lookup()` can tell the two apart in one test (`rest` empty or `.` means the path ended on a node; anything else went on past a Subtree).
- **`resolve()` stops at the first Subtree node and looks at nothing after it.** The segments after the node are the program's: they may not exist in this tree, and the program may use names this tree would refuse (not for a valid path, but for ones its own tree holds). The whole path is still judged first (`is_valid_path()`), so an invalid segment after the node is `BadPath` and never reaches the program.
- **`lookup()` is `resolve()` plus one test.** It reaches a Subtree node, and refuses to go through one with `NotDir`, as it refuses a path through a file. One walk instead of two keeps the order of the errors (`BadPath`, `InvArgs`, `NotDir`, `NotFound`) the same by construction. `Outgoing::judge()` still relies on `lookup()` returning the node of a declared path and stopping there.
- **The Open's checks 1 to 7 run on the connection the Open came on, then the walk (8), and then the Subtree node is the answer.** The checks are the connection's: a path with a bad segment, a connection without TRAVERSE, rights beyond the connection's, CONNECT for a service, a creation flag, what the design cut, a name missing or a file before the node. All of them refuse before the program is asked, and a test for each places a Subtree node behind it. Check 9, the node's own rules (a directory refuses `FileAppend`, a file refuses write), is the program's for what lies below.
- **The flags go on unchanged.** They are the consumer's own and have passed check 3, so they ask for no more than the connection holds. That is the whole of "rights only shrink" for the program: it must make what it makes with these flags and no wider ones. A mutation that adds a right, or hands over the connection's rights in their place, is caught.
- **No connection is made.** The channel is the program's from the call on, as a service node's is. A test fills the table so that one more connection would be refused, and shows Subtree Opens still reach the program.
- **The delegate's function is `subtree()`, and its default closes the channel.** `connect()` is pure, so a delegate must say what a service is; a program that uses no Subtree node has no reason to write `subtree()`, and the Open it receives anyway (a bug) must not leave the channel open for ever: the default takes it. A `LoopServer` with no delegate closes it too, as it does for a service. The same two cases have a test each.
- **`rest` views the request buffer, and is valid for the call only.** One buffer serves every callback, so the next read overwrites it. A test shows the stale view reading the next request's bytes, and a handler that parks the Open copies it and answers later.
- **A Subtree node sits at a declared path only.** The design says "at a declared path", and `Outgoing` keeps the nodes it declares in one table (`Storage::directories`) so that the overlap rule is a walk over segments. A Subtree node below a directory `add_directory(path)` returned would be a second kind of place to account for, in the overlap rule, in `in_declared_tree()` and in the compiler's twin of the rule, for a case nothing needs yet: a static tree with a mounted filesystem inside it. `Tree::add_subtree(parent, name, value)` has no such restriction, so a program's own tree (the example's) can hold one anywhere. `Outgoing::add_subtree(path, handler)` makes the node at the end of the path and every missing directory on the way, under the rules and the order of refusals of `add_directory(path)`; the two share `judge()` (syntax, `svc/`, overlap, room for the nodes) and `make_parents()`.
- **The handler table is shared, and its entry is two pointers.** The design's decision: `MAX_PROTOCOLS` stays 8 and now limits protocols, service nodes and Subtree nodes together. A protocol's handler is a `Handler` and a Subtree's a `SubtreeHandler`, two unrelated interfaces, so the table's entry is a `HandlerEntry` with a pointer for each, of which exactly one is set. That is 8 bytes more per entry, 64 in all (`Storage` 13,000 to 13,064). The alternatives were a `void *` and a tag (a cast the compiler cannot check), a base class common to the two handlers (the standing rule is to ask first, and the two functions have nothing in common), and a second table (a node's value would then have to say which). `connect()` and `subtree()` each refuse an entry of the other kind by closing the channel, which only a bug in `Outgoing` could reach. Refusals leave nothing behind: the table is judged after the path and before the first node is made, as `add_service()` judges it.
- **`SubtreeHandler::open(rest, flags, channel)`.** The name follows what the handler does with the call, as `Handler::connect()` follows `connect`. The class has a protected non-virtual destructor, for the reason `Handler` has one.

The Open still reaches the handler through `Outgoing`'s one `Delegate`: the value of the node is the index of its entry, and `Outgoing::subtree()` reads it as `connect()` reads a service node's.

What the program loses: a refused Open below the Subtree node is the handler's tree's, and `Outgoing::open_refused()` does not see it. Its console line is said for the outgoing root and `svc/` only (step 3), which is where the root's Opens arrive and where a manifest and a program that spell something differently show; the declared path itself, a typo between the two, is still said, because the walk fails before the node. The rest log in `examples/files` lost one line for this reason (see `examples-files.md`).

**Change**
```cpp
// before                                              // after
enum class NodeKind : u8 { Directory, File, Service };  enum class NodeKind : u8 { Directory, File, Service, Subtree };

                                                       struct Resolved { NodeId node; std::string_view rest; };
auto lookup(NodeId, std::string_view) const            auto resolve(NodeId, std::string_view) const -> Result<Resolved, Status>;
                                                       auto add_subtree(NodeId parent, std::string_view name, u64 value);
                                                       auto subtree(NodeId node) const -> Option<u64>;
```
```cpp
// server.cpp, Server::open(), after check 7
// before                                              // after
auto const found = m_tree->lookup(...);                auto const found = m_tree->resolve(...);
...                                                    if (kind == NodeKind::Subtree) {
                                                           return ustl::ok(Outcome{.action = Action::Subtree,
                                                               .handle = request.object,
                                                               .service = *m_tree->subtree(node),
                                                               .rest = found->rest,
                                                               .flags = flags});
                                                       }
```
```cpp
// outgoing.hpp
class SubtreeHandler {
  public:
    virtual auto open(std::string_view rest, ours_io::Flags flags, ours::Uandle channel) -> void = 0;
  protected: /* as Handler */
};
auto Outgoing::add_subtree(std::string_view path, SubtreeHandler &handler) -> ustl::Result<NodeId, ours::Status>;
// Storage::handlers: std::array<Handler *, 8>  ->  std::array<HandlerEntry, 8>
```

**Reference**
The node is the remote directory of Fuchsia's vfs, which hands the Open on with the rest of its path and the flags (`RemoteDir::open()`, `src/storage/lib/vfs/rust/src/remote.rs:108-123`, forwarding `path.as_ref()`), without the policy for a remote that died (a non-goal of the design). The difference is that the remote owns a client end and forwards to it, where this node gives the program the channel and lets it choose, which a filesystem that serves from its own tree can. A path that is left over is a `Path` there, and the path of the node itself is `Path::dot()` (`src/storage/lib/vfs/rust/src/path.rs:22`), which is the choice made here for "." as the rest.

**Verification**
- Host tests, all pass: `ioservice` 68 (was 49), `loop_server` 30 (was 21), `outgoing` 63 (was 41). The new ones cover:
  - `Tree`: the node's kind, value (zero and a wide one) and name; its place in a listing; nothing added below it; the adders' refusals and `available()` after them; `resolve()` stopping at the node, the rest for the node itself (`.`), for one segment and for several, from a directory further down, as a view of the path; no segment after the node looked at; each of two nodes with its own; no Subtree node met means an empty rest; the refusals up to the node, an invalid segment after it, and a Subtree node as a place to start from; `lookup()` reaching the node and not going through it.
  - `Server`: the value, the rest, the flags unchanged for nine kinds of flags (a protocol bit that does not fit, the file flags a directory refuses, a bit nobody defines) and the Open's own channel; no connection (the table full of other connections still lets the Open through); sixteen Opens that the connection's checks refuse, one for each, never reaching the program; a refusal says the whole path asked for; a Subtree node is listed as a directory; `bind()` to it is `NotDir`.
  - `LoopServer`: the delegate gets the value, the rest, the flags and the channel, and the channel is not written or closed; `.` for the node itself; no connection and no slot (many more Opens than connections); the rest is a view inside the request buffer and reads the next request's bytes afterwards; an Open parked and answered later with an epitaph on its channel after other traffic; the default closes the channel; no delegate closes it; an Open the connection refuses never reaches the delegate and is reported as a refusal; Opens arrive in order.
  - `Outgoing`: the node and the directories on the way before and after `serve()`; the rest, the flags and the channel at the handler for the node itself and below it; the flags for five kinds, unjudged; the connection's checks (rights beyond the entry's, a path that is not valid, creation, a node-only connection, a name not on the way, no CONNECT for a service, no TRAVERSE) before the handler; two nodes, two handlers; a Subtree node beside a static tree and a protocol, each reached by its path; the node listed as a directory; the path refusals of `add_directory(path)` (`/svc`, `/svc/x`, relative, `/`, empty and `.` segments, a segment that is too long) with `/svcx` accepted; the overlap with a declared path of either kind, equal, above and below, with siblings accepted; a refusal for room, for the table and for an invalid path leaving nothing behind (no directory, no handler slot); nothing added below the node; eight of any mix of protocols, service nodes and Subtree nodes in four different orders and then `NoResource` for each kind, every Open reaching its own handler; a handler that parks an Open and answers it later.
- Mutation checks of the sources, 48, each a change to one source built, run and reverted (the script is in the session scratchpad, not in the tree; the tree was compared with a copy after, and was identical). 47 fail a named test, and the last (`MAX_PROTOCOLS` of 7) fails to build the tests, since the older `AddProtocolRefusals` names eight protocols. Two of the lookup mutations first crashed a test binary, because `unwrap_err()` traps on a call that succeeded; the tests that name them now read a status through a helper that returns Ok for a success, and fail instead. The counts are of failing tests over the three binaries:

  | Mutation | Caught by (first) |
  |---|---|
  | `resolve()` does not stop at a Subtree node | 28 tests in all three binaries |
  | it stops only at a node that ends the path | 27 |
  | the rest for the node itself is empty | `ResolveStopsAtASubtreeNodeAndReturnsTheRestOfThePath` and three more |
  | the rest is the whole path / the last segment / all but the first | 18 / 5 / 5 tests |
  | `lookup()` walks through a Subtree node / refuses one that ends the path | `LookupWalksToASubtreeNodeButNotThroughIt`, `AddSubtreeRefusesAPathThatOverlapsADeclaredOne` |
  | `add_subtree()` stores value 0 / makes a directory; `subtree()` answers for any node | 9 / 26 / 1 |
  | the Open reaches the program before check 2, 3, 4, 6 or 7 | 4 / 4 / 2 / 2 / 2 tests, the cases of the table in `TheConnectionsOwnChecksRunBeforeTheProgramIsAsked` and `AnOpenIsCheckedOnTheConnectionItCameOnBeforeTheHandlerIsAsked` |
  | the flags handed over carry the connection's rights / one right more / the rights alone / no protocol bits | 3 / 5 / 5 / 5 |
  | the rest handed over is the whole path; the value is 0; the channel is the connection's | 14 / 7 / 14 |
  | a connection is made for a Subtree Open | 3 (`AnOpenOfASubtreeNodeMakesNoConnection`, the same in `LoopServerTest`) |
  | a Subtree node is listed as a file | 2 |
  | the glue does not call the delegate; no delegate leaves the channel open; the default leaves it open | 17 / 1 / 1 |
  | the delegate gets the rest `.` / no flags / the value 0 | 12 / 4 / 4 |
  | the directory's connection closes after a Subtree Open | 14 |
  | `Outgoing::subtree()` reads the service half of the entry; always the first handler; hands over `.`; hands over no flags | 10 / 2 / 8 / 3 |
  | `add_subtree()` does not count its handler; has no table check; checks the table after the directories are made; checks it one early | 13 / 10 / 1 / 10 |
  | it does not record the declared node; answers the parent; makes a directory; carries value 0 | 2 / 1 / 15 / 3 |
  | `judge()` does not refuse `/svc` | 3 |
  | `MAX_PROTOCOLS` is 9 / 7 | 11 tests / the tests do not compile |

- `boot_check.py --out out/dc-step6`: dbg PASS, 44 passed, 4 skipped, 0 failed; rel PASS, 41 passed, 8 skipped, 0 failed, 7 of 7 required; host tests 151 passed (the same executables, with more tests in them).
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step6`) on the four sources and the four tests: no diagnostic on any line of `system/lib/ioservice`; what the tests print is googletest's header. `format_cpp.py --check` clean.
- Sizes: `Outgoing::Storage` 13,064 bytes (13,000); the rest is in `examples-files.md`.

**Deliberately not done**
- The shared Open-check function that a Subtree node's users would want (the design's open question, left out of this step). The check order is private to `Server::open()`; a handler that wants it repeated for its own tree writes a message, as `examples/files` does (`examples-files.md`).
- A Subtree node below a directory `add_directory(path)` returned, in `Outgoing`.
- A policy for a handler that is slow or gone: the channel is the program's.
- A console line for a refusal below the node.
- Any change to the guide: the passages are in the task note.

**How it was found**
None. A new feature, from the design. Two things the design left open and this step settled: the rest for the node itself, and the shape of a shared table for two handler interfaces.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `Resolved::rest` is `.` when the path ends on the node and empty when the walk met no Subtree node. What would a handler do with an empty rest that it does not do with `.`, and how does `lookup()` use the difference?
2. The Open's flags reach the handler exactly as the consumer sent them, and the handler's tree holds the same Open to its own checks. Which check on which connection is the ceiling for the rights, and what would go wrong if the core replaced the flags by the connection's rights?
3. Why does the table's entry hold two pointers, and what would a `void *` and a tag have cost?
