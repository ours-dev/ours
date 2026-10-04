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
- Added in review (2026-10-04): two more `Run*` tests for step 1, so the binary now runs 18 tests, 6 passing and 12 failing until `run()` exists. Two wrong `run()`s, built in a scratch copy, passed the 16 tests above. The first let an index equal to the table's size through (`index > size` for `index >= size`). That read past the fixture's array, and the bytes there happened not to match generation 0. `LoopRun.DropsAnIndexOnePastTheTableWhateverLiesThere` now puts a second Loop's slot in use right after the first Loop's table, so such a `run()` calls the second Loop's callback. The second wrong `run()` judged "in use" by a non-null callback and left the callback in place on Close. The only free slot the tests named had never been used, so its callback was null. `LoopTest.RunDropsAPacketForASlotFreedInItsNextLife` forges the key of a slot freed by Close, at its next generation, and that `run()` calls the old callback again. A scratch reference `run()` passes all 18. A `run()` that skips the in-use check entirely still fails only by crashing, on the never-used slot's null callback.

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

## Loop::run() dispatches packets, written by the AI for the MVP, and check_loop() runs on both images (2026-10-04)

**Files**
- `system/lib/loop/src/loop.cpp` — `run()` replaces the stub that answered NotSupported: `port_wait`, the drop rule, the callback, then re-arm or close. Written by the AI on the user's instruction of 2026-10-04; the hand-writing exercise of P3 stays open
- `system/lib/loop/include/loop/loop.hpp` — the spec's sentence on who writes `run()` now says so; the spec itself is unchanged. `Callback`'s comment gains a paragraph on when Rearm is safe (added in review)
- `system/lib/loop/test/loop_test.cpp`, `test/BUILD.gn`, `README.md`, and the root `BUILD.gn` (added in review) — comments that said the `Run*` tests fail until `run()` exists; no test changed
- `examples/shell/shell_tests.cpp` — `check_loop()` is called from `shell::tests::run()`, right after `check_port_observer_limit()`, on both images
- `examples/shell/shell_tests.hpp` — `check_loop()`'s comment says where it runs
- `docs/repairs/261004/loop.md` — item 2: two system calls of the Loop's own per message, a performance observation

**Symptom**
Before this change `Loop::run()` dispatched nothing and answered NotSupported. 12 of the 18 host tests failed, and `check_loop()` was compiled into the shell but never called, because wired in it printed `shell: loop run(two channels) = NotSupported (expected TimeOut)` and `shell: loop checks FAILED`. With no dispatch, nothing in user space could wait on many handles at once, and step 13's root needs exactly that.

**Cause**
This entry has no bug to explain. Its learning value is why each of `run()`'s four steps is where it is, and what breaks if one is skipped or moved. The objects are the first entry's: one port, a table of slots `{handle, signals, callback, context, generation}`, and at most one live wait per slot in use, keyed `generation << 32 | index`.

Step 1, the type comes first. `port_queue` delivers User whatever type its caller wrote (`system/public/ours/port.hpp`, `PacketType`), so the type is the one field of a packet that a holder of a duplicate port cannot forge. The key can be forged freely. `check_loop()` queues a User packet carrying slot 0's live key, `Key{0, 0}`, while slot 0's real wait is still armed. Suppose a `run()` skipped the type check. a's callback would run with the User payload's zeroes as its observed signals. If it answered Rearm, `arm()` would put a second wait on slot 0, and both could then queue packets. That breaks "at most one live wait per slot", which is the whole argument for a table no larger than PORT_MAX_OBSERVERS never meeting NoResource from the port. If it answered Close, a's handle would be closed for a message a's client never sent.

Step 1, the bound comes before the slot is touched. `std::span::operator[]` does not check its index here, so `m_slots[size]` is whatever memory follows the table. In `LoopRun.DropsAnIndexOnePastTheTableWhateverLiesThere` that memory is another Loop's slot in use at generation 0. A `run()` that tested `index > size` would call that Loop's callback from this Loop's packet.

Step 1, "in use" is the handle. The generation alone is not enough. Step 4 bumps a slot's generation when it frees it, so a free slot already carries the generation of its *next* life. A packet with that key passes the generation check, although no wait of this Loop's ever had it. The Slot's own definition is that a free slot's handle is Invalid, and step 4 always sets it so. A callback pointer that is still non-null says nothing: the slot may have been freed. `LoopTest.RunDropsAPacketForASlotFreedInItsNextLife` forges exactly this key.

Step 2, the slot stays in use while its callback runs. The callback may `add()`, and `add()` takes the lowest slot whose handle is Invalid. Suppose a `run()` freed the slot first and then called the callback, to "save" the step-4 bookkeeping. Then an `add()` inside the callback could take this very slot for a new client, and the Close that follows would close the new client's handle. Keeping the slot in use until the callback has answered also means `slot` is still the same slot afterwards. The table is the caller's span and never moves, so the reference taken before the call is still good after it, however many `add()`s ran.

Step 3, one callback per packet, then re-arm. This is I4. The wait is level-triggered, so a handle that still has messages queues its next packet at once, behind every packet already on the port. `LoopTest.RunServesABusyClientInTurns` and `check_loop()`'s "abaa" show it: a's three messages do not starve b's one.

Step 4, a failed re-arm closes the slot. If `arm()` fails, nothing is armed. A slot left in use would never fire again: its handle and its slot would leak for as long as the Loop lives, and its client would wait forever for a reply. Closing it gives the client PeerClosed, the only answer a client of a one-thread server can still get. The close is safe for the same reason a Close answer is (I3): the slot's only wait fired, and its packet is the one `run()` just took, so nothing of the slot is left on the port. The generation is bumped so that every key of this life is retired. The callback and context are cleared too. That is not needed for correctness, since the handle decides "in use", but a freed slot that kept them would hold a context pointer to an object its owner may already have destroyed. `add()`'s failure path clears the same fields.

`run()` has no error of its own. A failed callback, a failed re-arm and a forged packet are all dealt with inside one dispatch, and the loop goes on. The only way out is `port_wait` answering something other than Ok. With deadline 0 that is TimeOut once the port is empty, which turns `run(0)` into Fuchsia's "run until idle".

**Change**
Before (`system/lib/loop/src/loop.cpp`):

```cpp
auto Loop::run([[maybe_unused]] ours::Instant const deadline) -> ours::Status {
    // Written by the user (P3): see the spec above.
    return ours::Status::NotSupported;
}
```

After, with the comments shortened:

```cpp
auto Loop::run(ours::Instant const deadline) -> ours::Status {
    while (true) {
        auto packet = ours::PortPacket{};
        if (auto const status = ours::port_wait(m_port, deadline, &packet);
            status != ours::Status::Ok) {
            return status;
        }
        // 1. Drop what is no wait of this Loop's.
        if (packet.type != ours::PacketType::SignalOne) {
            continue;
        }
        auto const key = Key::from_raw(packet.key);
        if (key.index() >= m_slots.size()) {
            continue;
        }
        auto &slot = m_slots[key.index()];
        if (slot.m_handle == ours::Uandle::Invalid || slot.m_generation != key.generation()) {
            continue;
        }
        // 2. The callback, with the slot still in use.
        auto const action = slot.m_callback(slot.m_context, packet.signal.observed);
        // 3. Rearm.
        if (action == Action::Rearm && arm(key.index()) == ours::Status::Ok) {
            continue;
        }
        // 4. Close, or a failed re-arm.
        auto _ = ours::handle_close(slot.m_handle);
        slot.m_handle = ours::Uandle::Invalid;
        slot.m_signals = ours::Signals::None;
        slot.m_callback = nullptr;
        slot.m_context = nullptr;
        ++slot.m_generation;
    }
}
```

The shell's `run()` gains one line, `check_loop();` after `check_port_observer_limit();`. It is not gated on the image: the check makes its own port and channels and needs nothing either image hands the shell.

The review added one paragraph to `Callback`'s comment: a callback answers Rearm only once it has consumed what asserted the signal. The wait is level-triggered, so a Rearm with the message still unread, or with only PeerClosed or Terminated asserted, has its packet queued again at once, and `run()` calls the callback on every turn without end: a spin on the root's one thread, and a `run(0)` that never finds the port empty. I4 keeps it from starving the other slots, since the new packet queues behind theirs, but nothing in `run()` can stop it: the Loop does not know what consuming a signal means for an object, so the rule belongs to the callback. Step 13's callback on a shell's Terminated is the first that could break it, by answering Rearm instead of Close. Linux's epoll(7) warns of the same trap for level-triggered descriptors, and Fuchsia's FIDL binding tears down, rather than waiting again, when a wait finishes with PEER_CLOSED and nothing READABLE (`ChannelWaiter::HandleWaitFinished()`, `sdk/lib/fidl/cpp/wire/transport_channel.cc:185-193`, at `51f05104c13`).

Rejected:
- A private `dispatch(PortPacket const &)` helper, called from `run()`. It would add a member to the header, whose `run()` comment is the spec of one function. The body is about thirty lines, which the spec expected.
- Freeing the slot (step 4) through a helper shared with `add()`'s failure path. The two differ in the generation, which `add()` must not bump because it handed out no key, so a shared helper would need a flag. Two short blocks are clearer.

**Reference**
- Fuchsia, at `51f05104c13`: `async_loop_run()` (`sdk/lib/async-loop/loop.c:327-337`) calls `async_loop_run_once()` (`:347-407`) until it answers anything but ZX_OK. That is the same exit rule as here, and `async_loop_run_until_idle()` (`:339-345`) is `async_loop_run(loop, 0, false)` with TIMED_OUT mapped to ZX_OK. `async_loop_run_once()` takes one packet with `zx_port_wait` (`:355`), then dispatches on its key and type. A SIGNAL_ONE packet's key is cast back to the `async_wait_t` it was armed with (`:374-380`), with no check, because `async_loop_cancel_wait()` guarantees no stale packet is left. That is the guarantee ours cannot make without `port_cancel`, hence step 1.
- Fuchsia's FIDL server binding is where steps 2-4 live in Fuchsia. `AsyncBinding::MessageHandler()` (`sdk/lib/fidl/cpp/wire/async_binding.cc:83-108`) dispatches one message, then calls `CheckForTeardownAndBeginNextWait()` (`:179-198`), which begins the next wait with `async_begin_wait`. If that fails, it calls `PerformTeardown()`. One message, then the wait again, and teardown when the wait cannot be begun: this is step 3, and step 4's failed re-arm.
- Linux, at `62cc90241548`: `ep_poll()` (`fs/eventpoll.c:2255`) hands out up to `maxevents` ready items per `epoll_wait` through `ep_send_events()` (`:2128-2160`). That is where Linux differs from both Fuchsia's loop and this one, which take one packet per wait (repair item 2).

**Verification**
- Host tests: `prebuilt/ninja/ninja -C out/bringup.x86 phony/system/lib/loop/tests && out/bringup.x86/host/obj/system/lib/loop/test/loop` prints `[  PASSED  ] 18 tests.` (it was 6 passed and 12 failed with the stub).
- `TMPDIR=$PWD/out/tmp python3 .agents/skills/ours-verify/scripts/boot_check.py`: dbg PASS (28 passed, 1 skipped, 0 failed), rel PASS (25 passed, 3 skipped, 0 failed). Both logs show `shell: loop run(two channels) = TimeOut`, `shell: loop run(b's peer closed) = TimeOut`, `shell: loop close(b1) after the Loop closed it = BadHandle`, `shell: loop run(forged packets, then c) = TimeOut` and `shell: loop checks passed`.
- clang-tidy (`prebuilt/clang/llvm-23/bin/clang-tidy -p out/bringup.x86`) reports nothing on `loop.cpp`, in either of its two compile entries (the host test and the user toolchain), and nothing new on `loop_test.cpp` or `shell_tests.cpp`. `scripts/format_cpp.py` finds every changed file already formatted.

**Deliberately not done**
- No row in `docs/authored.md`. P3 reserved `run()` for the user, to write by hand under a time limit. On 2026-10-04 the user asked for an unattended MVP and for the AI to write it, so the exercise stays open. To take it up, put the stub back (`git show 9b023747:system/lib/loop/src/loop.cpp`). The 12 `Run*` tests then fail again, the user writes the body against them, and the AI reviews it as a strict interviewer. The row goes in after that review.
- The concurrency argument CLAUDE.md asks for before a concurrency decision, in which the user writes the invariant, who races, and the worst interleaving, is skipped for this run, on the same instruction. The design's "Concurrency: one thread and one Loop" section is still the answer that argument would be graded against.
- The two system calls `run()` adds per message are not optimised; they are recorded as `docs/repairs/261004/loop.md` item 2, to be measured first.
- No `quit()` and no `remove(key)`, as the first entry says.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- A User packet arrives carrying the key of slot 0, which is in use with its wait still armed. If `run()` skipped the type check and the callback answered Rearm, how many live waits would slot 0 have, and which bound would that break?
- Why must step 4 free the slot after the callback returns rather than before calling it? Give the sequence in which a callback's own `add()` and an early free close the wrong client's handle.
- `arm()` fails in step 3. What would a `run()` that left the slot in use do to the client and to the table, and what does Fuchsia's FIDL binding do in the same case?
