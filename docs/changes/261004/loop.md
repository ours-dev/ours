# loop changes

## The Loop library, with the spec and failing tests for its dispatch run(), which the user writes by hand (2026-10-04)

**Files**
- `system/lib/loop/include/loop/loop.hpp` — `Loop`, `Slot`, `Key`, `Action`, `Callback`; the comment on `Loop::run()` is the whole spec of the function the user writes
- `system/lib/loop/src/loop.cpp` — the constructor, the destructor, `add()` and `arm()`; `run()` is a stub marked `// Written by the user (P3): see the spec above.` that answers NotSupported. **Pending: written by the user (P3)**
- `system/lib/loop/test/fake_kernel.hpp`, `fake_kernel.cpp` — a host model of `port_wait`, `object_wait_async` and `handle_close`, defined under the vDSO's own symbol names
- `system/lib/loop/test/loop_test.cpp` — 16 host tests: 6 for what is written (pass), 10 named `Run*` that pin `run()` down (fail until it exists)
- `system/lib/loop/BUILD.gn`, `test/BUILD.gn`, `README.md`, `.clang-format`, `.gitignore` — scaffolded with copier from `tools/templates/cpp_library`
- `BUILD.gn` — `//system/lib/loop:tests` joins the host test list
- `examples/shell/shell_tests.cpp`, `shell_tests.hpp`, `BUILD.gn` — `shell::tests::check_loop()`, the on-target check, compiled into the shell but not yet called from `run()`
- `docs/repairs/261004/loop.md` — `add()`'s linear scan for a free slot, a performance observation

**Symptom**
None: a new library, and a preparation step. Today init waits on one handle at a time with `object_wait_one`: the launcher's pong (`system/service/init/init.cpp:441`), the shell's PeerClosed (`:463`), and the shell's TERMINATED (`report_exit`, `:306`). The root of the component framework has to serve /boot, /svc and the Launcher to every component at once on one thread (D16), and a thread blocked in `object_wait_one` on one handle serves nobody else.

**Cause**
Why a Loop at all. A server on one thread must sleep until any of many handles has something for it. ours has no `object_wait_many`, and even Zircon's (64 items) is the wrong tool: the set changes on every Open. Zircon's answer, which ours already has, is the port: each handle gets a one-shot wait with `object_wait_async`, which queues one packet on the port when the handle asserts a signal, and the thread sleeps in `port_wait` on the port alone. The packet carries a 64-bit key chosen when the wait was armed, and the key is the only thing that says which handle fired. The Loop is the bookkeeping around that key: a table of slots `{handle, signals, callback, context, generation}`, the key being `generation << 32 | index` (FS:311).

Why the generation. The worst case for a key that is only an index is the ABA problem. Suppose slot 5 serves connection X, and something closes X's handle while X's PeerClosed packet is still queued. Slot 5 is freed, and the next Open takes it for connection Y. Then the old packet is read: index 5, so run() calls Y's callback with X's signals, and Y's callback reads from Y's channel as if Y had closed, or closes Y. Nothing in the packet says it is old. The generation does: freeing slot 5 bumps it, so the old packet's key no longer matches and is dropped. Fuchsia avoids the same trap differently: its key is the address of the caller's `async_wait_t`, and `async_loop_cancel_wait` calls `zx_port_cancel_key` to pull the queued packet out before the wait object can die. ours has no `port_cancel` (FS:314), so the key has to carry the check itself.

Why only a slot's own callback may close it (I3). With the rule, the ABA case above cannot happen at all: when a callback runs, its slot's one wait has fired and its packet has already been taken off the port, so closing the handle then leaves nothing of the slot behind. The generation check becomes a defence, for packets no wait of the Loop's own could have queued. Without the rule, it would be the only thing standing between a stale packet and the wrong connection. That is why the design's exit path closes the process handle and nothing that has a slot: the /svc server end, each vfs connection and the outgoing client end see PeerClosed and close in their own callbacks.

Why the callback answers Rearm or Close instead of arming itself. One place, `run()`, decides whether a slot waits again, so "at most one live wait per slot, none for a free slot" is a property of `run()` and not of every callback in the root. That property is what makes the table's size a bound on the port's observers: PORT_MAX_OBSERVERS (1024, `system/public/ours/port.hpp:100`) counts armed waits and queued packets, and a table no larger than it can never meet NoResource from the port.

Why re-arm after every message (I4). The wait is level-triggered: armed on a channel that still has messages, it queues its packet at once, at the back of the port's queue. So a callback that reads one message and answers Rearm puts its client behind everyone else who is waiting; a client that sends a thousand messages is served one per round. A callback that drained its channel, or a `run()` that called the same callback again while the handle stayed readable, would let one client starve the rest.

Why `run()` is the function the user writes. It is where I3 and I4 are kept (P3 in `.agents/tasks/component-framework/PLAN.md`): the key check, the call, and the choice between re-arming and closing, in that order, are the whole of the Loop's correctness, in about thirty lines.

**Change**
The public shape (`loop.hpp`):

```cpp
enum class Action : u8 { Rearm, Close };
using Callback = auto (*)(void *context, ours::Signals observed) -> Action;

class Key final { /* generation << 32 | index; from_raw(), index(), generation(), raw() */ };

class Loop final {
  public:
    Loop(ours::Uandle port, std::span<Slot> slots);
    ~Loop();
    auto add(ours::Uandle handle, ours::Signals signals, Callback callback, void *context)
        -> ustl::Result<Key, ours::Status>;
    auto run(ours::Instant deadline) -> ours::Status;

  private:
    auto arm(u32 index) -> ours::Status;
    ours::Uandle m_port;
    std::span<Slot> m_slots;
};
```

And the function left to the user, as committed:

```cpp
auto Loop::run([[maybe_unused]] ours::Instant const deadline) -> ours::Status {
    // Written by the user (P3): see the spec above.
    return ours::Status::NotSupported;
}
```

Three choices go past FS §4.2's sketch (`add(handle, signals, fn, ctx) -> Key`, `run()`), each recorded as a deviation from the plan's wording, none touching a decision of the design's table:
- `run()` takes a deadline, as Fuchsia's `async_loop_run(loop, deadline, once)` does. The root passes INSTANT_INFINITE and never returns; the checks pass 0 and return at TimeOut once the port is empty, which is what lets a check run the Loop at all.
- The Loop takes its port instead of creating it. A check can then keep a duplicate with only the Write right and forge packets on it, the only way to put a stale key on a real port, since the Loop itself never produces one.
- `add()` returns `ustl::Result<Key, Status>` and leaves the handle with the caller on failure, because a server whose pool is full must still send the epitaph NoResource on the new server end before closing it (FS §4.2).

Testing on the host needed a seam, and it is the link line rather than a template or a virtual: `loop.cpp` calls `ours::port_wait` and the other two through `<ours/vdso.hpp>`, whose declarations carry the asm labels `ours_port_wait` and so on, and `test/fake_kernel.cpp` defines those three functions, which inherit the labels. The library is compiled unchanged for both; the target links the vDSO, the test links the fake. A `run()` that made any other system call would not link in the test, which is how the spec's "no other system call" is enforced. Rejected: a `Loop<Sys>` template, which would put the whole Loop in a header for the sake of a test.

The on-target check, `shell::tests::check_loop()`, runs the real Loop on four channels and a real port: a with three messages and b with one served "abaa"; b's peer closed, b's callback answering Close and the Loop closing b1 (a second close answers BadHandle); slot 1 taken again as Key{1, 1}; then, through the Write-only duplicate, packets with slot 1's first-life key, a free slot's key, an index past the table, and a's key on a User packet, all dropped, with c's packet behind them served. It is compiled into the shell and declared in `shell_tests.hpp`, but not called from `run()`: it prints FAILED until `Loop::run()` exists, and boot_check refuses a FAILED line. Wiring it is one line, `check_loop();` after `check_port_observer_limit();` in `shell::tests::run()`.

**Reference**
- Fuchsia, at `51f05104c13`: `async_loop_run()` (`sdk/lib/async-loop/loop.c:327-337`) loops over `async_loop_run_once()` (`:347-407`), which takes one packet with `zx_port_wait` (`:355`), turns a SignalOne packet's key back into the `async_wait_t` it was armed with (`:374-380`) and calls its handler through `async_loop_dispatch_wait()` (`:426-432`). Keys are the waits' addresses (`:23-31`); a handler re-arms by calling `async_begin_wait()`, which reaches `async_loop_begin_wait()` and `zx_object_wait_async` (`:579-600`); `async_loop_cancel_wait()` pulls a queued packet with `zx_port_cancel_key` (`:602-634`). The header's contract is `async_loop_run()` at `sdk/lib/async-loop/include/lib/async-loop/loop.h:144`: TIMED_OUT when the deadline passes, as ours's run(0) answers TimeOut.
- Zircon's `zx_object_wait_async` is always one-shot now (`ZX_WAIT_ASYNC_ONCE` is 0 and "superfluous", `zircon/system/public/zircon/syscalls/port.h:16-17`), and level-triggered unless `ZX_WAIT_ASYNC_EDGE` is given (`:19`); ours's has only the default (`system/public/ours/syscalls.gni`, `object_wait_async`).
- Linux, at `62cc90241548`: epoll is the same machine with other names. `EPOLLONESHOT` disables an item once it has been reported (`ep_deliver_event()`, `fs/eventpoll.c:2112-2113`), and `epoll_ctl(EPOLL_CTL_MOD)` arms it again (`ep_modify()`, `:1980`), which is Rearm. A level-triggered item that is still ready is put back at the tail of the ready list (`:2114-2123`), which is I4's "behind everyone else". And `epoll_event.data` is the key: a server that stores only the fd there meets the ABA problem when the fd number is reused, which is why event loops store a pointer or a generation in it.

**Verification**
- Host tests, today, with the stub: `prebuilt/ninja/ninja -C out/bringup.x86 phony/system/lib/loop/tests && out/bringup.x86/host/obj/system/lib/loop/test/loop` prints `[  PASSED  ] 6 tests.` and `[  FAILED  ] 10 tests`, every `Run*` test, the first failure in each being `run()` answering NotSupported where TimeOut (or BadType, BadHandle, BadState) was expected.
- The tests were checked against a reference `run()` written in a scratch copy and not committed: all 16 passed, and with `check_loop()` wired in, both images printed `shell: loop checks passed` and boot_check passed both.
- With the stub and `check_loop()` wired, the rel image prints `shell: loop run(two channels) = NotSupported (expected TimeOut)`, then `shell: loop served "" (expected "abaa")`, and ends `shell: loop checks FAILED`.
- As committed (not wired): boot_check passes both images, and clang-tidy is clean on every line this change wrote.

**Deliberately not done**
- `run()` itself: it is the user's, under a time limit, then reviewed as a strict interviewer would (P3). `docs/authored.md` gets its row after that review, not before.
- `check_loop()` is not called from the shell's `run()`, for the reason above.
- No `quit()`, unlike Fuchsia's `async_loop_quit()`: the root never stops, and the checks stop with deadline 0. It can come with the first program that needs to leave its loop from inside a callback.
- No `remove(key)`: closing a slot from outside its callback needs `port_cancel` (FS:314), and I3 forbids it until then.
- No static_assert on the table's size inside the Loop: the table is a span, sized where it is declared, and the root's static_assert against PORT_MAX_OBSERVERS belongs with its pool sizes in step 13 (D27).
- `add()`'s linear scan is left as it is and recorded in `docs/repairs/261004/loop.md`, to be measured first.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- A slot's handle is closed by code outside its callback while its PeerClosed packet is queued, and the slot is then taken by a new connection. What does the old packet do with and without the generation in the key, and why does I3 make the generation only a defence?
- Why does answering Rearm after reading one message serve a client that sent a thousand messages in turns, and what would a callback that drained its channel do to everyone else?
- Why can a table no larger than PORT_MAX_OBSERVERS never meet NoResource from the port, and which line of `run()` would break that if it were written wrong?
