# directory-capability-step5: a directory capability's tree can hold a service node, and files stops on it

**Date**: 2026-10-31

## Delivered
Branch `dc-step5`, worktree `.worktrees/dc-step5`, one commit on `3766b50f`: `feat(ioservice): a directory capability's tree can hold a service node, and the files example stops on it`. It is step 5 of the Staging table of `.agents/tasks/directory-capability/DESIGN.md`:

- **`Outgoing::add_service(parent, name, handler) -> ustl::Result<NodeId, Status>`** (`system/lib/ioservice`). `parent` is a node `add_directory(path)` or `add_directory(parent, name)` returned, as the step 3 wrappers take; the new node is returned. The handler goes in the table `add_protocol()` fills, with the node's value as its index, so `MAX_PROTOCOLS` (8) is a limit on protocols and service nodes together; the doc comments and the README say so. Checks, in this order: `parent` is at or below a declared directory (InvArgs for the outgoing root, `svc/`, a protocol's node, the directory on the way to a declared path, and an id of no node); the table has room (NoResource) before the tree is touched; then `Tree::add_service()`'s own refusals (InvArgs for a bad name, NotDir, AlreadyExists, NoResource for the nodes). A refusal leaves nothing behind. The private `m_protocols` is `m_handlers`; `MAX_PROTOCOLS` keeps its name.
- **`examples/files`**: a service node `stop` under `export/docs` and a `StopHandler` whose `connect()` logs `files: stopping, as a consumer asked` and calls `process_exit(0)`. The channel is dropped, not closed: the process's death closes it with the rest. The root logs `init: files exited with 0`. `files.cm` does not change.
- **routing_checks**: `check_files_stop()`, called last in `main()`: with a file connection held and read, it connects to `/files/stop` (the channel closes with nothing on it), waits for the held connection to read PEER_CLOSED, then `open()` on the entry `/files` answers PeerClosed, so does a second `open()` under it (from the binding), one on `/files-sub`, and a connect to `/files/stop` again. `FILES_NAMES` gains `stop`.
- **`boot_check.py`**: `REQUIRED` for rel gains `init: files exited with 0` (the printed count is 7/7).
- Documents: `docs/changes/261030/{ioservice,examples-files,routing-checks}.md` (sections appended), `docs/repairs/261029/routing-checks.md` (item 2 gains the new waits, cites moved), the ioservice README, comments in `examples/files/BUILD.gn` and `kernel/products/image/dev/BUILD.gn`, this note.

## Verification
- `boot_check.py --out out/dc-step5` on the final tree, against step 4's counts:

  | | Step 4 | Now |
  |---|---|---|
  | dbg | PASS, 44 passed, 4 skipped, 0 failed, 0 error lines | the same |
  | rel | PASS, 41 passed, 8 skipped, 0 failed, 6 of 6 required, 0 error lines | PASS, the same counts, **7 of 7 required** |
  | host tests | PASS, 151 passed (executables), 0 failed | the same; `outgoing` has 41 tests (was 31) |

  The new case is not in the shell's counts: it runs in `routing_checks` on rel and shows as the required `routing_checks: passed` and `init: files exited with 0`. dev.dbg_1 has no root, so it starts neither.
- 24 more rel boots on the final build (`--no-build --no-tests`): 12 with 4 CPUs and 12 with `SMP=2`, all PASS with 7 of 7 required. `SMP=1` is not run: step 4 found it fails in the kernel before `init` prints a line.
- Host mutations (16 of `outgoing.cpp` and `server.cpp`, each built, run and reverted, the files compared with copies after): the table check after the node is added, no parent check, only `svc/` refused, the root and `svc/` refused but not the directory on the way to a declared path, a protocol's node accepted, only the declared node and nothing below it, `connect()` always the first handler, the node's value 0, the count not raised, the last slot unusable, a refusal that takes a slot, `add_protocol()` blind to the services, a service opened as a directory answering BadType, the last CONNECT check of the service branch removed, and both CONNECT checks removed: 15 fail the new tests. The one that passes is check 4 (CONNECT for the service flag) removed alone, which is equivalent: the last check of the branch refuses the same request, so only the two removed together change an answer, and that is caught. Table in `ioservice.md`.
- Target mutations (each built, booted on rel with a 40 s timeout, reverted): the case connects to `/files/sub/stop` (FAILED line, NotFound), to `/files/hello.txt` (FAILED line, BadType), `files` adds no stop node (FAILED lines), the handler exits with 1 (no `init: files exited with 0`, 6 of 7 required), the handler does not exit and the handler closes the channel without exiting (**both hang**: `no "routing_checks: passed", "init: files exited with 0" within 40s`, with no FAILED line). Table in `routing-checks.md`.
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step5`) on `outgoing.cpp` (with its header), `outgoing_test.cpp`, `files_server.cpp` and `routing_checks.cpp`: no diagnostic on any line of the changed sources; the test file prints googletest's own header. `format_cpp.py --check` and `format_gn.py --check` clean on every changed file. `lint_docs.py --changed` clean on every document.
- Sizes: `files_server` text 28,509 (27,717 in step 4's note), data 664 (640); `kernel.dev.obi` 1,018,448 (1,017,128); `kernel.dev.dbg_1.obi` 954,512 (unchanged); `files.cm` 438, `root.cm` 1,840 and `routing_checks.cm` 1,506 unchanged.
- The rel log: `files: stopping, as a consumer asked`, `init: files exited with 0`, then `routing_checks: passed` and `init: routing_checks exited with 0`. The shell's `spawn(routing_checks child)` still gets `exited with 0`.

## Deviations from the design
- **`add_service(parent, name, handler)`, not the design's `add_service(path, handler)`.** The brief left the form to this step and asked for step 3's wrappers' shape. A path form would walk a second time to a parent the program already holds, and would need a rule for splitting off the node's name. Said in `ioservice.md`.
- **"Skipping the exit must fail the case" is a hang, not a FAILED line.** The waits are infinite (`object_wait_one` has no other deadline: repair item 2), so a stop node that does not exit leaves `routing_checks` waiting and `boot_check.py` reports the missing `routing_checks: passed` after its timeout. A wrong path does fail with a FAILED line, because the server's epitaph is the first wait's answer and the case stops. `init: files exited with 0` is a required line now, so a handler that exits wrongly (with 1) fails the boot as well.
- **The case's place does not matter to `check_spawn()`.** The brief, step 4's note and the design say the case must run after the spawn because the entries are cloned into the child. `fsio::export_namespace()` skips an entry whose server is gone, and moving the case before `check_spawn()` still passes (tried). The case is last all the same, so that nothing depends on that, and the comment says why.
- **A required boot line was added** (`init: files exited with 0`), which the brief names as the root's behaviour but not as a change of the script. It changes the count the script prints and the guide's examples of it (below).
- **The design's `files` listing, `docs`, gains a fifth name,** `stop`, as a service dirent; the brief implied it, and the listing check was updated.

## Found along the way
- A consumer that held an entry whose provider exited reads PeerClosed on every `open()` under it, from the binding, for good: `fsio` records the answer. The guide's advice for a lost Open, "the next connect gets NotFound" (cause 12 of the debugging chapter), is true only for a consumer that did not hold the entry. Listed with the guide passages below.
- The close order that makes the first `open()` deterministic (handles close in the order they arrived, so the newest, the stop channel, closes last) is the same fact `check_echo()` rests on. It held in 24 of 24 boots; a kernel that closed newest first would show as a FAILED line in this case, which does not allow for a loss.
- The scratchpad of this session held earlier steps' files; the mutation scripts of this step are `mutate_host.py`, `mutate_target.py` and `flaky.py` there, not in the tree.
- No new repair item. Item 2 of `docs/repairs/261029/routing-checks.md` gains the case's waits; `docs/repairs/261031/fsio.md` item 1 is unaffected.

## Not done
- The Subtree node and `add_subtree()`: step 6. The handler node, tickets and the Loop's Suspend: steps 7 to 9.
- The guide and the design notes, as the brief says. The passages below are wrong or stale now, in addition to the lists of the notes of steps 2 to 4.

### Guide passages that step 5 makes wrong (in addition to the earlier lists)
| Passage | Says | Now |
|---|---|---|
| `docs/guide/04-tree-servers.md:253`, `06-checklist.md:96` | `add_protocol` is `NoResource` once 8 protocols are served; "Protocols in one outgoing directory: 8" | 8 handlers, protocols and service nodes together; `add_service` answers NoResource too |
| `01-concepts.md:438`, `02-first-server.md:223`, `04-tree-servers.md:233`, `:251` | a `Handler` is what an Open of `svc/<name>` is handed to; the tree is "the outgoing directory, its `svc/` directory and a service node per protocol" | also a service node in a directory capability's tree (`add_service`), at a path outside `svc/`; the quoted body of `Handler` is unchanged, only the comment above it |
| `04-tree-servers.md:164` | the nodes are `2 + MAX_PROTOCOLS` | (already stale from step 3) a service node of a capability takes a node of the pool too |
| `02-first-server.md:24`, `README.md:90` | the sample `rel` line ends `6/6 required` | `7/7 required` |
| `05-testing-and-debugging.md:225-228` | "For `rel` there are six" required lines, "counts as `6/6 required`" | seven; the new one is `init: files exited with 0` |
| `02-first-server.md:612-619`, `06-checklist.md:432`, `05-testing-and-debugging.md:264` | the `REQUIRED` list at `boot_check.py:90-100` and the sample log with `init: echo exited with 0` | the list is at `:92-102` and holds `init: files exited with 0` after echo's; the sample log has `files: stopping, as a consumer asked` and `init: files exited with 0` before `routing_checks: passed` |
| `README.md:99` | the required lines include `init: echo exited with 0`, "the evidence that `echo` ran, answered a client and was stopped" | and `init: files exited with 0`, the evidence that `files` was stopped from inside its own directory |
| `05-testing-and-debugging.md:418` (cause 12) and `:425` | PeerClosed with no epitaph and no root line: "the Open reached a provider that was exiting ... the next connect gets `NotFound`"; "`routing_checks` expects exactly that after it asks `echo` to stop" | also a consumer that holds a directory entry whose provider exited: every `open()` under the entry reads PeerClosed, recorded by `fsio`, and never turns into NotFound; `routing_checks` asks `files` to stop too (`check_files_stop()`), last |
| `05-testing-and-debugging.md:275`, `README.md:70` | the children of `root.cm` and the sample log | the log also holds `files: stopping, as a consumer asked` and `init: files exited with 0` (in no fixed order against the shell's lines) |
| `06-checklist.md:327`, `:342` | "Enforced that the root logs an exit, for echo `init: echo exited with 0`" | and for `files` |

### Guide cites that moved
Old lines are `3766b50f`'s, new lines are this commit's. `outgoing.hpp` grew by about 50 lines and `outgoing.cpp` by about 50.

| Guide | Cites | Old | New |
|---|---|---|---|
| `04-tree-servers.md:60`, `:233`; `02-first-server.md:225` | `class Handler` | `outgoing.hpp:79`, `:79-91` | `:84`, `:84-96` (the comment above it from `:79`) |
| `04-tree-servers.md:60` | `class Outgoing` | `outgoing.hpp:95` | `:100` |
| `04-tree-servers.md:255`, `06-checklist.md:96` | `MAX_PROTOCOLS` | `outgoing.hpp:98` | `:108` (the comment from `:102`) |
| `04-tree-servers.md:255`, `06-checklist.md:97` | `MAX_CONNECTIONS` | `outgoing.hpp:108` | `:118` (comment from `:110`) |
| `04-tree-servers.md:170`, `:143` | the sizes comment of `Storage`, `struct Storage` | `outgoing.hpp:137`, `:141` | `:147`, `:151` |
| `04-tree-servers.md:255` | `add_protocol`'s refusals | `outgoing.hpp:170` | `:181` (the comment; the declaration is `:192`) |
| `04-tree-servers.md:255` | `serve` | `outgoing.hpp:228` | `:273` (the comment; the declaration is `:284`) |
| `01-concepts.md:204`; `06-checklist.md:147`, `:179`, `:296` | the one-thread sentence; the `Handler` comment; `Handler::connect`'s comment | `outgoing.hpp:71`, `:75-78`, `:81-83`, `:81-84` | `:75`, `:79-83`, `:86-88`, `:86-89` |
| `01-concepts.md:484` | the class comment | `outgoing.hpp:18` | starts at `:18` still, and is longer |
| `04-tree-servers.md:138` | `unwrap()` for a call that cannot fail | `outgoing.cpp:78` | `:95` |
| `04-tree-servers.md:176` | the `LoopServer` built over the `Storage` | `outgoing.cpp:66` | `:81` |
| `04-tree-servers.md:369` | `add_service` with the handler's index | `outgoing.cpp:88` | `:105` |
| `05-testing-and-debugging.md:434` | the line, `open_refused()` | `outgoing.cpp:196-218` | `:248-270` |
| `05-testing-and-debugging.md:434` | the NotFound walk, `tree.cpp:211`, and the tests by name | unchanged | unchanged (`tree.cpp` did not change) |
| `04-tree-servers.md:467`, `05-testing-and-debugging.md:286`, `:464`, `06-checklist.md:438`, `:386` | `check_denied`, `check_echo`, `main`, `stop_echo`, `connect_status`, `Incoming`, `connect_or_fail`, `check_files`, `refused`, `call` in `routing_checks.cpp` | `:364`, `:1031`, `:1102`, `:974`, `:998`, `:553`, `:635`, `:538`, `:340`, `:652` | `:371`, `:1038`, `:1211`, `:981`, `:1005`, `:560`, `:642`, `:545`, `:347`, `:659` |
| `docs/repairs/261029/routing-checks.md` item 2 | `connect_status`, `Incoming::receive` | `routing_checks.cpp:995-1016`, `:567-585` | `:1002-1023`, `:574-592` (updated in the repair item) |
| `02-first-server.md:612`, `06-checklist.md:432` | `REQUIRED` in `boot_check.py` | `:90-100` | `:92-102` |

## For step 6
- `connect(u64 service, channel)` is the `Delegate`'s one hook for a service node and `Outgoing` reads its value as an index into the one handler table, now shared by protocols and service nodes. A Subtree node (`Action::Subtree`, its own delegate function) has to decide whether its handlers go in that table too, which would make `MAX_PROTOCOLS` a limit on three kinds, or in a table of their own. That changes what the constant means in the public interface, so it is the maintainer's to decide.
- `add_subtree(path, handler)` is added "at a declared path": the node is the end of the declaration, as the directory `add_directory(path)` makes is. `is_declared()`, the overlap rule and `in_declared_tree()` (a walk over the children of each declared node) all treat it as one, and `Tree::first_child()` of a node that is not a directory is none, so a Subtree node has no tree under it for `add_service()` to find. A program that wants a `stop` node inside a Subtree-served directory has to serve it itself.
- `examples/files` serves `docs` from a static tree today and `stop` inside it. If step 6 moves `docs` behind a Subtree handler, `/files/stop` must still be reachable by `connect()`, or `check_files_stop()` and the required exit line fail. The simplest way to keep it is to leave `docs` as it is and serve a second declared directory through the handler.
- The case depends on the close order of a process's handles, and on `stop` being a service reachable through the `r*` entry. Neither a Subtree node nor the handler nodes of steps 8 and 9 change either.
- `boot_check.py` now requires `init: files exited with 0`; a boot that does not run `files` to its stop (for example a change that makes `files` crash earlier) fails on that line first.
