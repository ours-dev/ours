# directory-capability-step3: an outgoing directory holds a directory capability's tree at its declared path

**Date**: 2026-10-30

## Delivered
Branch `dc-step3`, worktree `.worktrees/dc-step3`, one commit on `605f93fa`: `feat(ioservice): an outgoing directory holds a directory capability at its declared path`. It is step 3 of the Staging table of `.agents/tasks/directory-capability/DESIGN.md`:

- `Outgoing::add_directory(path) -> ustl::Result<NodeId, Status>`: the absolute path as declared; makes the directory at its end and every missing one on the way (`export/` once for `/export/a` and `/export/b`) and returns the last node. InvArgs for a path that is not absolute, is `/`, has an empty, `.` or `..` segment or an invalid name, or is `/svc` or under it (by segment: `/svcx` is fine); AlreadyExists for a path equal to, above or below one already added (by segment); NoResource when the segments left do not fit. The checks run in that order and nothing is made until the last has passed, so a refusal changes nothing.
- The tree is filled in below the returned node with `Outgoing::add_directory(parent, name)` and `Outgoing::add_file(parent, name, file)`. **The adders are wrapped; `tree()` is not exposed.** A reachable tree lets a program add a service node whose value `Outgoing::connect()` reads as an index into its handler table, and a node at the root that no declaration accounts for; both break an invariant `Outgoing` owns. Step 5's `add_service(path, handler)` has to go through `Outgoing` for the same reason.
- `MAX_CONNECTIONS` 4 to 32 and a new `MAX_NODES` of 32 (decision 8 (b)); `Storage::nodes` is sized by `MAX_NODES`. The nodes are one pool: `2 + MAX_PROTOCOLS` of them are the root, `svc/` and the protocols only once all eight are added. A program that wants all its protocols adds them before a large tree.
- `Tree::available()`, the room left in a tree's storage: `add_directory(path)` needs it to refuse for room before it makes the first node. `Tree::allocate()` asks it too, so there is one source of the limit.
- `Outgoing::open_refused()` now says its console line only for an Open refused through the outgoing root or `svc/` (see "Deviations").
- The ioservice README, the change record `docs/changes/261030/ioservice.md` (created), and this note.

Echo's and hello's sources do not change. Their Loop tables follow the constant (36 and 33 slots, against the port's limit of 1,024). The dev image does not change in behaviour: no program calls `add_directory()` and the root still answers `NotSupported` for a directory that ends at a provider.

## Verification
- `boot_check.py --out out/dc-step3`, before (`605f93fa`) and after, the same counts:

  | | Before | After |
  |---|---|---|
  | dbg | PASS, 44 passed, 4 skipped, 0 failed, 0 error lines | the same |
  | rel | PASS, 41 passed, 8 skipped, 0 failed, 6 of 6 required, 0 error lines | the same |
  | host tests | PASS, 151 passed (executables), 0 failed | the same |

- Tests (in existing executables, so the executable count stays 151): `outgoing` 14 to 31 tests, `ioservice` (server and tree) 48 to 49. The new ones cover `add_directory("/export/data")` making `export` and `data` and returning the last (a file added to it is read at `export/data/x`); two siblings sharing `export` (counted to the node); every refusal with its status and the tree untouched afterwards (22 invalid paths, then all 30 nodes still free); `/svcx`, `/svc2/data`, `/export/svc` accepted; equal, above and below at any depth refused while `/export/data2`, `/export/dat` and a sibling are accepted; NoResource leaving no half-made path; the pool shared with the program's files; the wrappers' refusals; an Open of `export/data` making a connection with the rights asked, a file under it reading, a subdir opened and `..` refused; rights beyond the connection's AccessDenied; a file and a protocol opened with `ProtocolDirectory` answering NotDir and not reaching the handler; the 32 connections running out with NoResource on the Open's channel and the next Open working after a client goes; protocols and directories side by side in both orders; the console line for a declared path the program does not serve, and none for a refusal below a declared path; and the decided sizes pinned as numbers.
- Mutation checks, 29, each a deliberate change to one source, built, run and reverted (script in the session scratchpad, not in the tree). Every one is caught: the refusal checks (relative path, `.` alone, validity, `svc` by prefix, `svc` not refused, only `/svc` itself), the overlap (declaration not recognised, equal-or-above not refused, not recorded, wrong node recorded, overlap by name prefix), the order of checks (no room check with a creation that returns its error and so leaves `x` behind, room counted one short, room counted from the whole path), the returned node, the console line (every directory, root only, nothing), the constants (`MAX_CONNECTIONS` 4 and 31, `MAX_NODES` 10, 31 and 33), `Tree::available()` off by one, and four mutations of `server.cpp` the Open tests rest on (a file or a service answering BadType, rights beyond the connection's not refused, a new connection given every right, the table running out answering the wrong status). Details in the change record. Two kinds of failure were crashes (an `unwrap()` that traps because the room count was wrong, `Tree::available()` off by one writing past the storage); those are the library trapping on its own bug, and still fail the run.
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step3`) on `outgoing.cpp`, `tree.cpp`, `outgoing.hpp` and both test files: no diagnostic on any line of `system/lib/ioservice`; what remains is googletest's header. `format_cpp.py --check` is clean. `lint_docs.py --changed` is clean on the change record and the README.
- Sizes, from the target build (`llvm-nm -S`, `llvm-size -A`), against the design's table:

  | | Before | After | Design |
  |---|---|---|---|
  | `Outgoing::Storage` | 9,496 B | 13,000 B | about 12.9 KB, +3,376 B |
  | echo's Loop table | 8 slots, 192 B | 36 slots, 864 B | +672 B |
  | echo `.bss` | 22,960 B | 27,136 B | about +4 KiB |
  | hello `.bss` | 22,816 B | 26,992 B | |
  | echo and hello `.text` | 26,670 / 26,590 B | 27,006 / 26,926 B | |

## Deviations from the design
- **`Storage::directories`, +128 B.** The overlap rule needs to know which nodes end a declaration, and the table of them is 32 node ids that the design's size table did not count. `Storage` is 13,000 B, not about 12,872. The other way was a flag in `Tree`'s `Node`, which fits in padding but would put a "mark" in a generic tree for one user; not taken, and cheap to change later.
- **The console line is narrower.** The design left to this step whether `open_refused` covers a refused Open below a directory node. It does not: it is said for the outgoing root and `svc/`, which is where the root's Opens arrive and where a manifest and a program that spell something differently show. A connection of a declared directory is a consumer's, who reads its epitaph; saying it would let a consumer with a connection write a line on the provider's console for every Open it makes, and the old text ("the outgoing directory refused") would name the wrong thing. The design's remark that a typo between the manifest's path and the program's "reads as a NotFound ... with no line in any log" was already untrue for the root's Opens: the line was said for every refusal, whatever the depth, so `export/ghost` is logged (`ADeclaredPathTheProgramDoesNotServeIsSaidOnTheConsole`).
- **"A service or file at the declared path ... answers NotDir."** `add_directory` always makes a directory at the declared path, so no file or service can be at it. The case that exists is a route whose declared path joined with its subdir ends at a file, or at a service under it, and that is what the tests open: `export/data/x` and `svc/<name>` with `ProtocolDirectory`. Both answer NotDir from the server as it was. Nothing in `server.cpp` changed.
- **The node pool is shared.** The design says `2 + MAX_PROTOCOLS` of the 32 nodes are the root, `svc/` and the protocols, and 22 the directories'. It is one pool, not a partition: a provider with no protocol can use 30 nodes. The cost is order: `add_protocol()` can answer NoResource for room if a large tree came first. A reserved share would make the 22 literal and cost 8 nodes in every provider that serves fewer protocols. Left for the maintainer; changing it is one comparison in `add_directory` and the two wrappers.
- **`Tree::available()` is new public interface** of a class the design did not name. It is one read-only accessor, with a test, and `allocate()` uses it.

## Found along the way
- A test that expects a refusal and calls `unwrap_err()` traps the whole test binary when the call succeeds, so a mutation that makes a refusal succeed showed as a crash, not a failing test. The new tests read a status through a helper that returns Ok for a success.
- `docs/guide` cites `outgoing.hpp`, `outgoing.cpp` and `tree.cpp` by line in many places, and the header grew by about 40 lines. The table below lists the cites; the guide is not edited here.
- No repair item written.

### Guide and README passages that are now wrong
The ioservice README is updated. The guide is not (the brief):

| Passage | Says | Now |
|---|---|---|
| `docs/guide/04-tree-servers.md:148-152` | `Storage` with `std::array<ioservice::Node, 2 + MAX_PROTOCOLS> nodes` | `MAX_NODES` nodes, and a `directories` array of node ids |
| `04-tree-servers.md:164` | "`Outgoing` has room for its directory, `svc/`, and one service node per protocol: `2 + MAX_PROTOCOLS`" | `MAX_NODES` (32), a pool that also holds the declared paths and their trees |
| `04-tree-servers.md:168` | "An `Outgoing` costs about 9 KiB" | 13,000 B |
| `04-tree-servers.md:233` | the tree is "the outgoing directory, its `svc/` directory and a service node per protocol" | and the trees of directory capabilities |
| `01-concepts.md:436` | "Under `svc/` it holds one entry for each protocol"; "a connection for each handle open on the directory or on `svc/`" | and a directory at each declared path; connections of a consumer's entry too |
| `06-checklist.md:97` | "Open connections to the outgoing directory: 4" | 32; and "Nodes in one tree" may cite `MAX_NODES` |
| `04-tree-servers.md:166` and `02-first-server.md:259` | the Loop's slots follow `MAX_CONNECTIONS` | still true; the value is 32 |
| `05-testing-and-debugging.md:397`, `:411` | the provider logs "refused an Open of `svc/<name>`" | still true for the root's Opens; a consumer's refusal below a declared path is not logged |

Cites that moved (guide line, what it cites, old to new):

| Guide | Cites | Old | New |
|---|---|---|---|
| `04-tree-servers.md:60`, `:233`; `02-first-server.md:225` | `class Handler` | `outgoing.hpp:59`, `:59-71` | `outgoing.hpp:79`, `:79-91` |
| `04-tree-servers.md:60` | `class Outgoing` | `outgoing.hpp:75` | `outgoing.hpp:95` |
| `04-tree-servers.md:255`, `06-checklist.md:96` | `MAX_PROTOCOLS` | `outgoing.hpp:78` | `outgoing.hpp:98` |
| `04-tree-servers.md:255`, `06-checklist.md:97` | `MAX_CONNECTIONS` | `outgoing.hpp:85` (`80-85`) | `outgoing.hpp:108` (comment from `100`) |
| `04-tree-servers.md:170` | the sizes comment of `Storage` | `outgoing.hpp:101` | `outgoing.hpp:137` |
| `04-tree-servers.md:143` | `struct Storage` | `outgoing.hpp:105` | `outgoing.hpp:141` |
| `04-tree-servers.md:255` | `add_protocol`'s refusals | `outgoing.hpp:129` | `outgoing.hpp:170` |
| `04-tree-servers.md:255` | `serve` | `outgoing.hpp:141` | `outgoing.hpp:228` |
| `04-tree-servers.md:138` | `unwrap()` for a call that cannot fail | `outgoing.cpp:63` | `outgoing.cpp:78` |
| `04-tree-servers.md:176` | the `LoopServer` built over the `Storage` | `outgoing.cpp:52` | `outgoing.cpp:66` |
| `04-tree-servers.md:369` | `add_service` with the handler's index | `outgoing.cpp:73` | `outgoing.cpp:88` |
| `04-tree-servers.md:139` | `is_valid_name`'s declaration | `tree.hpp:200` | `tree.hpp:206` |
| `05-testing-and-debugging.md:434` | the `NotFound` walk | `tree.cpp:204` | `tree.cpp:211` |
| `04-tree-servers.md:139` | a walk only goes down | `tree.cpp:192` | `tree.cpp:199` |
| `05-testing-and-debugging.md:434` | the line, `outgoing.cpp:99-115` | `open_refused()` | `outgoing.cpp:196-218` |
| `01-concepts.md:204`; `06-checklist.md:147`, `:179`, `:296` | the one-thread sentence (`51`); the `Handler` comment (`55-58`); `Handler::connect`'s comment (`61-63`, `61-64`) | `outgoing.hpp:51`, `:55-58`, `:61-63`, `:61-64` | `outgoing.hpp:71`, `:75-78`, `:81-83`, `:81-84` |
| `01-concepts.md:484` | the class comment, "the outgoing directory, `add_protocol` and `serve`" | `outgoing.hpp:18` | starts at `outgoing.hpp:18` still, and is longer |

(The `outgoing.hpp`, `outgoing.cpp` and `tree` lines are read off the files of this commit. The `outgoing_test.cpp` cites of `05-testing-and-debugging.md:434` are not recomputed: the file gained 17 tests, so find the test by name, `OpenOfAnUnservedNameIsSaidOnTheConsole`.)

## Not done
- The root's `forward_directory()` and `examples/files`: step 4. A directory that routes to a provider is still `NotSupported`. When it lands, the Open it writes is `Route::path` without its leading `/`, then `/<subdir>` when the joined subdir is not empty, on the provider's first connection.
- `add_service(path, handler)` (step 5) and `add_subtree()` (step 6).
- The guide and the design notes, as listed above.

## For step 4
- The root's Open must be the declared path **without** its leading `/` (`export/data`) plus `/<subdir>`; `Route::path` has the slash. `Outgoing` answers an Open with a leading `/` as BadPath.
- `examples/files` should call `add_directory("/export/docs")` once, keep the returned node, and add `hello.txt`, `numbers.txt` and `sub` with the wrappers. The paths and names it passes must outlive the `Outgoing` (the nodes view them): string literals do.
- A manifest that declares `/export/ghost` while the program serves nothing there reaches the provider as `ioservice: the outgoing directory refused an Open of "export/ghost": NotFound` on the provider's console, once for each consumer that routes to it, and that is the only trace of it besides the consumer's epitaph.
- `examples/files` is a provider with no protocol, so all 30 non-root nodes are its own; its tree (`export`, `docs`, `hello.txt`, `numbers.txt`, `sub`, `nested.txt`, `ghost` served or not) is a handful of them.
- A consumer's `fsio::export_namespace()` clone of an entry costs a connection; the 32 are shared by the root's own and every consumer's entry, as in the design.
