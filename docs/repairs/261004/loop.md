# loop outstanding repairs

## 1. Loop::add() finds a free slot by scanning the table from index 0

**Severity**
Low: a performance observation, not measured. Nothing is wrong, and today's tables are small.

**Symptom**
Latent, cannot be observed today. Each add() costs time in proportion to the number of slots in use below the first free one. The root will add a slot for every Open and every Clone (one vfs connection each), and D27 sizes its table at 3 slots per component plus one per connection, up to a few hundred, so an Open under load would walk most of the table before it is served, on the one thread everything waits behind (D16).

**Cause**
`Loop::add()` (`system/lib/loop/src/loop.cpp`) looks for the lowest slot whose handle is Invalid, starting at 0 each time. Taking the lowest free slot also reuses freed slots soonest, which is what makes the generation check in `run()` matter, but it is not needed for correctness: any free slot would do.

**Change**
Measure first, as CLAUDE.md asks of any performance work: a benchmark in `benches/` that fills a table of the root's size, frees slots in a scattered order, and times add() against the number in use, before deciding with the user. Candidates:
- a free list threaded through the free slots (an index in each free slot, the head in the Loop): O(1) add() and free, no extra memory, at the cost of reusing the most recently freed slot first;
- a bitmap of free slots scanned a word at a time with a count-trailing-zeros instruction: still a scan, 64 slots per step, and keeps "lowest free first".
Rejected: none yet; the choice waits for the numbers.

**Reference**
Fuchsia's async-loop has no table: the caller owns each `async_wait_t` and its address is the key (`sdk/lib/async-loop/loop.c:23-31`), so it allocates nothing. Zircon allocates handles from `HandleTableArena::Alloc()` (`zircon/kernel/object/handle.cc:103`), over `fbl::GPArena::Alloc()`, which pops a lock-free free list before it grows the arena (`zircon/kernel/lib/fbl/include/fbl/gparena.h:219-226`).

**Acceptance**
The benchmark's numbers are in the change record, the user has chosen, and `system/lib/loop/test` still passes, with a test that a slot freed and taken again gets a new generation whichever slot the new policy picks.

## 2. Loop::run() makes two system calls of its own for every message it dispatches

**Severity**
Low: a performance observation, not measured. Nothing is wrong; it is the cost of I4.

**Symptom**
Latent, cannot be observed today: no server runs on the Loop yet. Once the root does (step 13 of `.agents/tasks/component-framework/PLAN.md`), a client that sends N messages costs the root 2N system calls on top of the N `channel_read`s its callbacks make: one `port_wait` to take each packet, and one `object_wait_async` to re-arm after each message.

**Cause**
`Loop::run()` (`system/lib/loop/src/loop.cpp`) takes one packet per `port_wait`, because ours's `port_wait` hands out one packet (`system/public/ours/syscalls.gni`). It re-arms after every callback, because a callback handles one message and a wait is one-shot (I4 in `docs/design/261004/component-framework.md`). Both are deliberate. The first follows Zircon's ABI. The second is what serves a busy client in turns instead of letting it starve the rest. Their price is a system-call round trip that a batching design would share between several messages.

**Change**
Measure first, as CLAUDE.md asks of any performance work. Write a benchmark in `benches/` in which one client floods a channel and several others send one message each. Record the root's system calls per message, the time per message, and the latency of the quiet clients, before deciding with the user. Candidates:
- a `port_wait` that hands out up to N packets, as `epoll_wait`'s `maxevents` does: one call for many packets, but an ABI change to the kernel, which is the user's decision;
- callbacks that read up to K messages before answering Rearm: fewer re-arms, at the cost of weakening I4 from one message per turn to K;
- leaving it, as Fuchsia does: `async_loop_run_once()` takes one packet per `zx_port_wait`, and its FIDL binding begins a new wait after every message.

Rejected: an edge-triggered wait (Zircon's `ZX_WAIT_ASYNC_EDGE`), which ours does not have. It would also not remove the re-arm, since the wait stays one-shot.

**Reference**
Fuchsia, at `51f05104c13`: `async_loop_run_once()` (`sdk/lib/async-loop/loop.c:347-407`, one `zx_port_wait` at `:355`); `AsyncBinding::MessageHandler()` and `CheckForTeardownAndBeginNextWait()` (`sdk/lib/fidl/cpp/wire/async_binding.cc:83-108`, `:179-198`), which re-arm after every message. Linux, at `62cc90241548`: `ep_send_events()` (`fs/eventpoll.c:2128-2160`) copies up to `maxevents` ready items out in one `epoll_wait`.

**Acceptance**
The benchmark's numbers are in the change record, the user has chosen, and `system/lib/loop/test` still passes, including `LoopTest.RunServesABusyClientInTurns`, or a test of the new fairness bound if the user chose K messages per turn.
