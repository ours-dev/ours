# examples-files changes

## Title: examples/files is the first component that provides a directory, and the dev image starts it (2026-10-31)

**Files**
- `examples/files/files_server.cpp` — the program: an `Outgoing` on a Loop that serves `/export/docs` with three files, one of them in a subdirectory, and nothing at `/export/ghost`
- `examples/files/BUILD.gn` — `files_server` and `files.cm`, which declares `docs` and `ghost` and exposes four things
- `examples/BUILD.gn` — `files_server` joins the `examples` group, so that `//:default` builds it as it builds echo
- `kernel/products/image/dev/BUILD.gn` — `root.cm` gets the eager child `files` and six offers from it to `routing_checks`; the comment above lists the child

**Symptom**
Latent, cannot be triggered before this change: nothing provided a directory. The manifest compiler (step 2) accepted the language, `Outgoing::add_directory()` (step 3) was called by no program, and the root (the entry for `init`) could not yet connect a consumer. Without a real provider on the target, none of those could be run.

**Cause**
A language that is only compiled and a tree that is only run on the host can both be wrong in the join between them: the path the manifest declares and the path the program serves are written twice, by hand, and are the one string the two share. Only a component that declares a path, a program that serves it, and a consumer that opens it, all booted, shows that they agree. Decision 10 (b) of the design makes that provider a new example and leaves echo, `examples/hello` and the guide's chapters on protocol servers as they are.

What it serves, and why each piece is there:

| Path | Content | For |
|---|---|---|
| `hello.txt` | one line | a Read, and GetBackingMemory |
| `numbers.txt` | four lines | a Read that crosses lines, and the order of a listing |
| `sub/nested.txt` | one line | a use's subdir, and an expose's subdir |
| `/export/ghost` | nothing | a path the manifest declares and the program never adds |

`ghost` is declared on purpose and never served: it is the target case "a path declared in the manifest but not served by the program", the typo between the manifest and the program that the design says reads as a `NotFound` written by the provider's server. A program that left it out by mistake would look the same, which is why the manifest says so in a comment.

`docs` is exposed three times, under three names, because a provider may expose one declaration with different rights or subdirs, and that is what makes the failing routes expressible. A target name must be unique among a component's exposes, so the design's table, which gives the expose with the subdir `sub` the name `docs`, is `docs-sub` here.

| Declared as | Exposed as | Offered as, used at | Expected, and observed |
|---|---|---|---|
| `docs`, `/export/docs`, `r*` | `docs`, `r*` | `files`, `r*`, used at `/files`, `r*` | works: read, list, rights shrink |
| | `docs-sub`, `r*`, subdir `sub` | `files-sub`, `r*`, `/files-sub` | works: only `nested.txt` is there |
| | `docs`, `r*` | `files-denied`, `rx*`, `/files-denied`, `rx*` | AccessDenied at files' expose; the entry arrives closed |
| | `docs-wide`, `rx*` | `files-wide`, `rx*`, `/files-wide`, `rx*` | AccessDenied at files' declaration; the entry arrives closed |
| `ghost`, `/export/ghost`, `r*` | `ghost`, `r*` | `ghost`, `r*`, `/ghost` | declared, never served: NotFound from files' server |
| | `docs`, as above | `files-file`, `r*`, subdir `hello.txt`, `/files-file` | the joined path ends at a file: NotDir from files' server |

The last row is not in the design's table. It is there because the table has no route that can tell whether the root's Open carries `ProtocolDirectory`: the declared path is always a directory, so the bit changes nothing for the five routes. Only a route whose joined path ends at a file does. The maintainer asked for dropping the bit to make a `routing_checks` case fail; this row is what makes it so (see `routing-checks.md`).

**Change**
The manifest of the provider:

```gn
capabilities = [
  { directory = "docs",  path = "/export/docs",  rights = [ "r*" ] },
  { directory = "ghost", path = "/export/ghost", rights = [ "r*" ] },
]
expose = [
  { directory = "docs",  from = "self", rights = [ "r*" ] },
  { directory = "docs",  from = "self", as = "docs-sub",  rights = [ "r*" ], subdir = "sub" },
  { directory = "docs",  from = "self", as = "docs-wide", rights = [ "rx*" ] },
  { directory = "ghost", from = "self", rights = [ "r*" ] },
]
```

It uses nothing and provides no protocol, so it has no `/svc` and no way to be asked to stop. The program (names and contents were the implementer's to choose): `files_server.cpp` takes the `DirectoryRequest` handle, makes a port and a Loop of `Outgoing::MAX_CONNECTIONS` slots, builds the tree with `add_directory("/export/docs")` and the wrappers, serves, and blocks in `loop.run(INSTANT_INFINITE)` like echo. Each file is a `VmoFile` whose VMO the program makes with `vmo_create` and fills with `vmo_write` from a constant; the bytes the Read copies from are the same constant. The VMOs are real because `GetBackingMemory` duplicates the handle without looking at it. `executable` is false. Nothing in the program names the capability `docs`; the path is the one string it shares with the manifest.

`root.cm` adds the child after `echo` and six offers from `#files` to `#routing_checks`, each `as` the name the consumer uses. The build now prints three warnings each time `root.cm`'s action runs, the existing one for `/denied` and one for each of the two routes that fail by rights; the real text:

```text
warning: //kernel/products/image/dev:root.cm: #routing_checks uses directory "denied" with rights rx*,
         but the route holds less at an offer:
           use    #routing_checks "denied"                                    rx*
           offer  root.cm "boot" from parent to #routing_checks as "denied"   r*   <- holds less
         The root will answer AccessDenied for this use. Offer rights that contain rx*, or ask the use for fewer.

warning: //kernel/products/image/dev:root.cm: #routing_checks uses directory "files-denied" with rights rx*,
         but the route holds less at an expose:
           use    #routing_checks "files-denied"                                    rx*
           offer  root.cm "docs" from #files to #routing_checks as "files-denied"   rx*
           expose #files "docs" from self to parent                                 r*   <- holds less
         The root will answer AccessDenied for this use. Expose rights that contain rx*, or ask the offer for fewer.

warning: //kernel/products/image/dev:root.cm: #routing_checks uses directory "files-wide" with rights rx*,
         but the route holds less at the declaration:
           use     #routing_checks "files-wide"                                         rx*
           offer   root.cm "docs-wide" from #files to #routing_checks as "files-wide"   rx*
           expose  #files "docs" from self to parent as "docs-wide"                     rx*
           declare #files "docs" at /export/docs                                        r*   <- holds less
         The root will answer AccessDenied for this use. Declare rights that contain rx*, or ask the expose for fewer.
```

The root logs the same three at the consumer's start, as `init: routing_checks's use of "files-denied" routes nowhere: AccessDenied at files (expose of "docs")` and `... "files-wide" ... (declaration of "docs")`. `files-file` and `ghost` route fine and warn nothing.

Limits, checked against this change and none raised:

| Limit | Where | Now |
|---|---|---|
| Directory uses of one component | `MAX_DIRECTORY_USES`, 15 | `routing_checks` has 10 |
| Bindings of a namespace | `fsio::MAX_BINDINGS`, 16 | 11: ten directory uses and `/svc` |
| Instances | `MAX_INSTANCES`, 8 | 6: the root, `routing_checks`, `echo`, `files`, `shell`, `pc-ps2` |
| Files of /boot | `MAX_BOOT_NODES`, 256 | 14 |
| Messages queued on `files`' outgoing channel | 64 (`channel_dispatcher.hpp`) | 4: one Open for each use that routes, and the two denied uses write none |
| Connections of `files`' `Outgoing` | `MAX_CONNECTIONS`, 32 | the root's own, one per working entry (2), and the few the checks hold open at once; a spawned child clones the two working entries |
| Handles the root pays for | `HANDLES_PER_COMPONENT` | unchanged: a provider-sourced entry costs the channel pair the root made, as a `boot` entry does, and no `/boot` connection |

Sizes, `llvm-size` of `out/dc-step4`: `files_server` text 27,717, data 640, bss 22,320 (the `Outgoing::Storage` of 13,000 and its Loop table of 32 slots are most of it); `files.cm` 438 bytes, `root.cm` 1,840 (was 1,369), `routing_checks.cm` 1,506 (was 1,062); `kernel.dev.obi` 1,017,128, `kernel.dev.dbg_1.obi` unchanged at 954,512.

**Reference**
Fuchsia's directory providers declare a `source_path` (`sdk/fidl/fuchsia.component.decl/capability.fidl:64-76`) and serve a directory there with `OutgoingDirectory::AddDirectoryAt()` (`sdk/lib/component/outgoing/cpp/outgoing_directory.cc:169`); the example plays the part of a component that does. The rights words and the hops are the design's own (decisions 3 and 4 follow Fuchsia's `ExposeDirectory`).

**Verification**
- `boot_check.py --out out/dc-step4` before the change and after: dbg PASS, 44 passed, 4 skipped, 0 failed, 0 error lines, both; rel PASS, 41 passed, 8 skipped, 0 failed, 6 of 6 required, 0 error lines, both; host tests 151 passed, both. `files` runs on rel only: dbg's image (`dev.dbg_1`) has no root, so it starts neither `files` nor `routing_checks`.
- The rel log: `init: started files`, the two refusals on `files`' console (`export/ghost` NotFound and `export/docs/hello.txt` NotDir), no `init: files exited`, `routing_checks: passed`.
- Twenty more rel boots of the final build (a fresh output directory), ten with 4 CPUs and ten with `SMP=2`, all PASS. `SMP=1` fails on the kernel, before `init` prints a line (`CPU[0] tick device I8253 failed and no other device can serve it`); that is not this change's, and was not checked against the commit before it.
- Mutations of the manifest, each built, booted and reverted: `docs-sub` exposed without its subdir (7 FAILED lines), the declaration of `docs` widened to `rx*` (4: `/files-wide` opens), every right of `files.cm` widened to `rx*` (7: both closed entries open), the ghost directory served (3), `hello.txt`'s bytes changed (3), `nested.txt` put at the top (10), the VMO written with no bytes (2). All are caught.
- clang-tidy on `files_server.cpp`: no diagnostic. `format_cpp.py --check` and `format_gn.py --check` clean on all the files.

**Deliberately not done**
- A way for `files` to exit, and `Outgoing::add_service()`: step 5.
- A Subtree or handler node, and a second declared directory served that way: steps 6 to 9.
- Any change to echo, `examples/hello` or the guide. The guide still says a tree server of one's own cannot be reached; the task note lists the passages.
- A marker on a use or an offer that silences the warning of a deliberate failure. The build warns three times on every compile of `root.cm`, as the design predicted.

**How it was found**
None. A new example, from the design. The sixth row of the table was found by asking which `routing_checks` case would fail if the root dropped `ProtocolDirectory`: none would, until a route ended at a file.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. The manifest says `/export/docs` and the program says `DOCS_PATH`. Which string would you change to make every consumer read `NotFound`, where would the typo show in the logs, and why is that line on the provider's console and not the root's?
2. `docs-wide` is exposed with `rx*` over a declaration of `r*`. Which hop is denied, which of the build and the root says so first, and what does the consumer of `/files-wide` read?
3. Why is `ghost` served by nothing and still routed without a warning, while `files-denied` is routed to nothing and warned about?

## Title: files exits when a consumer connects to its service node stop (2026-10-31)

**Files**
- `examples/files/files_server.cpp` — `StopHandler`, whose `connect()` exits the process with 0; `build_tree()` takes it and adds the service node `stop` under `export/docs` with `Outgoing::add_service()`; the header comment says what the node is for and who can reach it
- `examples/files/BUILD.gn` — the comment above `files_server` no longer says the program has no way to be asked to stop
- `kernel/products/image/dev/BUILD.gn` — the comment above `root.cm` lists the stop node among what routing_checks reaches

**Symptom**
Latent, cannot be triggered before this change: `files` served for as long as the system ran, so the provider-exit case of the design's Testing section (a consumer holds a working entry, the provider exits, the consumer sees it) had no provider that would exit when the test wanted. Visible now: `/files` lists a fifth name, `stop`, between `numbers.txt` and `sub`, and the rel log holds `files: stopping, as a consumer asked` and `init: files exited with 0`. The change is step 5 of 9 of `.agents/tasks/directory-capability/DESIGN.md`.

**Cause**
The case needs the provider to die at a moment the consumer chooses, after the entry has worked and after the consumer holds a connection of its own, and it must die in the way a provider dies: by exiting, so that the kernel closes its handles. The design weighed four ways and took the service node (decision 11 (c)): the other three cost a codec library and a second capability for a `Stop` protocol (a), a handler node that does not exist before step 9 (b), or a hook for a connection that ended and a test whose timing the client does not control (d). The service node costs one `Outgoing` call (`add_service()`, `ioservice.md`) and puts a service inside a directory, which nothing else exercised.

The decisions inside the example, which the design left to this step:

- **The exit is `process_exit(0)` from inside the handler**, as echo's `stop()` does. The other form, `main` returning 0, needs the Loop to stop, and it cannot: `Loop::run()` returns only when the port fails, and there is no call that quits it. The root logs `init: files exited with 0` either way.
- **The channel is dropped, not closed.** The handler never touches it, and it closes with every other handle as the process dies. This matters to the test, not to the example: the consumer's wait on the channel is then a wait for the death. A handler that closed it first would end that wait while `files` was alive, and the consumer would go on to check connections that were still open. It also orders the closes. The kernel empties a process's handle table in the order the handles arrived (`HandleTable::clean()`, as `check_echo()` relies on), and the stop channel is the newest handle `files` holds, so the held connection and the entries (older) are closed by the time the stop channel reads PEER_CLOSED, and the first `open()` on the entry afterwards answers PeerClosed with no retry.
- **The handler is a local of `main`, declared before the `Outgoing`.** The `Outgoing` keeps it by address and the program never returns from `main` but through the exit; the order is for the sake of reading it right.

Who can stop it: the node is in the tree a consumer's entry shows, and an entry of `r*` holds CONNECT, so any component offered `docs` can stop `files`. That is acceptable in an example whose point is to be stopped by its test, and wrong in a service. The design's last open question says so, and the header comment of the program too.

**Change**
```cpp
// before                                              // after
auto build_tree(ioservice::Outgoing &outgoing)         auto build_tree(ioservice::Outgoing &outgoing, StopHandler &stop)
                                                           ...
                                                           outgoing.add_service(docs.unwrap(), STOP_NAME, stop)

                                                       class StopHandler final: public ioservice::Handler {
                                                         public:
                                                           auto connect(Uandle) -> void override {
                                                               auto _ = ort::debug_line("files: stopping, as a consumer asked");
                                                               ours::process_exit(0);
                                                           }
                                                       };
```
`files.cm` does not change: the node is inside `docs`, which the manifest already declares and exposes. The tree has 9 nodes of the 32.

**Reference**
None for the exit: this project's own test hook. The exit from a Loop callback is echo's `EchoServer::stop()` (`examples/echo/echo_server.cpp`).

**Verification**
- `boot_check.py --out out/dc-step5`: dbg PASS, 44 passed, 4 skipped; rel PASS, 41 passed, 8 skipped, 7 of 7 required (`init: files exited with 0` is now one of them; see `routing-checks.md`); host tests 151.
- Sizes (`llvm-size`): `files_server` text 28,509 (27,717 in step 4's note), data 664 (640); `kernel.dev.obi` 1,018,448 (1,017,128), `kernel.dev.dbg_1.obi` 954,512, `files.cm` 438 and `root.cm` 1,840 unchanged. The note's bss, 22,320, is not comparable with this build's 21,496; I did not trace why (the command the note used is not recorded).
- On the target, the mutations that touch this program, each built, booted on rel and reverted: the handler does not exit (the boot hangs in `routing_checks` and `boot_check.py` reports `no "routing_checks: passed", "init: files exited with 0"` after its timeout), the handler closes the channel and does not exit (the same), the handler exits with 1 (`routing_checks: passed` but no `init: files exited with 0`), and no stop node (the listing check and the connect both FAILED). Table in `routing-checks.md`.
- clang-tidy on `files_server.cpp`: no diagnostic.

**Deliberately not done**
- A reply on the stop channel, or any protocol on it: the node only takes a channel.
- Stopping on the first of several kinds of request, or after a delay.
- A check that the caller is allowed to stop the program. There is nothing to check against; the point of the node is that the test can.
- A change to the manifest.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. The handler never closes the channel it is given. What would a consumer that waits for the channel to close learn if it did, and how does the order in which a process's handles close make the entry read PeerClosed on the first `open()` afterwards?
2. Why is `process_exit(0)` in a Loop callback the program's way to quit, and what does the root see of it?
3. Any component offered `docs` can stop `files`. Which two facts make that true, and what would a service have to do instead?
