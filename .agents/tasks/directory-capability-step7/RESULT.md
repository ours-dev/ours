# directory-capability-step7: a Waiter can leave its slot unarmed, and the Loop can resume it

**Date**: 2026-11-03

## Delivered
Branch `dc-step7`, worktree `.worktrees/dc-step7`, one commit on `864dc78e`: `feat(loop): a Waiter can leave its slot unarmed and the Loop can resume it`. It is step 7 of the Staging table of `.agents/tasks/directory-capability/DESIGN.md` (decision 7, option C′, the Loop's half):

- **`loop::Action::Suspend`.** After `on_signals()` returns it, `run()` marks the slot suspended and does nothing else: it arms no wait, closes no handle, bumps no generation. The slot stays in use (its handle is not Invalid), so `add()` never takes it, and its key stays valid. `add()` needed no change.
- **`Loop::resume(Key) -> ours::Status`**, by the state of the slot, as the design's table says: the suspended slot is armed and the answer is Ok; BadState for a slot whose Waiter is running, an armed slot (its packet queued included) and a second `resume`; NotFound for a free slot, the old key of a reused slot and an index past the table. None of these changes the slot. A `resume` whose arm the kernel refuses closes the handle, frees the slot, bumps its generation, calls `on_rearm_failed(cause)` once before `resume` returns, and answers the cause. That path is shared with the failed re-arm of `run()`: the old inline step 4 is now the private `free_slot()` (close, clear, bump, return the Waiter) and `fail_arm()` (`free_slot()` then the call), which `run()` and `resume()` both use.
- **Inside the Waiter that suspends, `resume` is BadState** because of the order of events alone: `run()` sets the flag after `on_signals()` returns, so while the Waiter runs the slot is in use, in the key's life, and not marked, which is the state of an armed slot. No extra field. A mutation that marks the slot before the call makes the test of this case fail.
- **The flag**: `Slot::Flags`, a private nested `enum class : u8` with `Suspended`, and `Slot::m_flags` after the generation. `sizeof(Slot)` is **24** (the existing `static_assert` in `loop_test.cpp` still holds, and its comment now counts the flag); `-fdump-record-layouts-simple` on the compile commands of both the x86-64 user build and the host: size 24 bytes, data size 21, field offsets 0, 4, 8, 16, 20. A free slot has no flags (`free_slot()` clears them).
- **Step 1 of `run()`** also drops a packet that names a suspended slot. Tested with a forged packet through the fake (`RunDropsAPacketThatNamesASuspendedSlot`).
- The destructor closes a suspended slot's handle like any other; it needed no change.
- **Documents**: the spec and comments of `loop.hpp` (the namespace, `Action`, `Waiter`, `Slot`, the destructor, `add()`, `resume()`, `run()` steps 1 to 3, I1, I3, I4, the live-wait invariant, the paragraph on the key, and the constructor's `PORT_MAX_OBSERVERS` sentence now "at most one live wait"), `system/lib/loop/README.md` (a section "Suspend and resume": the table of `resume`'s answers and a worked example of a connection that takes a request, suspends and is resumed), `docs/changes/261103/loop.md`. The developer's guide is not touched (below).
- **`LoopServer` and every other Waiter are untouched.** None answers `Suspend`; the 30 tests of `loop_server` and the other host suites pass as before.

## Verification
- Host: `out/dc-step7/host/obj/system/lib/loop/test/loop`: 44 tests pass (27 before, 17 new, listed in the change record).
- **Mutation.** 17 breakages of `loop.cpp`, one at a time, source put back after each (byte-identical to a saved copy, checked). Every one fails a named test, crashes the binary or hangs it:

  | Breakage | Result |
  |---|---|
  | Suspend arms a wait / bumps the generation / closes the handle | 14 / 13 / 15 tests fail |
  | `resume` arms twice | 6 fail |
  | `resume` does not clear the flag | 6 fail |
  | a stale key accepted (no generation check) | 2 fail (`ResumeAnswersNotFoundForTheOldKeyOfAReusedSlot`, ...) |
  | a free slot accepted (no handle check) | 1 fails (`ResumeAnswersNotFoundForAKeyThatNamesNoSlotInUse`) |
  | the index not checked | SIGSEGV in the same test |
  | the flag not checked (a second `resume` and a `resume` on an armed slot succeed) | 4 fail |
  | the flag set before `on_signals()` | 1 fails (`ResumeInsideTheWaiterThatSuspendsAnswersBadState`) |
  | a refused arm: generation not bumped / slot not freed / Waiter not told | 2 fail each |
  | the packet for a suspended slot delivered | 1 fails (`RunDropsAPacketThatNamesASuspendedSlot`) |
  | `add()` takes a suspended slot | 6 fail (`AddNeverTakesASuspendedSlot`, ...) |
  | `resume` arms nothing | 8 fail |
  | Suspend arms a wait and the packet is delivered / Suspend is a Rearm | the test binary does not end: the spin of `Rearm` over a closed peer, as `Waiter`'s comment warns |

- **Mutation of the shell check** (both images are built from the mutated Loop, dbg is booted):

  | Breakage | `shell: loop` |
  |---|---|
  | `resume` arms nothing (assertion 2: called after `resume`) | `served "abaabce" (expected "abaabcee")`, `checks FAILED` |
  | `resume` does not check the flag (assertion 3: `BadState` to a second `resume`) | `resume(e again) = Ok (expected BadState)`, `checks FAILED` |
  | Suspend is a Rearm (assertion 1: not called while its channel holds a message) | `served "abaabcee" (expected "abaabce")`, `checks FAILED` |
  | Suspend arms a wait and step 1 delivers the packet | the same |
  | **Suspend arms a wait, alone** | **`checks passed`: not caught on the target** |

  The last row is a finding about the Loop, not a gap to close: step 1's defensive drop of a packet for a suspended slot swallows the packet of the wait that Suspend wrongly armed, so nothing reaches the Waiter and nothing on the target can see the stray wait except the port's observer count. The fake's count of armed waits sees it, and eight host tests fail on it.
- **Boot**, `boot_check.py --out out/dc-step7` on the final tree, against the baseline of `864dc78e` (the counts have risen since the last known ones; `check_loop()` is one check with more assertions, so they do not rise now):

  | | Baseline | Now |
  |---|---|---|
  | dbg | PASS, 46 passed, 5 skipped, 0 failed, 0 error lines | the same |
  | rel | PASS, 44 passed, 8 skipped, 0 failed, 7 of 7 required, 0 error lines | the same |
  | host tests | PASS, 164 passed (executables), 0 failed | the same |

  `shell: loop checks passed` is in `dbg.log` and `rel.log`; the mutated builds above print `checks FAILED`.
- **Repeated rel boots** on the build of the final code (`--no-build --no-tests --only rel`; the only change after them is a sentence of a header comment, and the full check above was run again on the tree as committed):
  - 4 CPUs (the default): 10 of 10 pass.
  - `SMP=2`: **6 of 10 pass; the 4 that fail are one known failure, not this change's.** Each prints `routing_checks: Gpt.GetInfo = PeerClosed (expected Ok) FAILED` after `virtio-blk: no virtio block device on bus 0` and `routing_checks: no block device, skipped`, and `2 FAILED line(s) ... 6/7 required`; every other check passes and each of the 4 logs holds `shell: loop checks passed`. That is item 1 of `docs/repairs/261103/routing-checks.md` ("`check_gpt` can fail where the block check skips, when there is no disk", filed as not diagnosed, one boot in seven). **The same boot on the unmodified parent `864dc78e`** (a second worktree built from it, same machine, same command, 20 boots with `SMP=2`) **passes 7 of 20 with the identical signature in all 13 failures**, so `SMP=2` makes the race far likelier than the item says and this change neither causes nor moves it. Not debugged under lldb, since it is independent of the change; from reading the code, `ours_block::Client::receive()` (`system/lib/ours.block/include/ours_block/client.hpp:146`) returns the status of its `channel_write` as it is, and a write on a channel whose peer the root has already closed answers `PeerClosed` without the epitaph queued behind it ever being read, whereas a write that lands before the close is followed by a read of the epitaph and an answer of `NotFound`, which `check_gpt()` turns into a skip. That reading is consistent with the log and with the rate rising when the timing changes, but I did not confirm it in a debugger. The item is worth a line about `SMP=2` and this reading.
  - The failing runs' logs are kept in the scratchpad of this session only; the worktree of the baseline was removed after the runs.
- clang-tidy (`prebuilt/clang/llvm-23`, `-p out/dc-step7`, `--header-filter='.*/system/lib/loop/.*'`) on `loop.cpp` and `loop_test.cpp`: no diagnostic; on `shell_tests.cpp`: three, none on a line of this change. `format_cpp.py --check` on the four C++ files: clean. `lint_docs.py --changed` on the change record: no violation.

## Deviations from the design
- **`resume` is `[[nodiscard]]`** and **the flag is a `Slot::Flags` word, not a `bool`**, by the repository's rule for a state an object is in. Neither changes the interface the design names.
- **Two private helpers** (`free_slot()`, `fail_arm()`), where the design says "follow how the existing re-arm failure path does it and share the code". The design did not name them.
- **Nothing else.** The design's table for `resume` and its invariants are implemented as written.

## Found along the way
- **The defences hide each other on the target** (the last row of the shell mutation table). Not a bug, and not worth removing the drop for; the fake-kernel test is the one that sees a stray wait. Recorded in the change record.
- **For step 9, a rule the design does not state.** `resume()` from inside the `on_signals()` of the slot being resumed is BadState, by design. If the glue's `LoopServer::complete()` is ever called while that connection's own Binding is running (a handler that answers `Pending` and completes in the same call stack, for example from `open()` itself), the `resume()` it ends with is BadState, the Binding then answers `Suspend`, and the connection stays suspended for good with its reply written and nobody left to resume it. The design's rule "a synchronous handler always returns `Done`" covers the plain case; the glue should still treat BadState from `resume()` in `complete()` as "the Binding is running": record that the completion already happened, and have the Binding answer `Rearm` instead of `Suspend`. Step 9 is where to decide it.
- `docs/repairs/261004/loop.md` item 1 ("Loop::run() frees a slot whose re-arm failed without telling the slot's callback") was fixed on 2026-10-29 (`on_rearm_failed()`, `docs/changes/261029/loop.md`) and is still listed. Step 7 does not affect it (it adds a second caller of the fixed code), so I left it; deleting it is a one-line cleanup. No repair item of `docs/repairs/*/loop.md` is closed or changed by this step; no new one.

## Not done
- `LoopServer::watch()` still drops the key that `add()` returns; the `Binding` keeps it in step 9.
- Steps 8 and 9 (the handler node, `Server::complete`, `LoopServer::complete`).
- The developer's guide and the architecture notes, as the brief says. These passages are now incomplete or have moved cites (line numbers are `864dc78e`'s):

  | Passage | Says | Now |
  |---|---|---|
  | `docs/guide/01-concepts.md:127` | a slot holds a handle, signals, the waiter and a generation; "a slot is free while its handle is `Invalid`" | and a flag; a slot whose Waiter answered `Suspend` is in use with nothing armed |
  | `01-concepts.md:146` | `on_signals` answers `Rearm` or `Close`; `on_rearm_failed` is called "in one case only": the waiter answered `Rearm` and the Loop could not arm again | a third answer, `Suspend`; the call is also made by `resume()` when the kernel refuses its arm, before `resume()` returns |
  | `01-concepts.md:152-156` (the steps of `run`) | step 1 drops a packet for a free slot or a stale generation; step 3 is `Rearm` or `Close` | step 1 also drops a packet that names a suspended slot; step 3 gains `Suspend` (mark the slot, arm nothing); `resume()` arms later |
  | `01-concepts.md:159-166` (the four consequences) | one message per call; read before `Rearm`; who closes the handle | `Suspend` leaves the message in the channel and does not spin; the Loop also closes the handle when a `resume()` cannot arm; a fifth consequence for `Suspend` and `resume` is missing |
  | `01-concepts.md:175` and `06-checklist.md:301`, `:305`, and the comment in echo's source they quote | "Each slot in use holds one live wait" | at most one; none while suspended |
  | `docs/guide/02-first-server.md:355`, `:357`, `:640` | `on_signals()` answers `Rearm` or `Close`; every `return` is one of them | and `Suspend`, for a server that waits on something before the next message |
  | `docs/guide/04-tree-servers.md:210`, `:216-225` | the `Binding`'s answer is `Rearm` or `Close`; the table of outcomes | unchanged until step 9, where `LoopServer` answers `Suspend` for a pending outcome |
  | `docs/guide/06-checklist.md:26-27`, `:44`, `:155`, W5 (`:163-167`), W6 (`:169-173`), W9 (`:195-199`), R1 (`:446`) | "a callback ... answers Close or Rearm"; "Rearm only after reading"; W9 frees the record on Close or a failed re-arm | `Suspend` is a third answer, and a Waiter may `resume()` another slot; W6's spin is a `Rearm` thing only; W9 also covers a `resume()` that cannot arm, and a Waiter that never resumes keeps its slot for good |
  | cites of `loop.hpp` lines: `01-concepts.md:129`, `:202`, `:203`; `04-tree-servers.md:439`; `06-checklist.md:81`, `:94`, `:109`, `:110`, `:147`, `:161`, `:167`, `:173`, `:281`, `:302`, `:316`, `:317`, `:321`, `:327`, `:406` | `Waiter` at `:106-117`, `Key` at `:119`, `Loop::run` at `:238`, and so on | the header grew by about 100 lines: `Action` is at `:40`, `Waiter` at `:146`, `Key` at `:164`, `Slot` at `:202`, `Loop` at `:247`, `add()` at `:297`, `resume()` at `:337`, `run()` at `:426` (the spec comment above it) |
  | `docs/architecture/component-framework/glossary.md:49` | "**Loop**: ... a port and a set of callbacks" | already wrong (`docs/repairs/261029/loop.md`); a port and a table of slots, each waiting on a `Waiter` that may suspend its slot |
