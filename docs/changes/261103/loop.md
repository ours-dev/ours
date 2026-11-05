# loop changes

## Title: a Waiter can answer Suspend and leave its slot in use with nothing armed, and Loop::resume arms it again (2026-11-03)

**Files**
- `system/lib/loop/include/loop/loop.hpp` — `Action::Suspend`; `Slot::Flags` (a private nested `enum class : u8` with `Suspended`) and `Slot::m_flags`; `Loop::resume(Key) -> ours::Status`; the private `Loop::free_slot()` and `Loop::fail_arm()`; the comments of the namespace, `Action`, `Waiter` (the answers, the spin rule, `on_rearm_failed()`, the lifetime), `Slot`, `Loop::~Loop`, `add()` and `run()` (steps 1 to 3, I1, I3, I4, the live-wait invariant and the paragraph on the key) say what Suspend and resume do
- `system/lib/loop/src/loop.cpp` — `resume()`; step 1 of `run()` also drops a packet that names a suspended slot; step 3 marks the slot on `Suspend`; step 4 and the failed re-arm go through `free_slot()` and `fail_arm()`, which `resume()` shares
- `system/lib/loop/test/loop_test.cpp` — `Answer::Suspend` for the test `Client`, `LoopTest::add_suspended()`, 17 tests (27 to 44); the comment of the `sizeof(Slot)` assertion
- `system/lib/loop/README.md` — the section "Suspend and resume", with the table of `resume`'s answers and a worked example; the account of the slot's size; what `check_loop()` covers
- `system/app/shell/shell_tests.cpp` — `check_loop()` gains a fifth step: a slot that suspends, is not called while its channel holds a message, is called after `resume`, and answers `BadState` to a second `resume`

**Symptom**
Not a defect: step 7 of 9 of `.agents/tasks/directory-capability/DESIGN.md` (decision 7, option C′). `loop::Action` was `Rearm` or `Close`. A connection that took a request whose reply has to wait on another party (a handler waiting on a disk, in steps 8 and 9) could answer neither: `Rearm` calls it again with the next message at once, with the first request unfinished, and `Close` ends the connection. Nothing in the tree answers `Suspend` yet; `LoopServer`, the root's servers and the examples answer `Rearm` and `Close` as before, and step 9 gives `LoopServer` the key it needs to resume.

**Cause**
Why the Loop has to learn this and a Waiter cannot do it alone. A wait is level-triggered: as long as the channel holds a message, an armed wait fires. A Waiter that wants the next message held back has only two ways. It can read the message and keep it, which is design option C (the core buffering a pending connection's later messages: at least 4,352 bytes for each connection, `wire::MAX_MESSAGE_BYTES` and `MAX_MESSAGE_HANDLES`), or it can leave the wait unarmed so that the message stays in the kernel's channel, where it costs the server nothing (64 messages per channel). Only the Loop arms (a Waiter "never arms its next wait itself", the rule that keeps at most one live wait per slot), so leaving the wait unarmed is a third answer, and arming it later is a Loop call.

Why the slot stays in use. Suspend is not a close: the handle is still open, the Waiter still holds its state for the slot, and the key was handed out and must stay valid, since the glue of step 9 keeps it. So the slot's handle is not Invalid ("in use" is exactly that, `add()` scans for Invalid), and its generation is not bumped. `add()` therefore never takes a suspended slot with no change to `add()` at all, and step 1 of `run()` needs one added condition, because the one case of a packet naming a suspended slot is a forged one (no wait is armed, so no packet of the Loop's exists).

Why `resume()` can answer `BadState` for a call inside the Waiter that suspends, with no extra state. `run()` marks the slot only after `on_signals()` returns, so while the Waiter runs the slot is in use, in the key's life, and not marked: the same state as an armed slot, and the same answer. The alternative, marking before the call and unmarking if the Waiter answers something else, would let a `resume()` from inside succeed and arm a wait for a slot whose Waiter is about to say it does not want one; one mutation below tries it, and the test of this case fails.

Why `resume()` answers `NotFound` and `BadState` as it does. A key is `generation << 32 | index`, so a stale key is one whose generation is not the slot's, and a key for a free slot is one whose handle is Invalid; both are "the key names nothing", which a caller may meet harmlessly after the slot closed (the Waiter answered `Close` while a `resume()` was on its way). `BadState` is a slot that is the key's but not suspended. The two answers keep a second `resume()` honest: it answers `BadState` while the slot is armed and `NotFound` once its Waiter has answered `Close`, and neither disturbs the slot's next life. The generation check is the one test of the old key of a reused slot: without it, a stale key would resume the slot's new life, which is exactly the use-after-reuse the generation exists to stop.

Why a failed arm at `resume()` is the same path as a failed re-arm. After a `Suspend` the Waiter has told the Loop it wants to be called again, as after a `Rearm`; if the kernel refuses the arm, nothing is armed and the slot would never fire, so the Loop closes the handle, bumps the generation, frees the slot, and tells the Waiter, which holds what it kept for the slot. `free_slot()` clears the slot before the Waiter is told, since `on_rearm_failed()` may `add()` and take the very slot; it is the code step 4 had inline, now called by `run()` for `Close`, by `fail_arm()` for the failed re-arm, and by `resume()` through `fail_arm()`. The call happens inside `resume()`, so the caller of `resume()` finds its own state for the slot released when it returns.

Why a flag in `Slot` and why it costs nothing. `Slot` is `{handle 4, signals 4, waiter 8, generation 4}` with 4 bytes of tail padding. A `Flags : u8` word at offset 20 fits in them; `-fdump-record-layouts-simple` on the compile command of `loop.cpp` (x86-64 user and the arm64 host alike) gives size 24 and data size 21, with the field offsets 0, 4, 8, 16 and 20. A free slot has no flags (`free_slot()` clears them), so `add()` needs to set nothing. It is a Flags word, not a `bool`, by the rule of `modern-cpp-development` for a state an object is in; the word is written only by the Loop, on its one thread, so the rule's condition on shared protection is trivially met.

**Change**
```cpp
// loop.hpp
enum class Action : u8 {
    Rearm,
    Close,
    Suspend, // new: the slot stays in use, nothing is armed, until resume(key)
};

class Slot final {
    enum class Flags : u8 { None = 0, Suspended = 1U << 0 };
    USTL_ENABLE_INNER_ENUM_BITMASK(Flags);
    ...
    u32 m_generation = 0;
    Flags m_flags = Flags::None; // in the 4 bytes of padding: still 24 bytes
};

[[nodiscard]]
auto resume(Key key) -> ours::Status;
```
`run()` before and after, for the tail of step 1 and for steps 3 and 4:
```cpp
// before
if (slot.m_handle == ours::Uandle::Invalid || slot.m_generation != key.generation()) { continue; }
auto const action = slot.m_waiter->on_signals(packet.signal.observed);
auto rearm = ours::Status::Ok;
if (action == Action::Rearm) { rearm = arm(key.index()); if (rearm == ours::Status::Ok) { continue; } }
// 4. close the handle, clear the slot, ++generation ... then, only for a failed re-arm:
if (action == Action::Rearm) { waiter.on_rearm_failed(rearm); }

// after
if (slot.m_handle == ours::Uandle::Invalid || slot.m_generation != key.generation() ||
    (slot.m_flags & Slot::Flags::Suspended) != Slot::Flags::None) { continue; }
auto const action = slot.m_waiter->on_signals(packet.signal.observed);
if (action == Action::Suspend) { slot.m_flags |= Slot::Flags::Suspended; continue; }
if (action == Action::Rearm) {
    auto const rearm = arm(key.index());
    if (rearm == ours::Status::Ok) { continue; }
    fail_arm(key.index(), rearm); // free_slot(), then on_rearm_failed(rearm)
    continue;
}
free_slot(key.index()); // Close
```
and the new function, by the state of the slot:
```cpp
auto Loop::resume(Key const key) -> ours::Status {
    if (key.index() >= m_slots.size()) { return ours::Status::NotFound; }
    auto &slot = m_slots[key.index()];
    if (slot.m_handle == ours::Uandle::Invalid || slot.m_generation != key.generation()) {
        return ours::Status::NotFound;
    }
    if ((slot.m_flags & Slot::Flags::Suspended) == Slot::Flags::None) { return ours::Status::BadState; }
    slot.m_flags &= ~Slot::Flags::Suspended;
    if (auto const status = arm(key.index()); status != ours::Status::Ok) {
        fail_arm(key.index(), status);
        return status;
    }
    return ours::Status::Ok;
}
```
Rejected: keeping a PEER_CLOSED-only wait armed while the slot is suspended, which would let the Loop see a vanished client at once but needs a second set of signals per slot, a Waiter that runs while a request is pending, and a `cancel(ticket)` on the handler (the design's alternatives, and its open question on cancellation). Rejected: a `Suspended` bool, by the Flags rule above. Rejected: sharing nothing between `resume()` and `run()`'s failed re-arm: two copies of "close, clear, bump, tell" are two places for the order to drift, and the order (clear before telling) is what lets `on_rearm_failed()` add.

**Reference**
Fuchsia, `sdk/lib/async-loop/loop.c` at `51f05104c13`: a wait there is one-shot and its owner decides whether to begin the next one. `async_loop_begin_wait()` (`:579`) is called again from the handler, and `AsyncBinding::MessageHandler()` re-arms through `CheckForTeardownAndBeginNextWait()` (`sdk/lib/fidl/cpp/wire/async_binding.cc:107`, `:179`). A handler that does not call it leaves the object unwaited on, which is Suspend, and calling it later is resume. The differences are this project's own: the Loop owns the table of slots, not the caller a wait object, so a slot can answer by its state (`BadState`, `NotFound`, and the generation that makes the old key of a reused slot harmless), where `async_loop_begin_wait()` trusts its caller not to begin a wait that is already pending. The design for the use, the handler node of steps 8 and 9, is the maintainer's of 2026-10-30.

**Verification**
- `prebuilt/ninja/ninja -C out/dc-step7 phony/system/lib/loop/tests`, then `out/dc-step7/host/obj/system/lib/loop/test/loop`: 44 passed, the 27 of before and 17 new ones. New: a Suspend leaves the slot in use with no wait armed and its handle open, and a message that arrives is not delivered (`RunLeavesASuspendedSlotInUseWithNothingArmed`); the key stays valid; `resume` arms the same wait (handle, port, key, signals); the waiting message is delivered behind the packets already queued; `resume` from another slot's Waiter; a peer closed while suspended is seen after `resume` as `PEER_CLOSED` alone and the Waiter answers `Close`; `resume` inside the Waiter that suspends is `BadState`; `BadState` for an armed slot, with its packet queued, and for a second `resume`; `NotFound` for a closed slot, for the next life of a free slot, for a slot never used, for an index equal to the table size and one far past it; `NotFound` for the old key of a reused slot, whose new suspended life is left alone; a refused arm at `resume` closes the handle, frees the slot, bumps the generation, tells the Waiter once before `resume` returns, and answers the cause; the Waiter may `add()` from that `on_rearm_failed()`; `add()` never takes a suspended slot (the table full, one slot suspended); the destructor closes a suspended slot's handle and calls and tells no Waiter; a packet that names a suspended slot is dropped; `Rearm` and `Close` Waiters are served as before beside a suspended slot. The `sizeof(Slot) == 24` assertion of the file compiles.
- Mutation, one at a time with the source put back after each (`loop.cpp`; the table is the tests that fail, or the shell line):

  | Breakage | Result |
  |---|---|
  | Suspend arms a wait | 14 tests fail (`RunLeavesASuspendedSlotInUseWithNothingArmed`, `AddNeverTakesASuspendedSlot`, `ResumeArmsTheWaitUnderTheSameKeyAndSignals`, ...) |
  | Suspend bumps the generation | 13 fail, among them `RunKeepsTheKeyOfASuspendedSlotValid` |
  | Suspend closes the handle | 15 fail, among them `Loop.ClosesTheHandleOfASuspendedSlotWhenItGoes` |
  | `resume` arms twice | 6 fail: `ResumeArmsTheWaitUnderTheSameKeyAndSignals`, `ResumeAnswersBadStateForASecondResumeThenNotFoundOnceClosed`, ... |
  | `resume` does not clear the flag | 6 fail: the second `resume` and the delivery tests |
  | `resume` accepts a stale key (no generation check) | 2 fail: `ResumeAnswersNotFoundForTheOldKeyOfAReusedSlot`, `ResumeLetsTheWaiterAddFromOnRearmFailed` |
  | `resume` accepts a free slot (no handle check) | 1 fails: `ResumeAnswersNotFoundForAKeyThatNamesNoSlotInUse` |
  | `resume` does not check the index | the test binary crashes (SIGSEGV) in `ResumeAnswersNotFoundForAKeyThatNamesNoSlotInUse` |
  | `resume` does not check the flag | 4 fail: `...BadStateForASlotThatIsArmed`, `...ForASecondResume...`, `ResumeInsideTheWaiterThatSuspendsAnswersBadState`, `RunDropsAPacketThatNamesASuspendedSlot` |
  | the slot is marked before `on_signals()` runs | 1 fails: `ResumeInsideTheWaiterThatSuspendsAnswersBadState` |
  | a refused arm does not bump the generation | 2 fail: `ResumeFreesTheSlotAndTellsTheWaiterWhenTheArmIsRefused`, `ResumeLetsTheWaiterAddFromOnRearmFailed` |
  | a refused arm does not free the slot | the same 2 |
  | a refused arm does not tell the Waiter | the same 2 |
  | step 1 delivers the packet of a suspended slot | 1 fails: `RunDropsAPacketThatNamesASuspendedSlot` |
  | `add()` takes a suspended slot | 6 fail, among them `AddNeverTakesASuspendedSlot` |
  | `resume` arms nothing | 8 fail |
  | Suspend arms a wait and step 1 delivers a packet of a suspended slot | the test binary does not end (the Waiters that answer `Suspend` over an unread `PEER_CLOSED` spin: the hang `Waiter`'s comment warns of) |
  | Suspend is a Rearm | the same |

  The last two rows are caught as hangs, which is the spin of an `Action::Rearm` over a peer that is gone; the rows above them are failures of named tests.
- Mutation of the shell check, each built into both images and booted on dbg (`boot_check.py --only dbg --no-tests`), the source put back after:

  | Breakage | `shell: loop` line |
  |---|---|
  | Suspend arms a wait | `checks passed`: not caught. Step 1's drop of a packet for a suspended slot hides it: the wait fires, its packet is dropped, and nothing reaches the Waiter. Only the fake's count of armed waits sees it, which is what the host test does |
  | Suspend arms a wait and step 1 delivers the packet | `served "abaabcee" (expected "abaabce")`, `checks FAILED` |
  | Suspend is a Rearm | the same, and `resume(e) = BadState (expected Ok)` |
  | `resume` arms nothing | `served "abaabce" (expected "abaabcee")`, `checks FAILED` |
  | `resume` does not check the flag | `resume(e again) = Ok (expected BadState)` and `served "abaabceee"`, `checks FAILED` |

  So each of the check's three assertions fails when its code breaks. The first needs both of the Loop's defences broken to show, which is a finding about the code, not about the check: the two defences cancel each other for anyone who watches only what the Waiter is called for.
- Boot, `python3 .agents/skills/verify/scripts/boot_check.py --out out/dc-step7` on the final tree, against the baseline of the parent commit `864dc78e` (the shell's counts do not rise: `check_loop()` is one check with more assertions):

  | | Before | After |
  |---|---|---|
  | dbg | PASS, 46 passed, 5 skipped, 0 failed, 0 error lines | the same |
  | rel | PASS, 44 passed, 8 skipped, 0 failed, 7 of 7 required, 0 error lines | the same |
  | host tests | PASS, 164 passed, 0 failed | the same; `loop` has 44 tests (was 27) |

  `shell: loop checks passed` is in `dbg.log` and in `rel.log`.
- Repeated rel boots on the final build: see `.agents/tasks/directory-capability-step7/RESULT.md`.
- `prebuilt/clang/llvm-23/bin/clang-tidy -p out/dc-step7 --header-filter='.*/system/lib/loop/.*'` on `loop.cpp` and `loop_test.cpp`: no diagnostic. On `shell_tests.cpp`: three, none on a line of this change (`H`, `sink`, `dump`). `format_cpp.py --check` on the four C++ files: clean.
- Not measured: the size of `loop.cpp`'s object code, and the cost of the extra flag test in step 1 of `run()` (one load and a mask, on a path that already loads the slot).

**Deliberately not done**
- `LoopServer` and every other Waiter: none answers `Suspend`. `LoopServer::watch()` still drops the key `add()` returns; step 9 keeps it in the `Binding`.
- A `complete()` that calls `resume()`: steps 8 and 9. One rule for them, found here: a `complete()` called from inside the `on_signals()` that suspends gets `BadState` from its `resume()` and the connection would stay suspended for good, so a handler that finishes at once answers `Done`, never `Pending`.
- A timeout for a slot that is never resumed, and a cancellation: the Loop has no timers, and the design leaves both open.
- The developer's guide: a later commit syncs it; the passages that are now incomplete are in `RESULT.md`.

**How it was found**
None. A new feature, from the design.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `resume(key)` called from inside the `on_signals()` that is about to answer `Suspend` is `BadState`. Which field of the slot makes it so, when is that field written, and what would break if it were written before `on_signals()` was called?
2. A Waiter answers `Suspend` over a message it has not read, and the message is still there at `resume()`. Why is that not the spin that `Rearm` over an unread message is, and why is the packet queued at once?
3. Step 1 of `run()` drops a packet that names a suspended slot, though none should exist. Why does that make "Suspend arms a wait" invisible to the shell's check, and which test catches it on the host?
