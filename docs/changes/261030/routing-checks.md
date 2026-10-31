# routing-checks changes

## Title: routing_checks reads the directories the sibling files provides, and its comments count the uses rightly (2026-10-31)

**Files**
- `system/service/init/test/routing_checks/routing_checks.cpp` — `check_files()` and the nine functions under it; the header comment; the namespace count (5 entries to 11)
- `system/service/init/test/routing_checks/BUILD.gn` — six directory uses; the comment above the manifest lists them and counts the protocol uses rightly
- `docs/repairs/261029/routing-checks.md` — item 1 is deleted, item 2 updated

**Symptom**
Before: nothing on the target connected a consumer to a directory that a component provides, so the root's `forward_directory()` and the example `files` (see `init.md` and `examples-files.md`) were unchecked beyond the build. Item 1 of the repairs for this module: the comments said "five protocols" and "six protocol uses" and left out `ours.resource.Sampling`, when the manifest has seven.

**Cause**
The target cases of the design's "On the target" table did not exist, and the module is where such a case lives (`routing_checks` is the only consumer in the boot). The comment counts were stale because a protocol was added to the manifest and the program and not to the lists that name them.

Each case is a statement about one route, and three details of how they are written come from how a refusal arrives.

- **A refusal the root makes is already on the entry when the program starts.** `/files-denied` and `/files-wide` are closed by the root with an epitaph before `routing_checks` runs, exactly as `/denied` is. `fsio::open()` writes on the entry, the write answers `PeerClosed`, the epitaph is read once and recorded, and every later `open()` under the entry answers it from the binding. Nothing waits: that is the model, and `check_files_closed()` is `check_denied()`'s shape, over six paths.
- **A refusal the provider makes arrives later, and the first Open on the entry can be lost to it.** `/ghost` (nothing is served at the declared path) and `/files-file` (the joined path ends at a file) route fine; the provider's server refuses the root's forwarded Open and writes the epitaph on the entry's channel, and closes it. A consumer's `open()` that was written before that is queued on the entry, and is destroyed with it: the new connection reads `PeerClosed` with nothing on it, and the status is on the entry for the next `open()`, whose write is refused and which reads it. So `refused_by_provider()` allows one `PeerClosed` and asks again, as `check_echo()` allows one loss after echo's exit, and the second answer must be the status. This is the shape of D8 (a)'s fire and forget seen from the consumer; it is not a fault of the provider. The retry is not dead code: with the `/ghost` case moved to the first thing `main()` does, the first answer was `PeerClosed` in 24 of 24 boots (12 with 4 CPUs, 12 with `SMP=2`) and the second `NotFound`; in its place, after the cases that read `files`, the first answer was `NotFound` in 20 of 20.
- **Nothing here can wait on a consumer entry that never opens, except by a provider that is alive and reads nothing.** `object_wait_one()` takes a deadline of 0 or infinity only, so no case can give up (item 2 of the repairs for this module). The cases wait on `files`' server only, which the root has already written to: a `files` that failed to start has its server end closed, so the root's Open and the entry are destroyed and `open()` answers at once; a `files` that is running serves. The one hang left is a provider that runs and never serves its outgoing directory, which is the existing item 2 and is now written into it.

**Change**
The cases, in the order `check_files()` runs them:

| Case | Expected |
|---|---|
| `hello.txt`, `numbers.txt` through `/files`, `nested.txt` through `/files/sub` | the bytes |
| the directory `/files` and `/files/sub` | `.`, `hello.txt`, `numbers.txt`, `sub` and `.`, `nested.txt`, in that order |
| `GetBackingMemory` of `hello.txt` | a VMO that holds the bytes; with `WRITE`, AccessDenied |
| write, execute, or modify-directory through `/files`, a missing name | AccessDenied for the three, NotFound for the name |
| `/files-sub` | `.`, `nested.txt`; `hello.txt` and `sub` under it are NotFound |
| `/files-denied`, `/files-wide` and a file and a directory under each | AccessDenied |
| `/ghost` and a path under it | NotFound |
| `/files-file` and a path under it | NotDir |
| the capability nobody offers (`ours.test.Unoffered`) | NotFound, as before |

26 cases in all, not counting the unoffered capability, which was there. The manifest gains the six uses; the program expects 11 entries. The `/files-file` use and the offer behind it are not in the design's table (see `examples-files.md`).

```cpp
// before                                                  // after
if (auto const bound = ...size(); bound != 5) {            if (auto const bound = ...size(); bound != 11) {
    fail("... four directory uses and /svc");                  fail("... ten directory uses and /svc");
```

**Reference**
None, this project's own tests; the cases follow `check_denied()` and `check_echo()`.

**Verification**
- `boot_check.py --out out/dc-step4`: dbg PASS, 44 passed, 4 skipped; rel PASS, 41 passed, 8 skipped, 6 of 6 required, `routing_checks: passed`; host tests 151. Counts unchanged, because the cases are part of `routing_checks`, not the shell's checks.
- Mutations, each built, booted and reverted; every one makes `routing_checks` print FAILED lines, and the boot check fails. The counts are `boot_check.py`'s, the closing `n check(s) FAILED` line among them.

  | Mutation | FAILED lines | The case that notices |
  |---|---|---|
  | the Open keeps the declared path's leading `/` | 19 | every `/files` case: BadPath |
  | the Open drops the joined subdir | 9 | `/files-sub` lists the whole tree and opens `hello.txt` |
  | the subdir is joined with another separator | 5 | `/files-sub`, `/files-file`: NotFound |
  | `ProtocolDirectory` is dropped | 3 | `/files-file` (PeerClosed, not NotDir), and no other |
  | the rights are dropped | 28 | every case that opens something: AccessDenied |
  | every right is asked, not the use's | 2 | `open(MODIFY_DIRECTORY)` of `/files` |
  | the use's rights plus `ModifyDirectory` | 2 | the same case |
  | the provider route is not forwarded | 19 | NotSupported everywhere |
  | `files` serves `/export/ghost` | 3 | `/ghost` is not NotFound |
  | `hello.txt` holds other bytes | 3 | the read and the VMO |
  | `nested.txt` is at the top of `docs` | 10 | the listings and the read |
  | the VMO of a file is written with no bytes | 2 | the VMO read |
  | files' expose of `docs-sub` has no subdir | 7 | `/files-sub` |
  | files' declaration of `docs` allows `rx*` | 4 | `/files-wide` opens |
  | every right of `files.cm` is `rx*` | 7 | `/files-denied` and `/files-wide` open |

  Two attempts at a mutation passed and were wrong: asking `r*` of the use of `/files-denied` (and of `/files-wide`) leaves the route denied, because the denial is between the offer's `rx*` and the expose's `r*`, not at the use. They are not gaps in the checks.
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step4`) on `routing_checks.cpp`: no diagnostic. `format_cpp.py --check` clean.
- Not run: a provider that exits (step 5), and `files` failing to start.

**Deliberately not done**
- The cases that need a `stop` node or a handler node (steps 5 and 9).
- A bound on a wait. The repair item for it stays, with this entry's cases added to its symptom.
- A check on the provider's console line (`ioservice: ... refused an Open of "export/ghost"`). `boot_check.py` reads no such line, and the tests of the library cover it (step 3).

**How it was found**
The `/files-file` row of the table above was found by mutation, not by reading: dropping `ProtocolDirectory` from the root's Open passed every case written from the design's table, which showed that no route there ended at a file. The retry in `refused_by_provider()` was found by moving the `/ghost` case first, with a line printed for each first answer.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `/files-denied` is answered by the root's epitaph and `/ghost` by the provider's. Why can `open("/files-denied")` never read `PeerClosed` with no status, and why can `open("/ghost")` read it once?
2. What is lost with the entry when the provider closes it, why is the status still there for the next `open()`, and what would a check that did not retry report for the lost one?
3. The use of `/files-denied` asks `rx*` and the fix to make the route work is not in the use. Which hop holds less, and what would you change in `root.cm` instead?

## Title: routing_checks stops files and checks what a consumer that held the entry sees of a provider that exited (2026-10-31)

**Files**
- `system/service/init/test/routing_checks/routing_checks.cpp` — `check_files_stop()`, called last in `main()`; `FILES_STOP`; `FILES_NAMES` gains `stop`; the header comment
- `.agents/skills/ours-verify/scripts/boot_check.py` — `REQUIRED` for rel gains `init: files exited with 0` (6 lines to 7)
- `docs/repairs/261029/routing-checks.md` — item 2 (a wait that cannot give up) gains the new case's waits and the cites move

**Symptom**
Latent, cannot be triggered before this change: nothing exited a provider whose directory a consumer was using, so the sixth row of the design's table of "What the consumer sees" (a provider that exits after the entry works) had no test on the target. The change is step 5 of 9 of `.agents/tasks/directory-capability/DESIGN.md`. The rel log now holds `files: stopping, as a consumer asked` and `init: files exited with 0` after `routing_checks`' last case.

**Cause**
The row says: "Exits after the entry works: nothing, the root is not involved. N and every connection of C1 see PEER_CLOSED". That is a claim about three things the root does not do after the consumer's start: it holds nothing per entry, it hears of the exit only through its own wait on the process and the outgoing end (it logs the exit), and it writes no epitaph on an entry it already handed over. So a consumer that already held the entry reads PeerClosed on the first `open()`: `fsio::open()` writes the Open, the write finds the entry's server end closed, `closed_status()` reads the channel for an epitaph and finds none, and the binding records PeerClosed. NotFound is a different path, `Realm::provider_serves()`, for a consumer that starts after the root knows of the exit, which `check_echo()` checks for echo. The two read differently on purpose, and the case pins the first.

What the case does, in order, and what each step proves:

| Step | Expected | What it shows |
|---|---|---|
| open `/files/hello.txt` and read one byte | Ok | a connection held before the stop, known to work |
| `connect_status("/files/stop")` | PeerClosed | the Open reached `files`' stop node (`CONNECT` is in `r*`), `files` took the channel and the channel closed with nothing on it; a refusal would be an epitaph, a status other than PeerClosed |
| wait on the held connection | PeerClosed | the connection of C1 is closed with the process |
| `open("/files/hello.txt")` | PeerClosed | the entry N is closed, with no epitaph, and the root said nothing |
| `open("/files/sub/nested.txt")` | PeerClosed | the binding keeps the answer: no second read of the channel |
| `open("/files-sub/nested.txt")` | PeerClosed | another entry of the same provider is closed too |
| `connect("/files/stop")` on a fresh channel | PeerClosed | the stop node is unreachable once its entry is closed |

**The order**: the case runs last, after every case that uses `/files`, `/files-sub`, `/files-file`, `/ghost`, `/files-denied` and `/files-wide`. Of the six, two stay connected to `files` and die with it, `/files` and `/files-sub`; `/ghost` and `/files-file` were closed by `files`' server with a status, `/files-denied` and `/files-wide` by the root, and keep that status. A case that read a file after the stop would read PeerClosed. `check_spawn()` is the one later check that touches the entries without using them: `fsio::export_namespace()` clones every live entry for the program it starts, and when a write finds an entry's server gone it marks the entry closed and leaves it out. So moving the case ahead of `check_spawn()` still passes today (tried; see the table), and that is a property of `fsio` and not of this case. The case is last so that nothing depends on it, and a case added later has to go above it. The comment on the function says so.

**The waits**, bounded the way the rest of the file is (`object_wait_one` takes a deadline of 0 or `INSTANT_INFINITE` only, repair item 2): both are `Incoming::receive()`, an infinite wait for a message or PEER_CLOSED, as `check_echo()`'s are. What they rely on: `files` exits once the stop node has its channel, and the kernel closes every handle of a process that exits. Two things follow.

- A wrong path does not hang. `files`' server answers an Open it refuses with an epitaph on the channel, so the first wait returns at once with a status that is not PeerClosed, a line with FAILED says which, and the case returns before the second wait, which only an exit would end.
- A stop node whose handler does not exit leaves the first wait for good. The failure is then the one every hang in this file is: `boot_check.py` waits its timeout and reports `no "routing_checks: passed"` and `no "init: files exited with 0"`. There is no FAILED line, and the repair item says so.

The close order is the other thing it relies on, as `check_echo()` does: handles close in the order the process received them, and `files` received the entries' server ends at its start, the held connection during the case and the stop channel last (it drops the channel and does not close it). So the stop channel's PEER_CLOSED comes after the held connection's and the entries', and the first `open()` on the entry answers PeerClosed, never a write that is accepted and lost. The case does not allow for a loss, as `refused_by_provider()` and `check_echo()` do. If a kernel change ever closes the table newest first, this is the case that fails, with `open() on the entry of a provider that exited = Ok (expected PeerClosed) FAILED`.

**Change**
```cpp
// before                                              // after
check_echo();                                          check_echo();
                                                       // Last, whatever else is added above: ...
                                                       check_files_stop();

constexpr auto FILES_NAMES =                           constexpr auto FILES_NAMES =
    ".,hello.txt,numbers.txt,sub";                         ".,hello.txt,numbers.txt,stop,sub";
```
`boot_check.py`: the line is in `REQUIRED` for rel because the root logging the exit is part of what the case proves, and a case that passed with the line missing (the handler exiting with 1, say) would otherwise pass the boot. The count the script prints is now `7/7 required`.

**Reference**
None, this project's own tests; the case follows `stop_echo()` and `check_echo()`.

**Verification**
- `boot_check.py --out out/dc-step5`: dbg PASS, 44 passed, 4 skipped; rel PASS, 41 passed, 8 skipped, 0 failed, 7 of 7 required; host tests PASS, 151 passed. The counts of the shell's checks are unchanged, because the case is part of `routing_checks`, not the shell's checks.
- Twenty-four more rel boots on the final build, `--no-build --no-tests`, twelve with 4 CPUs and twelve with `SMP=2`: all PASS with 7 of 7 required and no FAILED line. `SMP=1` fails in the kernel before `init` prints a line (step 4 found it; not run again).
- Mutations, each built, booted on rel (40 s timeout) and reverted, the sources compared with copies after:

  | Mutation | Result |
  |---|---|
  | the case connects to `/files/sub/stop` | rel FAIL: `a connection to files' stop node = NotFound (expected PeerClosed) FAILED`, 5 of 7 required (`passed` and the exit line are missing) |
  | it connects to `/files/hello.txt`, a file | rel FAIL: the same line with BadType |
  | the handler does not exit | rel FAIL: `no "routing_checks: passed", "init: files exited with 0" within 40s`, no FAILED line: the wait hangs, as described above |
  | the handler closes the channel and does not exit | the same: the wait on the held connection hangs |
  | `files` adds no stop node | rel FAIL: the listing check (`read_dirents: the names of a directory files serves are not what it holds`) and the connect (NotFound), 5 of 7 required |
  | the handler exits with 1 | rel FAIL: `no "init: files exited with 0"`, 6 of 7 required, `routing_checks: passed` present: the required line is what fails it |
  | the case runs before `check_spawn()` | rel PASS: not a mutation of the code; the ordering is not enforced by a failure (see "The order") |

- The shell's checks and the root's bookkeeping after the exit: the rel log holds `init: files exited with 0`, then `routing_checks: passed` and `init: routing_checks exited with 0`; the shell's `spawn(routing_checks child)` still gets `exited with 0`, and the boot check counts 0 error lines. No component of the dev image uses `files` but `routing_checks`.
- clang-tidy on `routing_checks.cpp`: no diagnostic. `format_cpp.py --check` clean. `lint_docs.py --changed` clean.

**Deliberately not done**
- A timeout on the waits: no clock (repair item 2). The item gains the case's waits.
- A check of the root's side beyond the log line: the root has no host test, and `routing_checks` cannot see its state. A consumer that starts after the exit would be answered NotFound by `provider_serves()` (logged as `routes nowhere: NotFound at files (exited)`); the dev image has none, and `check_echo()` covers the rule for echo.
- The `files` console line for the stop (`files: stopping, ...`) is not required by the boot check; the root's `init: files exited with 0` is.

**How it was found**
None. The ordering note comes from trying the case before `check_spawn()`, which passed, and reading `export_namespace()` to see why.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. A consumer that held the entry reads PeerClosed on the first `open()` after the provider exits, and one that starts later reads NotFound. What differs between the two paths, and who writes the NotFound?
2. The case waits on the stop channel first and on the held connection second. Why does the first wait's end imply the held connection is closed already, and what in the kernel does that rest on?
3. Skipping the exit and sending the case to a wrong path both fail the boot. Which one prints a FAILED line, why can the other not, and what would it take to make it?
