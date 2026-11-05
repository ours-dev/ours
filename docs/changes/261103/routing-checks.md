# routing-checks changes

## Title: routing_checks reads a slow file, writes two Reads back to back and abandons forty (2026-11-04)

**Files**
- `system/service/init/test/routing_checks/routing_checks.cpp` — `check_slow()` and four cases under it; `write_read()`, `receive_read()` and `same_bytes()`; the namespace now holds 16 bindings
- `system/service/init/test/routing_checks/BUILD.gn` — the use of `slow` at `/slow` (`r*`)

**Symptom**
Latent: the target did not run a request that waits, a connection left unread meanwhile, or a connection whose client went away with a request pending. These are the rows "a Read on the slow file", "two Reads written back to back on one connection" and "a Read written and the channel closed at once, repeated more times than the provider has connections" of the design's table of target cases (step 9).

**Cause**
The behaviours that matter here only exist between a client and a provider running on different CPUs: the client writes without waiting, the provider's Loop serves other slots between the request and its answer, and a client can close its channel at any point. A host test on a fake kernel shows that the glue is right; only the target shows that the same events happen on a port, a channel and a second CPU.

**Change**
`check_slow()` runs after `check_files()` and before everything that ends `files`. Four cases, in the style of `check_files_*`:

| Case | What it does | What it shows |
|---|---|---|
| `check_slow_open()` | opens `/slow` itself; asks for write (the server refuses), a name the handler lacks (the handler refuses), a path below the file, the file as a directory | the handler node's directory and the two layers of refusal |
| `check_slow_read()` | writes a Read of `/slow/slow.txt` and does not wait; reads `/files/hello.txt` whole; reads the slow file whole on a second connection; then takes the first Read's answer | the bytes; the provider answers another connection of its own meanwhile, on the Loop the pending Read waits on |
| `check_slow_back_to_back()` | writes `Read(5)` and `Read(100)` on one connection with nothing between | two replies, in order (`"slow "`, then `"hello\n"`): the second Read starts where the first ended, because the offset moves with a Read that is completed |
| `check_slow_abandoned()` | forty times: opens the file, writes a Read, closes the channel at once; then reads the file whole | the provider keeps answering: no connection leaked (a leaked connection runs the 32 of the outgoing directory out, and the normal read answers `NoResource`) |

```cpp
// check_slow_abandoned(), the loop
auto const file = open_or_fail(SLOW_FILE, Flags::PermReadBytes);
auto _ = write_read(file, next_txid(), 100);
auto _ = ours::handle_close(file);
...
check_contents(SLOW_FILE, SLOW_TEXT);   // NoResource here if any abandoned connection was kept
```

What the cases rely on, since there is no clock (repair item 2 of `docs/repairs/261029/routing-checks.md`): every wait for a reply is `Incoming::receive()`'s, infinite, for a message or PEER_CLOSED. A provider that refuses is answered with an epitaph, which the case reads as a status and fails on with a line. A provider that never ends a pending Read leaves the wait for good, and the failure is the missing `routing_checks: passed`, which `boot_check.py` reports (`no "routing_checks: passed" within Ns`). The case does not wait for anything the provider could be slow at beyond that: the forty abandoned Reads are not waited for at all, and at most a handful are alive at once, however fast this program writes them, because the provider takes one Open of an entry per turn and releases an abandoned connection a few turns after it starts it. `check_files_stop()` is still last, and its comment lists `/slow` among the entries that die with `files`.

**Reference**
None, this project's own design: the table of target cases in `.agents/tasks/directory-capability/DESIGN.md`.

**Verification**
- `boot_check.py --out out/dc-step9`: rel 44 passed, 8 skipped, 0 failed, 7 of 7 required; ten more rel boots with 4 CPUs, 10 of 10; `SMP=2`, 9 of 9.
- **Target mutations**, each built into the image and booted (rel, `--timeout 60`), source reverted after:

  | Mutation | Result |
  |---|---|
  | `files`' wake slot never fires, so a pending Read is never ended | hang: `no "routing_checks: passed"` within 60 s |
  | the `Binding` answers `Rearm` for a pending request | FAILED: the answers to both of two back-to-back Reads are `PeerClosed` |
  | `complete()` writes the reply on another connection's channel | FAILED, 8 lines: the slow reads read `PeerClosed` |
  | a connection whose client went away is not released | FAILED: the final read of the slow file is `NoResource` (and the shell's `/vfs` check, which shares the leaked table) |
  | `complete()` does not resume the slot | hang: `no "routing_checks: passed"` within 60 s |

  The `Rearm` mutant passed the first version of these cases, whose provider answered after one Loop turn; the delay is two turns now (`docs/changes/261103/examples-files.md`).
- The unmutated image passes after every mutation (checked at the end of each run).

**Deliberately not done**
- A bound on the waits. There is no clock; the missing `passed` line is the signal.
- A check that the provider's Read was still pending when the other connection was answered: the host tests hold the order, and on the target the order depends on which CPU runs first.
- The abandoned Reads are not counted on the provider's side; the final read is what shows them gone.

**How it was found**
None, new cases from the design. The `Rearm` mutant is what showed the first delay too short.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `check_slow_abandoned()` never waits for the forty Reads. Why does it not overrun the provider's 32 connections, and what does the final read answer when it did?
2. Why does the second of two back-to-back Reads begin at byte 5 and not at byte 0?
3. What does a missing `routing_checks: passed` mean for this file's cases, and which wait is the likely one?
