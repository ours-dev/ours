# directory-capability-step6: a Subtree node hands an Open and the rest of its path to the program, and files serves docs through one

**Date**: 2026-10-31

## Delivered
Branch `dc-step6`, worktree `.worktrees/dc-step6`, one commit on `600ca22c`: `feat(ioservice): a Subtree node hands an Open and the rest of its path to the program`. It is step 6 of the Staging table of `.agents/tasks/directory-capability/DESIGN.md` (decision 7, option A):

- **Core** (`system/lib/ioservice`): `NodeKind::Subtree`; `Tree::add_subtree()`, `Tree::subtree()`, and `Tree::resolve(directory, path) -> Resolved{node, rest}`, which stops at the first Subtree node and returns the rest of the path after it (`.` when the path ends on the node, empty when the walk met no Subtree node; `lookup()` is `resolve()` plus one test). `Server::open()` runs checks 1 to 7 and the walk on the connection the Open came on, then answers the new `Action::Subtree` with the node's value, the rest, the flags unchanged and the channel (new `Outcome::rest` and `Outcome::flags`), and makes no connection. A Subtree node is listed as a directory. `LoopServer::Delegate::subtree(value, rest, flags, channel)` is called inside the Binding's callback as `connect()` is; unless overridden it closes the channel, and a `LoopServer` with no delegate closes it too. `rest` views the request buffer and is valid for the call only (documented, and a test shows the stale view).
- **`Outgoing::add_subtree(path, handler)`**: `handler` is a `SubtreeHandler`, an abstract class like `Handler` with `open(rest, flags, channel)`, a protected non-virtual destructor. It makes a Subtree node at the end of `path` and every missing directory on the way, under the rules of `add_directory(path)` (they share `judge()` and `make_parents()`): InvArgs for a path that is not valid or is `/svc` or under it, AlreadyExists for one equal to, above or below a declared path of either kind (by segments), NoResource for the nodes and for the table, all judged before anything is added. It returns the node. The handler goes in the table `add_protocol()` and `add_service()` fill, whose entry is now a `HandlerEntry` (a pointer for each handler class, one of them null): `MAX_PROTOCOLS` stays 8 and limits the three kinds together, and the doc comments and the README say so. Nothing public was renamed.
- **A Subtree node sits at a declared path only**, not below a directory `add_directory(path)` returned. The design says "at a declared path"; a node below a static directory would be a second kind of place in the overlap rule and in `in_declared_tree()` for a case nothing needs yet. `Tree::add_subtree(parent, name, value)` has no such restriction, so the handler's own tree can hold one anywhere. Said in `docs/changes/261030/ioservice.md`.
- **`examples/files`**: `/export/docs` is a Subtree node. Its handler, `DocsTree`, owns a second `Tree` (root, `hello.txt`, `numbers.txt`, `stop`, `sub/nested.txt`) served by a second `LoopServer` on the same Loop, and `open()` writes the Open it was handed, unchanged, on the root connection of that server (a channel pair it made at start), so the server judges it with the checks it already has and makes the connection on the consumer's channel with exactly the rights in the flags. The service node `stop` lives in that tree; an Open of it reaches the `LoopServer::Delegate::connect()` of `DocsTree`, which logs and exits with 0 as before. `/export/ghost` is declared and never served. The files and listing consumers see, the rights, the subdir and the refusals are unchanged.
- **routing_checks**: no case changed; one comment (`refused_by_provider()`) names the extra hop.
- Documents: `docs/changes/261030/{ioservice,examples-files}.md` (sections appended), `docs/repairs/261029/routing-checks.md` (item 2 updated, cites moved), `docs/repairs/261031/fsio.md` (item 1 updated), the ioservice README, this note. `routing-checks.md` of the change records is not touched, since no case changed.

## Verification
- `boot_check.py --out out/dc-step6` on the final tree, against step 5's counts:

  | | Step 5 | Now |
  |---|---|---|
  | dbg | PASS, 44 passed, 4 skipped, 0 failed, 0 error lines | the same |
  | rel | PASS, 41 passed, 8 skipped, 0 failed, 7 of 7 required, 0 error lines | the same |
  | host tests | PASS, 151 passed (executables), 0 failed | the same; `ioservice` has 68 tests (was 49), `loop_server` 30 (21), `outgoing` 63 (41) |

  The target cases are not in the shell's counts: they run in `routing_checks` on rel and show as the required `routing_checks: passed` and `init: files exited with 0`. dev.dbg_1 has no root, so it starts neither `files` nor `routing_checks`.
- **Repeated rel boots, on the final build** (`--no-build --no-tests`): 12 with 4 CPUs and 12 with `SMP=2` first. All 12 `SMP=2` boots pass. Of the 4-CPU boots 11 pass and one, the twelfth, failed with `QEMU exited (rc 0) before the shell finished`, after 11 shell checks. **I have no log of that boot**: the next boot overwrote it, and I cannot say what ended it. I then ran 4-CPU boots until the first failure with the logs of a failing boot copied aside: 80 more passed, 150 passed under four busy processes, and 246 passed on an idle machine before the 247th failed the same way. That failure I have the log of, and it ends `qemu-system-x86_64: terminating on signal 15 from pid 39829 (<unknown process>)`, during the kernel's boot, with 0 shell checks run. QEMU exits 0 on SIGTERM, which is the "rc 0". It was not a guest reset. The same SIGTERM, from the same pid, ended the untouched baseline (`600ca22c`, built in a worktree of its own and booted concurrently, 150 clean boots under load and 246 clean before it) in the same iteration, and no process of mine sends a signal to another's QEMU: each `boot_check` stops its own, and the two loops were two processes. Another session on this host has its own QEMU running (the `fs-service` worktree), and the final verification run was SIGTERMed once from outside in the same way (pid 40989, an image that then passed on the rerun). So an outside process kills the QEMUs on this host now and then, and the one failure with a log was that. The first failure has the same one-line signature and I think it was the same, but I did not prove it. In all, 499 4-CPU boots passed on my build against two failures, and 396 on the baseline against one.
- Host mutations of the sources (48): 47 fail a named test and one (`MAX_PROTOCOLS` of 7) stops the tests from compiling. Table in `docs/changes/261030/ioservice.md`.
- Target mutations of the example (11, each booted on rel): ten are caught by `FAILED` lines or by the missing required lines; one (`WRITE_BYTES` added to the forwarded flags) passes and is equivalent for these cases. Table in `docs/changes/261030/examples-files.md`. The handler is on the path: with it closing every Open, 18 `FAILED` lines; with `docs` a plain directory in place of the Subtree node, 22.
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step6`) on the four sources, the four tests, `files_server.cpp` and `routing_checks.cpp`: no diagnostic on any changed line; the tests print googletest's header. `format_cpp.py --check` and `format_gn.py --check` clean on every changed file. `lint_docs.py --changed` clean on every changed document.
- Sizes (`llvm-size -A`, `llvm-nm -S` of the user ELF `files_server`): `.text` 25,646 to 28,526; `.bss` 18,496 to 34,496; the Loop's table (`SLOT_STORAGE`) 768 to 1,536 bytes, 32 to 64 slots against the port's limit of 1,024; `OUTGOING_STORAGE` 13,000 to 13,064; the new `DOCS_STORAGE` 15,176 (8 nodes, 32 connections and Bindings, the three message buffers, and 4,096 for the forwarded Open); `kernel.dev.obi` 1,018,448 to 1,020,544; `kernel.dev.dbg_1.obi` 954,512, unchanged. The pools: the example's `DOCS_CONNECTIONS` is 32 and its nodes are 6 of 8 used; the outgoing directory's 32 connections now carry the root's one.

## Deviations from the design
- **The example forwards the Open through a root connection of its own tree** rather than "opening its tree on S" directly. The design says the Open checks are not a function the program can call (the shared Open-check function is left out), and a handler that walked `rest` itself and called `LoopServer::serve()` would have to repeat check 9 for `/files-file` and every other rule of the node. A message to the tree's own server reuses the checks and the rule for "exactly the requested rights" (the permission bits of the flags read as rights) by construction. Costs: one connection, one channel pair, one Loop slot, one Loop turn per Open, a 4,096-byte buffer. A shared Open-check function would remove all of them; it is the maintainer's to ask for (the design's open question).
- **The table's entry is two pointers** (`HandlerEntry`), +8 bytes an entry, `Storage` 13,000 to 13,064. The design says only that the table is shared.
- **A Subtree node at a declared path only** (above).
- **The provider's console lost one line**: a refusal inside `docs`' tree (`/files-file`'s NotDir) is the tree's server's, and the outgoing directory says only the refusals at its root and `svc/` (step 3). `export/ghost` is still said. Recorded in the change records and in `fsio.md` item 1.
- The design's `add_service` for the stop node is no longer used by `examples/files` (the node is in the handler's tree), but stays in `Outgoing`, with its tests.

## Found along the way
- A test that expects a refusal and reads it with `unwrap_err()` traps the whole binary when the call succeeds, as step 3 found. The new `tree_test.cpp` cases read a status through helpers that return Ok for a success; two mutations that first crashed a binary now fail the named test.
- `MAX_PROTOCOLS` of 7 does not compile with the test file (`AddProtocolRefusals` names eight), so it is caught by the build; 9 is caught at run time by the new table tests, which count eight where the older ones loop to the constant.
- `WRITE_BYTES` added to the forwarded flags passes every routing_checks case: a file of this tree is never writable and no case opens a directory of `docs` for write. `MODIFY_DIRECTORY` added instead is caught. Recorded in `examples-files.md`.
- No new repair item.

## Not done
- The shared Open-check function; a Subtree node below a static directory; a console line for a refusal inside the handler's tree; sharing the message buffers of the two servers (8.4 KB); steps 7 to 9 (the Loop's `Suspend`/`resume`, the handler node, its glue).
- The guide and the design notes, as the brief says. The passages below are wrong or stale now.

### Guide passages that step 6 makes wrong (in addition to the notes of steps 2 to 5)
| Passage | Says | Now |
|---|---|---|
| `docs/guide/04-tree-servers.md:46`, `:60`, `:216-226` (the table of outcomes), `06-checklist.md` where it counts them | an `Outcome` is one of six actions: `Reply`, `ReplyAndClose`, `Drop`, `Bind`, `Connect`, `Refuse` | seven: `Subtree` too, with `LoopServer` calling `Delegate::subtree()` (or closing the channel with no delegate) and answering `Rearm` |
| `04-tree-servers.md:48`, `:55` (the diagram), `:359` | a `Delegate` is called when an Open reaches a service node; "two more functions" (`write_failed`, `open_refused`) | it is also called for a Subtree node, by a third function, `subtree()`, which closes the channel unless overridden |
| `04-tree-servers.md:60`, `:64-70` (the quoted `NodeKind`) | "the three node kinds"; "A node has one of three kinds" and the enum without `Subtree` | four kinds; `Subtree` is a place the program serves with a tree of its own; `Tree::resolve()` is new |
| `04-tree-servers.md:150-152` (the quoted `Storage`) | `std::array<Handler *, MAX_PROTOCOLS> handlers;`, "indexed by the value each service node was added with" | `std::array<HandlerEntry, MAX_PROTOCOLS>`, indexed by the value of a service node or of a Subtree node; 13,064 bytes (already stale since step 3) |
| `04-tree-servers.md:253`, `06-checklist.md:96` (already on step 5's list) | `MAX_PROTOCOLS` is a limit on the protocols and service nodes | and Subtree nodes |
| `04-tree-servers.md:283-316` (the nine checks and the table of step 9) | step 8 is the walk with `lookup()`; step 9 has three node kinds, and a passing request "gets a connection or `Connect`" | step 8 is `resolve()`, which stops at a Subtree node; the Open then answers `Subtree` before step 9 and makes no connection; the table of step 9 does not have the row |
| `04-tree-servers.md:369` | `Outgoing` adds each protocol with the index of its handler and looks the handler up by that index | the table is of `HandlerEntry`; `connect()` and `subtree()` each read their half |
| `04-tree-servers.md:377` | an Open of `/svc/...` passes through "two tree servers" | a directory use of `files` passes through three servers: the root's forward, the outgoing directory and the server of `docs`' tree |
| `05-testing-and-debugging.md:275`, `README.md:70`, and the sample logs | every rel log holds two `ioservice: ... refused an Open of "export/ghost"` / `"export/docs/hello.txt"` lines from `files`' console | one: `"export/ghost"`; the NotDir refusal is inside `docs`' tree and not said |
| `05-testing-and-debugging.md:431`, `:434` | the lookup `tree.cpp:204`; `lookup()` is what Open walks with | `Tree::resolve()` (`tree.cpp:218`); `lookup()` (`:202`) calls it |
| `06-checklist.md:145` | give a class you derive handlers from a protected non-virtual destructor, "as `ioservice::Handler` does" | and `ioservice::SubtreeHandler`, and `LoopServer::Delegate` |
| `01-concepts.md:438`, `:484`, `02-first-server.md:223`, `04-tree-servers.md:233` | the outgoing directory holds protocols, and (step 5) service nodes | and Subtree nodes: a declared path served by the program's own code |
| `docs/architecture/component-framework/glossary.md:48`, `docs/design/261004/component-framework.md:69`, `libraries.md` | ioservice has "three node kinds"; a `LoopServer` has a `Delegate` for service nodes | four; a Subtree node |

### Guide cites that moved
Old lines are `600ca22c`'s, new lines are this commit's.

| Guide | Cites | Old | New |
|---|---|---|---|
| `04-tree-servers.md:60`, `:64` | `NodeKind` | `tree.hpp:37` | `:37` (the enum is longer, to `:52`) |
| `04-tree-servers.md:60` | `class Tree` | `tree.hpp:96` | `:118` |
| `04-tree-servers.md:60`, `06-checklist.md:98` | `class Node`, `add_service`, `lookup`, `kind`, `service`, `available`, `first_child` | `tree.hpp:68`, `:131`, `:145`, `:150`, `:164`, `:170`, `:175` | `:75`, `:153`, `:176`, `:192`, `:206`, `:217`, `:222` |
| `04-tree-servers.md:139` | `is_valid_name`, `is_valid_path` | `tree.hpp:210`, `:219` | `:257`, `:266` |
| `04-tree-servers.md:60`, `:229` | `enum class Action`, `struct Outcome`, `class Connection`, `class Server` | `server.hpp:78`, `:109`, `:132`, `:167` | `:78` (to `:107`), `:119`, `:149`, `:184` |
| `04-tree-servers.md:343` | `Server::dispatch`, `Server::open` (declaration) | `server.hpp:196`, `:240` | `:213`, `:259` |
| `04-tree-servers.md:60`, `:349`, `06-checklist.md:179`, `:287` | `class LoopServer`, `Delegate`, `connect` | `loop_server.hpp:48`, `:58`, `:64` | `:51`, `:61`, `:67` |
| `04-tree-servers.md:359`, `:316` | `write_failed`, `open_refused`, `Binding`, `Memory`, `serve` | `loop_server.hpp:69`, `:84`, `:111`, `:134`, `:177` | `:91`, `:106`, `:133`, `:156`, `:200` |
| `04-tree-servers.md:60`, `:233`; `02-first-server.md:225` | `class Handler` | `outgoing.hpp:84` | `:92` |
| `04-tree-servers.md:60` | `class Outgoing` | `outgoing.hpp:100` | `:144` |
| `04-tree-servers.md:255`, `06-checklist.md:96`, `:97` | `MAX_PROTOCOLS`, `MAX_CONNECTIONS`, `MAX_NODES` | `outgoing.hpp:108`, `:118`, `:127` | `:153`, `:163`, `:172` |
| `04-tree-servers.md:143`, `:170` | `struct Storage` | `outgoing.hpp:151` | `:205` |
| `04-tree-servers.md:255` | `add_protocol`, `add_directory(path)`, `add_service`, `serve` | `outgoing.hpp:192`, `:219`, `:270`, `:284` | `:246`, `:273`, `:324`, `:373` |
| `04-tree-servers.md:343`, `05-testing-and-debugging.md:431`, `:434`, `06-checklist.md:97` | `Tree::lookup` (the walk), `find_child`, `Tree::add_service` | `tree.cpp:183`, `:84`, `:174` | `:202` (`resolve()` follows at `:218`), `:94`, `:184` |
| `04-tree-servers.md:343`, `:469`, `05-testing-and-debugging.md:431`, `06-checklist.md:97` | `Server::open`, the walk (check 8), check 9, `Server::dispatch`, `read_dirents` | `server.cpp:235`, `:281`, `:288`, `:476`, `:361` | `:239`, `:285`, `:310`, `:497`, `:382` |
| `04-tree-servers.md:229`, `:316`, `06-checklist.md:185`, `:240`, `:261` | `LoopServer::handle`, `case Action::Connect`, `case Action::Refuse`, `watch` | `loop_server.cpp:92`, `:169`, `:179`, `:56` | `:99`, `:176`, `:198`, `:63` |
| `04-tree-servers.md:369`, `05-testing-and-debugging.md:434` | `Outgoing::add_directory(path)`, `add_service`, `serve`, `connect`, `open_refused` | `outgoing.cpp:125`, `:202`, `:230`, `:234`, `:248` | `:195`, `:246`, `:274`, `:278`, `:305` |
| `docs/repairs/261029/routing-checks.md` item 2 | `connect_status`, `Incoming::receive`, `check_files_stop` | `routing_checks.cpp:1002-1023`, `:574-592`, `:1118` | `:1005-1026`, `:577-595`, `:1121` (updated in the repair item) |

## For step 7
- The Loop's `Suspend`/`resume` does not touch what this step adds. A Subtree handler that parks an Open already works (copy `rest`, answer later on the channel: tested), because the Open is one-way and the channel is the handler's; `Suspend` is for the handler node of steps 8 and 9, whose connections the core keeps.
- `LoopServer::watch()` (`loop_server.cpp:63`) still drops the key `add()` returns, which step 9 needs in the `Binding`.
- The program that now runs two `LoopServer`s on one Loop is `files`: its Loop table is 64 slots, `Outgoing::MAX_CONNECTIONS` plus `DOCS_CONNECTIONS`. A step that raises either constant moves that sum against the port's 1,024 observers (`static_assert` in the example).
- `Delegate::subtree()` is the second function of the delegate that closes the channel by default; step 9's `complete()` will want the same rule for a ticket whose connection is gone.
