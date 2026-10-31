# directory-capability-step4: the root forwards a directory use to its provider, and examples/files is the first one

**Date**: 2026-10-31

## Delivered
Branch `dc-step4`, worktree `.worktrees/dc-step4`, one commit on `b72170e6`: `feat(init): the root forwards a directory use to its provider, and examples/files is the first one`. It is step 4 of the Staging table of `.agents/tasks/directory-capability/DESIGN.md`:

- **Root** (`system/service/init/realm.{hpp,cpp}`): `Realm::forward_directory()` stands beside `forward()`. It writes `Open(<declared path without its leading '/'>[/<joined subdir>], <the use's rights as Perm* bits> | ProtocolDirectory, S)` on the provider's outgoing client end, fire and forget, and keeps nothing per entry. A declared path that is empty, is `/`, or does not start with `/` is InvArgs on S with a line naming the use, the declaration and the path. The `NotSupported` branch for a provider route is gone. `forward()` is split into the two halves it shares with the new function, `provider_serves()` (exited provider or closed outgoing end: NotFound, logged) and `write_open()` (encode, write, "cannot forward <name> to <provider>"), so the exited-provider rule exists once. `forward()`'s behaviour and log lines are unchanged.
- **`examples/files`**: program `files_server`, component `files`. It declares `docs` at `/export/docs` (`r*`) and `ghost` at `/export/ghost` (`r*`); exposes `docs` (`r*`), `docs-sub` (`docs`, `r*`, subdir `sub`), `docs-wide` (`docs`, `rx*`) and `ghost` (`r*`); uses nothing, provides no protocol, has no `/svc`, no `stop` node and no way to exit. It serves `hello.txt`, `numbers.txt` and `sub/nested.txt` under `/export/docs` and nothing under `/export/ghost`.
- **Program internals** (the implementer's choice): a `make_file(text)` that creates a VMO of the text's size with `vmo_create`, fills it with `vmo_write` and returns a `VmoFile` over the same constant; `build_tree()` with `add_directory("/export/docs")` and the wrappers; a Loop of `Outgoing::MAX_CONNECTIONS` slots; `loop.run(INSTANT_INFINITE)` to block, like echo.
- **Dev image** (`kernel/products/image/dev/BUILD.gn`): `root.cm` gets the eager child `files` (after `echo`) and six offers from `#files` to `#routing_checks`: `files`, `files-sub`, `files-denied`, `files-wide`, `ghost` (the design's five) and `files-file` (see "Deviations"). `routing_checks` declares six uses; the build prints three warnings (below). No pool was raised.
- **routing_checks**: 26 cases in `check_files()`, in the style of `check_denied()` and `check_echo()`, and the comments of its manifest and program now count the uses rightly (repair item 1 of `docs/repairs/261029/routing-checks.md` is closed).
- Documents: `docs/changes/261030/{init,examples-files,routing-checks}.md` (all new files), `docs/repairs/261029/routing-checks.md` (item 1 deleted, item 2 updated, its line cites moved), `docs/repairs/261031/fsio.md` (new item), this note.

The warnings of `root.cm`, three on each compile of its action (the real text is in `docs/changes/261030/examples-files.md`):

```text
warning: //kernel/products/image/dev:root.cm: #routing_checks uses directory "denied" with rights rx*,
         but the route holds less at an offer:
warning: //kernel/products/image/dev:root.cm: #routing_checks uses directory "files-denied" with rights rx*,
         but the route holds less at an expose:
warning: //kernel/products/image/dev:root.cm: #routing_checks uses directory "files-wide" with rights rx*,
         but the route holds less at the declaration:
```

## Verification
- `boot_check.py` on a fresh `--out out/dc-step4-fresh` (a cold build), against the same command on `b72170e6` before the first edit:

  | | Before | After |
  |---|---|---|
  | dbg | PASS, 44 passed, 4 skipped, 0 failed, 0 error lines | the same |
  | rel | PASS, 41 passed, 8 skipped, 0 failed, 6 of 6 required, 0 error lines | the same |
  | host tests | PASS, 151 passed (executables), 0 failed | the same |

  The counts are the shell's. The new cases are not in them: they run in `routing_checks` on rel only (dev.dbg_1 has no root, so it starts neither `files` nor `routing_checks`), and show as the existing required line `routing_checks: passed`. The root has no host test, so `forward_directory()` is run on the target only.
- Twenty more rel boots on the final build, ten with 4 CPUs and ten with `SMP=2`: all PASS. `SMP=1` fails in the kernel before `init` prints a line (`CPU[0] tick device I8253 failed and no other device can serve it`); not this change's, and not checked against the commit before it.
- Mutations (the scripts are in the session scratchpad, not in the tree), each applied, built, booted on rel and reverted; all 18 are caught by FAILED lines, and the tree was checked clean afterwards. The ones the brief names: the wrong path (leading `/` kept: 19 FAILED lines; subdir dropped: 9; subdir joined with another separator: 5), `ProtocolDirectory` dropped (3, only the `/files-file` case notices), the rights dropped (28), every right asked (2, only the `MODIFY_DIRECTORY` case). Also: the old `NotSupported` branch back (19), `files` serving the ghost (3), other bytes in `hello.txt` (3), `nested.txt` misplaced (10), an empty VMO (2), `docs-sub` without its subdir (7), the declaration widened (4), every right of `files.cm` widened (7), and the inverted InvArgs condition (19, which also shows the log line). Two attempts passed because they were wrong mutations, not because a check is missing: asking `r*` of the use of `/files-denied`, which leaves the denial between the offer and the expose. Table in `docs/changes/261030/routing-checks.md`.
- The retry in `refused_by_provider()` is exercised: with the `/ghost` case moved first, its first answer was PeerClosed in 24 of 24 boots (12 with 4 CPUs, 12 with `SMP=2`), then NotFound; in its place, NotFound at once in 20 of 20.
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step4`) on `realm.cpp`, `routing_checks.cpp` and `files_server.cpp`: no diagnostic. `format_cpp.py --check` and `format_gn.py --check` clean on every changed file. `lint_docs.py --changed` clean on every document.
- Sizes: `files_server` text 27,717, data 640, bss 22,320; `files.cm` 438 bytes, `root.cm` 1,840 (was 1,369), `routing_checks.cm` 1,506 (was 1,062); `kernel.dev.obi` 1,017,128, `kernel.dev.dbg_1.obi` 954,512 (unchanged).
- Limits, none raised: directory uses of `routing_checks` 10 of 15, namespace bindings 11 of 16, instances 6 of 8, /boot files 14 of 256, 4 Opens queued on `files`' outgoing channel of 64.

## Deviations from the design
- **A sixth use, `files-file`, which the design's table does not have.** Dropping `ProtocolDirectory` from the root's Open passed every case written from the table: the declared path is always a directory, so the bit changes nothing for the five routes. The brief asks that the mutation fail a routing_checks case. `files-file` is `docs` offered with the subdir `hello.txt`, so the joined path `export/docs/hello.txt` ends at a file; the Open asks for a directory and the provider answers NotDir. It costs one offer and one use, and warns nothing. If the maintainer does not want it, remove those two and the two lines of `check_files_refusing()`; the `ProtocolDirectory` mutation then goes uncaught.
- **The expose with the subdir is `docs-sub`.** The design's table calls it `docs`, but target names of exposes are unique (a compiler rule).
- **The refusal a provider makes arrives late, and the first Open on the entry can be lost to it.** The design's table says `/ghost` is NotFound "read once from the epitaph" for the entries the root closes, and "written by the provider's server on S" for the ghost, as if the two behaved alike. They do not: an Open the consumer wrote on the entry before the provider answered the root's is queued on the entry and is destroyed when the provider closes it, so the connection reads PeerClosed with no status, and the second `open()` finds the write refused and reads the epitaph. This is D8 (a) and fire and forget seen from the consumer, which the design accepts for a provider that exits (the fifth row), but it was not seen for a provider's own refusal. `refused_by_provider()` allows one PeerClosed and asks again, as `check_echo()` allows one loss. Recorded in `docs/repairs/261031/fsio.md`.
- **A declared path of exactly `/` is InvArgs too**, not only an empty or relative one: stripping its slash leaves an empty path, and joined with a subdir it leaves one with a leading `/`. The compiler never writes it.
- **`check_files_closed()` runs no wait, and the other new cases wait only on `files`' server.** The brief says to bound them as `check_denied()` does; `check_denied()` does not wait, it reads an epitaph the root wrote before the program started, and so do the cases for `/files-denied` and `/files-wide`. A bound on a wait needs a clock, which `object_wait_one` does not have (repair item 2). The one hang that remains is a `files` that runs and never reads its outgoing directory; that is now written in the repair item.
- **The root's log line for a denial at a declaration is unchanged** (`AccessDenied at files (declaration of "docs")`), which step 1 noted does not say that the provider allows less.
- **The design's cites into `realm.cpp` are of an earlier snapshot.** It puts `Realm::run`'s routing at `:497-544` and `forward()` at `:789-833`; at `b72170e6` the routing is at `:530-551` and `forward()` starts at `:830`. The guide's cites match `b72170e6`.

## Found along the way
- A `FAILED` count from `boot_check.py` includes the closing `n check(s) FAILED` line, so a mutation caught by one case shows 2.
- With `ProtocolDirectory` dropped, the provider opens the file for the entry; the next Open on a file connection is not answered, so the `/files-file` case reads `PeerClosed` twice and not `NotDir`. That is what the mutation shows.
- Not done and not filed: the `docs/repairs/261004/init.md` item 7 says routing_checks "holds 4 entries"; it holds 10 now, but the six new ones are not /boot connections, so its arithmetic for /boot is unchanged.

## Not done
- The `stop` service node, `Outgoing::add_service()`, and the provider-exit cases: step 5. The Subtree and handler nodes: steps 6 to 9.
- The guide and the design notes, as the brief says. The passages below are wrong or stale now.

### Guide passages that step 4 makes wrong (in addition to the lists of the step 2 and step 3 notes)
| Passage | Says | Now |
|---|---|---|
| `docs/guide/04-tree-servers.md:5`, `:34`, `README.md:24`, `:39`, `:60`, `:119`, `06-checklist.md:74` (already on step 2's list as "root's side is step 4") | a tree server of one's own cannot be reached | It can: the root forwards the use. These sentences are now false at run time too, not only about the manifest. |
| `01-concepts.md:483` and `04-tree-servers.md:364` | the root forwards an Open into a provider's outgoing directory, "for a protocol a sibling provides" | also for a directory: one Open of the declared path, at the consumer's start |
| `04-tree-servers.md:453`-`:468` | "When the root binds a directory to a component, it passes the route's rights to the tree server" | for `boot` only; for a provider's directory the rights travel in the forwarded Open's flags, and the provider's server makes the connection |
| `05-testing-and-debugging.md` causes 6 and 11, and the table before them | `AccessDenied` for a directory use only from an offer; `cannot forward <name> to <provider>` for a protocol | the denial may be at an expose or a declaration; the line is said for a directory too, with the directory's name at the provider |
| `05-testing-and-debugging.md`, the list of causes | no cause for a provider that refuses a directory Open | new: the consumer reads the status on the second `open()`; the provider's console says `ioservice: the outgoing directory refused an Open of "<path>"`; the root says `init: ... cannot be forwarded: ...` for an invalid declared path |
| `05-testing-and-debugging.md:275`, `README.md:70`, and the sample logs | the children of `root.cm` are `routing_checks`, `echo`, the shell and the others; the log shows `started echo` then `started shell` | `init: started files` between them; every rel log now holds two `ioservice: ... refused an Open of "export/ghost"` / `"export/docs/hello.txt"` lines from `files`' console, which are expected |
| `05-testing-and-debugging.md:286` ("a negative case there has four parts") and `:464` | a refusal is the root's epitaph, read at the first `open()` | true for `/files-denied` and `/files-wide`; `/ghost` and `/files-file` are the provider's, and arrive late |
| `06-checklist.md:438` and `04-tree-servers.md:468` | cites `routing_checks.cpp:528-544`, `:320-322`, `:320` | `check_denied` is at `:364` now; `check_echo` at `:1031`; `main` at `:1102` |

### Guide cites that moved
Old lines are `b72170e6`'s, new lines are this commit's. All of `realm.cpp` after line 128 moved by 13 (`to_flags()` and its asserts), everything after `forward()` by 70 and more.

| Guide | Cites | Old | New |
|---|---|---|---|
| `04-tree-servers.md:364`, `01-concepts.md:483`, `06-checklist.md:386` | `forward()` | `realm.cpp:830` (`:849-851`, `:872-876` the Open and the write) | `forward()` at `:900`; the shared parts are `provider_serves()` `:849-868` and `write_open()` `:870-898` |
| `05-testing-and-debugging.md:433`, `06-checklist.md:125`, `:342` | the exited-provider check and its line | `realm.cpp:838-848` | `realm.cpp:849-868` (`provider_serves()`) |
| `01-concepts.md:390`, `06-checklist.md:125`, `:327`, `05-testing-and-debugging.md:433` | `on_terminated()`, the exit line, the state set | `realm.cpp:897-918` (`:910`, `:917`) | `:986-1007` (`:999`, `:1006`) |
| `04-tree-servers.md:453`, `:527`, `05-testing-and-debugging.md:435` | the namespace entry in `run()` | `realm.cpp:540`, `:554`, `:530-551` | `:559`, `:573`, `:545-571` (a directory route that ends at a provider is now one more branch, `:553-558`) |
| `05-testing-and-debugging.md:275` | the children started in order | `realm.cpp:444-455` | `:457-468` |
| `04-tree-servers.md:467` | the dev image's offers | `kernel/products/image/dev/BUILD.gn:114` | the first offer of `boot` is at `:125` and the child list gained `files` |
| `docs/repairs/261029/routing-checks.md` item 2 | `connect_status`, `Incoming::receive` in the repair item | `routing_checks.cpp:770-788`, `:343-357` | `:995-1016`, `:567-585` (updated in the repair item) |

## For step 5
- `files` has no `stop` node and `Outgoing` has no `add_service(path, handler)`. The handler table is `Storage::handlers`, indexed by the value a service node was added with, and `MAX_PROTOCOLS` (8) limits it; `connect(u64 service, channel)` reads that value as an index, which is why `tree()` is not exposed: `add_service` has to go through `Outgoing` (step 3's note). `files` needs one `Handler` that calls `process_exit(0)` from `connect()`, as echo's `stop()` does.
- The stop case needs a held connection on `/files` first, then `/files/stop` with `ProtocolService`: `r*` holds CONNECT. After the exit, a held connection reads PeerClosed, and `open()` on the entry answers PeerClosed from its write, then reads the epitaph if one is there: none, because the root is not involved (the sixth row of the design's table), so expect PeerClosed, not NotFound, from a consumer that already had the entry.
- A consumer that starts after the exit gets NotFound from the root (`provider_serves()`), logged as `routes nowhere: NotFound at files (exited)`. The dev image has no such consumer; spawned children get a clone of a live entry.
- `routing_checks` runs `check_files()` before `check_echo()`, whose exit assumptions are about echo's handle order. A `stop` case for `files` should run last among `files` cases: the entries `/files` and `/files-sub` are cloned into every spawned child, and `check_spawn` runs before.
- `files` serves ghost's Open with a line on its console each time; a stop case that wants a clean log has nothing to do about it, the lines are expected.
- The 32 connections of `files`' `Outgoing` are shared by the root's own, each working entry, and every clone a spawn makes; the checks hold at most a handful open at once.
