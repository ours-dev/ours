# ioservice changes

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
