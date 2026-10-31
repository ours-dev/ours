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
