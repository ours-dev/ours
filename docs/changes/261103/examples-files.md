# examples-files changes

## Title: files serves a second declared directory through a handler node, whose Read waits two turns of the Loop (2026-11-04)

**Files**
- `examples/files/files_server.cpp` — `SlowDocs`, a `NodeHandler` that opens a directory and one file, `slow.txt`, and answers a Read `Pending`; its own channel pair and a Loop slot that ends the Read; `Outgoing::add_handler("/export/slow", slow)`; the Loop's table is one slot larger (65)
- `examples/files/BUILD.gn` — the manifest declares `slow` at `/export/slow` (`r*`) and exposes it (`r*`)
- `kernel/products/image/dev/BUILD.gn` — root.cm offers `slow` from `#files` to `#routing_checks`
- `system/service/init/test/routing_checks/BUILD.gn` — routing_checks uses `slow` at `/slow` (`r*`)

**Symptom**
Latent: nothing on the target ran a request that waits. The Loop's `Suspend` and `resume` (step 7), the core's `Pending` and `complete` (step 8) and the glue between them (step 9, `docs/changes/261103/ioservice.md`) had host tests on a fake kernel and nothing on real ports, channels and a second CPU. The design asks the first provider for "a delay of the provider's own making, so that `Pending` runs on real kernel objects without a disk".

**Cause**
A request that waits needs something to wait for. There is no disk and no clock, so the handler makes its own: the one thing that is certain to happen "later" and that a handler can arrange is a message to itself. `SlowDocs` makes a channel pair at start and puts a Loop slot on one end. `read()` writes one message on the other end, which names the Read (the ticket the Server gave it, its offset and count), and answers `Pending`. The message makes the slot fire after the packets the Loop already holds, so other connections are served first, and the slot's callback ends the Read with `Outgoing::complete()`. The message carries everything the answer needs, so the handler keeps no table of pending requests.

**Change**
```cpp
// SlowDocs::read(): the Read is written down and answered later
auto const wake = Wake{.index = ticket.connection.index, .generation = ticket.connection.generation,
                       .serial = ticket.serial, .turns = SLOW_TURNS - 1, .offset = offset, .count = count};
channel_write(m_post, ..., &wake, sizeof(wake), nullptr, 0);
return ioservice::Pending{ticket};

// SlowDocs::on_signals(): one firing of the slot
channel_read(m_wake, ..., &wake, ...);
if (wake.turns > 0) { wake.turns -= 1; channel_write(m_post, ..., &wake, ...); return loop::Action::Rearm; }
m_outgoing->complete(Ticket{{wake.index, wake.generation}, wake.serial}, ReadResult{ok(bytes)});
return loop::Action::Rearm;
```
The directory has two objects, a directory (cookie 0) and the file (cookie 1); `open()` answers at once; `read_dirents()` and `get_backing_memory()` are NotSupported, which is honest for an example that is not a filesystem. The manifest, the offer and the use follow the pattern of `ghost`: declared and exposed by `files.cm`, offered by `root.cm`, used by `routing_checks.cm`.

**The delay is two turns, and one is not enough.** The design says "a Loop turn later". The first version posted the message once and completed on the first firing. Mutating the library so that the `Binding` answers `Rearm` for a `Pending` request, which is the wrong answer, passed the whole boot: the message is posted while the connection's callback is still running, so its packet is queued ahead of the connection's own packet for the next message the client wrote behind the Read; the Read was ended first, the connection was found not pending, and nothing told the difference (the only trace was a `BadState` from the resume in a log line). With the message posted again after its first firing, the connection's packet comes between the two, the Read is still pending when it is read, the core drops the connection, and the second of two back-to-back Reads reads PEER_CLOSED. `SLOW_TURNS` is 2 and the comment on it says why.

Limits checked, none raised:

| Limit | Value | Use |
|---|---|---|
| files' Loop table | 65 slots (`Outgoing::MAX_CONNECTIONS` 32 + `DOCS_CONNECTIONS` 32 + the wake slot), under the port's 1,024 observers | `static_assert` in the source |
| `Outgoing` connections | 32 | the root's, the entry of `/slow`, and one per file open; the 40 abandoned Reads of routing_checks are alive at most a few at a time, because the provider takes one Open of an entry per turn |
| `Outgoing` handler table | 8 | 2 used: the Subtree node of `docs` and the Handler node of `slow` |
| `Outgoing` nodes | 32 | 5 used: root, `svc/`, `export/`, `docs`, `slow` |
| the outgoing channel's queue | 64 messages | 5 Opens forwarded to `files` (`files`, `files-sub`, `files-file`, `slow`, `ghost`) |
| routing_checks' directory uses | 15 of 15 (`init::MAX_DIRECTORY_USES`) | the 14 it had, and `/slow`: **no room is left** (`docs/repairs/261103/routing-checks.md`, item 2) |
| routing_checks' namespace | 16 of 16 bindings (`fsio::MAX_BINDINGS`) | the same use plus `/svc` |
| the root's handle charge | `HANDLES_PER_COMPONENT` 38 of 8 + 2 × 15 | already budgeted for 15 uses; its `static_assert`s hold |

The brief of this step counted 10 of 15 uses and 11 of 16 bindings for routing_checks; the manifest had 14 directory uses and 15 bindings before this change.

**Reference**
None, this project's own design: `.agents/tasks/directory-capability/DESIGN.md`, "The first provider: `examples/files`" and the note under the Staging table on the delay on the target.

**Verification**
- `boot_check.py --out out/dc-step9`: dbg 46 passed, 5 skipped, 0 failed; rel 44 passed, 8 skipped, 0 failed, 7 of 7 required, `routing_checks: passed`; host 172. Ten more rel boots with 4 CPUs: 10 of 10; nine with `SMP=2`: 9 of 9. The existing `/files` cases (the Subtree path at `export/docs`, `stop`, `ghost`, `files-file`, the two closed entries) pass unchanged.
- Target mutations (`docs/changes/261103/routing-checks.md` has the table): the wake slot never fires (a Read is never ended) hangs the boot; the library cases are in `docs/changes/261103/ioservice.md`.
- `files_server`: `OUTGOING_STORAGE` 14,984 bytes, `DOCS_STORAGE` 16,776, `SLOT_STORAGE` 1,560 (65 slots).
- clang-tidy and formatting: clean (`docs/changes/261103/ioservice.md`).

**Deliberately not done**
- A listing of the slow directory and a VMO for its file: both NotSupported. Neither is what the case is for.
- A delay that depends on the file or the offset, or a timer: the Loop has none, and one number is enough to show the Read waits.
- Pending `open()`: the example's Open answers at once; the pending Open is covered on the host (`loop_server_test.cpp`) and has no use on the target yet.
- A second handler or a `value` other than 0: one node.

**How it was found**
The two-turn delay by mutation: the `Rearm` mutant of the library passed the first version of the cases (`rel PASS`, 7 of 7), which showed that the cases did not test what they were written for. The log line `files: cannot end a pending Read: BadState` that the mutant printed is what pointed at the order of the packets.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does `SlowDocs` write its Read down in a message instead of keeping a table of pending requests indexed by connection?
2. With a delay of one Loop turn, a `Binding` that answers `Rearm` for a pending request behaves like one that suspends. Which two packets are in the queue, in which order, and why does a second turn change it?
3. Which of the example's limits is now full, and what has to be raised before routing_checks can use another directory?
