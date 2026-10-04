# loop outstanding repairs

## 1. Loop::run() frees a slot whose re-arm failed without telling the slot's callback

**Severity**
Medium: wrong, but only when `object_wait_async` refuses the re-arm of a slot in use, which for a Loop that owns its port and sizes its table within PORT_MAX_OBSERVERS means the kernel answering NoMemory for the observer (`kernel/object/handle_table.cpp:248-251`).

**Symptom**
Latent, cannot be triggered today: nothing exhausts the kernel heap at boot. Once a re-arm is refused, whatever the slot's owner keeps for it leaks for good. The root's BootServer (step 13 of `.agents/tasks/component-framework/PLAN.md`) keeps a vfs connection per slot, from a fixed pool (D27), and releases it only on the paths where its callback answers Close (`BootServer::handle()`). After a refused re-arm the connection stays allocated, holding a channel handle value the Loop has already closed; enough of them and every Open is answered NoResource. A component's TERMINATED slot would lose its supervision the same way.

**Cause**
Step 4 of the spec (the comment on `Loop::run()` in `system/lib/loop/include/loop/loop.hpp`) treats a failed re-arm like a Close answer, and its reason holds: nothing is armed, so a slot left in use would never fire again. But the two are not alike for the slot's owner. A Close answer is the callback's own decision, made while it can still free what it keeps for the slot. A failed re-arm happens after the callback has answered Rearm, believing the slot lives on; `run()` then closes the handle, clears `m_context` and tells nobody. Nothing ever calls that callback again, so its owner has no moment at which to clean up, and the `Callback` contract has no call that says "your slot is gone".

**Change**
The user's decision, since it changes the spec of the function P3 reserves for them. Options:
- (recommended) Before step 4 frees a slot whose re-arm failed, call its callback once more with `observed` set to `Signals::None`, and ignore the answer. A real SignalOne packet never observes None: a port observer fires only when the object asserts one of the signals it waits for (`Dispatcher::attach_port_observer()` and `notify_observers_locked()`, `kernel/object/dispatcher.cpp`), so None is unambiguous. `Callback`'s comment gains one rule, a callback told None frees its state, and `system/lib/loop/test` gains a test in which the fake refuses a re-arm. `add()` is unchanged.
- A second function pointer per slot, an `on_close(context)` that `add()` takes and `run()` calls whenever it frees a slot, for a Close answer or a failed re-arm. Clearer at the call site, but a wider Slot and a fifth parameter on every `add()`, for a case that is rare.
Rejected: leaving `run()` as it is and asking each callback to cope, since nothing ever calls the callback again; and having `run()` return on a failed re-arm, since the root's `run()` never returns and one failed connection must not stop the rest.

**Reference**
Fuchsia, at `51f05104c13`: `AsyncBinding::MessageHandler()` re-arms through `CheckForTeardownAndBeginNextWait()` (`sdk/lib/fidl/cpp/wire/async_binding.cc:179-198`), which on a failed `Begin()` records `UnbindInfo::DispatcherError(status)`. `MessageHandler()` then calls `PerformTeardown()` (`:372-385`), which reaches `AsyncServerBinding::FinishTeardown()` (`:432-460`), and that calls the server's `on_unbound` handler with the reason. The owner of a Fuchsia binding always hears why it ended; the owner of a Loop slot does not.

**Acceptance**
`system/lib/loop/test` passes with a new test in which the fake's `object_wait_async` refuses a re-arm and the slot's callback is told before the slot is freed, and the root's callbacks free their state when told. boot_check passes both images.

## 2. Loop::add() finds a free slot by scanning the table from index 0

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

## 3. Loop::run() makes two system calls of its own for every message it dispatches

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
