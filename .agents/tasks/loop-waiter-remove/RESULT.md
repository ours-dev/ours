# loop-waiter-remove: a slot waits on a Waiter, and the function-pointer form is gone

**Date**: 2026-10-29

## Delivered
Step 6 of 6 of `.agents/tasks/loop-waiter/PLAN.md`, one commit on the branch `lw-remove` (its hash is in the report that handed this step back, since a commit cannot name itself), `refactor(loop): a slot waits on a Waiter, and the function-pointer form is gone`, on top of `75cb036b`:

- `Slot` holds `Waiter *m_waiter`; `Callback`, `add(handle, signals, Callback, void *)` and `call_waiter()` are deleted; `add(handle, signals, Waiter &)` does the work; `run()` calls `slot.m_waiter->on_signals(observed)`; `add()`'s failure path and step 4 of `run()` clear `m_waiter`. The dispatch is otherwise as it was. `Slot` is 24 bytes where it was 32.
- Every comment of `loop.hpp`, `loop.cpp` and `README.md` that spoke of the older form, a later step, the callback or the context names the Waiter; the "later `ioservice::Outgoing`'s" sentence is corrected in both files, and its repair item (`docs/repairs/261029/loop.md`, the only one in the file) is deleted.
- The loop tests and the shell's `check_loop()` use a `Waiter`; `static_assert(sizeof(loop::Slot) == 24)` is in `loop_test.cpp`. The bench defines form A's `Callback` itself.
- `docs/perf/261028/loop-dispatch.md` is Done, with a Decision; `docs/changes/261029/loop.md` has the account.

## Verification
- `prebuilt/ninja/ninja -C out/bringup.x86` builds everything and `phony/benches/benches` builds. Host tests, run from `out/bringup.x86`: `host/obj/system/lib/loop/test/loop` 21 passed, `host/obj/system/lib/ioservice/test/ioservice` 42, `outgoing` 9, `loop_server` 13, `host/obj/system/service/init/test/autorun` 5, all with 0 failed. `loop-dispatch-bench check` prints `check ok`.
- `grep -rn "loop::Callback\|Callback\b" system examples kernel benches --include='*.cpp' --include='*.hpp'`: besides three unrelated names in the kernel (`ccrt`'s `Callback` union, `gktl::Command::Callback`, a word in `kernel/include/ours/dpc.hpp`), only the bench's local alias and the comments about it.
- clang-tidy against `out/bringup.x86`: `loop.cpp`, `loop_test.cpp`, `loop_dispatch_bench.cpp` exit 0 with no diagnostic; `shell_tests.cpp` exit 0 with 3 pre-existing `readability-identifier-naming` diagnostics at lines 5030, 7257 and 7264, none on a changed line. `format_cpp.py --check` and `lint_docs.py --changed`: clean.
- `boot_check.py --out out/lw-remove` with the final tree: build ok 6.6 s; dbg PASS (shell finished at 5.5 s, 44 passed, 4 skipped, 0 failed, 0 error lines); rel PASS (4.7 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines, 0 error lines); host tests PASS (148 passed, 0 failed). `rel.log` holds `routing_checks: passed`, `init: echo exited with 0`, `pc-ps2: key a` and `pc-ps2: key A`; both logs hold `shell: loop checks passed`.
- Deliberate breakages of `Loop::run()`, the source put back after each (`cmp` against a saved copy): step 2 passing `Signals::None`: 8 of 21 loop tests fail, and the dbg image logs `shell: loop served "ab" (expected "abaa")` and `shell: loop checks FAILED` (`boot_check` dbg FAIL, 1 FAILED line). Step 3 skipping the re-arm: 6 of 21 fail, and the same two lines on the target. After restoring: 21 passed.
- The account (sizes of `Slot`, `init` and `echo_server` before the task, before this step and after, and the objects' `sizeof`): in the change record, `docs/changes/261029/loop.md`, last section. In short: `Slot` 32 to 24; `init` `.data` + `.bss` 106,592 / 108,064 / 106,656, `.text` 108,494 / 109,278 / 108,878, stripped file 154,112 / 178,176 / 177,776; `echo_server` `.bss` 22,960 / 23,024 / 22,960, `.text` 25,342 / 25,694 / 25,294, stripped file 30,192 / 30,640 / 30,240.
- Not measured: the time of the real `Loop::run()` before and after; the shell's sections; the BootFS image's size; anything on x86-64 hardware.

### How the old tests map
The loop test had 29 tests before this step: 18 of the function form, 11 added by step 1 for the Waiter form. It has 21. Every case has a counterpart but those listed under "no counterpart".

| Old test | New test |
|---|---|
| `AddHandsOutTheLowestFreeSlotAtItsGeneration` | the same name |
| `AddArmsOneWaitUnderTheSlotsKey` | the same name, with the check of step 1's `AddWithAWaiterTakesTheLowestFreeSlot...` that adding calls nothing |
| `AddRefusesAnInvalidHandleNoSignalsOrNoCallback` | `AddRefusesAnInvalidHandleOrNoSignals`, with step 1's check that the first slot is still the lowest free one |
| `AddAnswersNoResourceWhenEverySlotIsInUse` | the same name, with step 1's check that no wait was armed for the refused handle |
| `AddLeavesTheSlotFreeAndTheHandleWithTheCallerWhenTheWaitFails` | the same name |
| `ClosesItsHandlesAndThenItsPortWhenItGoes` (`Loop`) | the same name |
| `RunReturnsTimeOutOnceThePortIsEmpty`, `RunWaitsWithTheDeadlineItIsGiven`, `ReturnsTheAnswerOfAPortWaitThatFails` (`LoopRun`) | the same names |
| `RunCallsTheCallbackOfTheSlotWhoseWaitFired` | `RunCallsTheWaiterOfTheSlotWhoseWaitFired` |
| `RunRearmsUnderTheSameKeyAndSignals`, `RunServesABusyClientInTurns`, `RunClosesTheSlotWhenTheRearmFails`, `RunDropsPacketsThatNameNoSlotInUse`, `DropsAnIndexOnePastTheTableWhateverLiesThere` (`LoopRun`), `RunDropsAPacketForASlotFreedInItsNextLife` | the same names |
| `RunClosesTheSlotWhenItsCallbackAnswersClose` | `RunClosesTheSlotWhenItsWaiterAnswersClose`, with step 1's check that nothing stays armed for the handle |
| `RunKeepsTheSlotInUseWhileItsCallbackAdds` | `RunKeepsTheSlotInUseWhileItsWaiterAdds` |
| step 1: `AddWithAWaiterTakesTheLowestFreeSlotAndArmsOneWaitUnderItsKey` | folded into `AddHandsOutTheLowestFreeSlotAtItsGeneration` and `AddArmsOneWaitUnderTheSlotsKey` |
| step 1: `AddWithAWaiterRefusesAnInvalidHandleOrNoSignals` | folded into `AddRefusesAnInvalidHandleOrNoSignals` |
| step 1: `AddWithAWaiterAnswersNoResourceWhenEverySlotIsInUse` | folded into `AddAnswersNoResourceWhenEverySlotIsInUse` |
| step 1: `AddWithAWaiterLeavesTheSlotFreeAndTheHandleWithTheCallerWhenTheWaitFails` | folded into `AddLeavesTheSlotFreeAndTheHandleWithTheCallerWhenTheWaitFails` |
| step 1: `RunCallsTheWaiterOfTheSlotWhoseWaitFiredWithTheObservedSignals` | folded into `RunCallsTheWaiterOfTheSlotWhoseWaitFired` |
| step 1: `RunTellsAWaiterWhatTheObjectAsserted` | folded into `RunClosesTheSlotWhenItsWaiterAnswersClose` (the same close_peer scenario and the same observed `PeerClosed`) |
| step 1: `RunRearmsAWaiterUnderTheSameKeyAndSignals` | folded into `RunRearmsUnderTheSameKeyAndSignals` |
| step 1: `RunClosesTheSlotWhenItsWaiterAnswersClose` (a Waiter that answers Close with a message unread) | `RunClosesTheSlotWhenItsWaiterAnswersCloseWithAMessageUnread`; renamed because the plain name went to the peer-closed scenario above |
| step 1: `RunCallsAWaiterAndAFunctionEachForItsOwnSlot` | `RunCallsEachWaiterForItsOwnSlotOnly`, with two Waiters |
| step 1: `RunClosesOnlyTheSlotThatAnswersCloseWhateverFormTheOtherIs` | `RunClosesOnlyTheSlotWhoseWaiterAnswersClose` |
| step 1: `RunKeepsTheSlotInUseWhileAWaiterAdds` | folded into `RunKeepsTheSlotInUseWhileItsWaiterAdds` (the same scenario) |

No counterpart, because they were about the function pointer itself:
- the null callback refused with `InvArgs` (the third check of `AddRefusesAnInvalidHandleNoSignalsOrNoCallback`): a reference cannot be null, and the compiler refuses what the function checked;
- the context passed through: the old tests handed `&client` as the `void *` and cast it back in `serve()`. The Waiter is the object, so there is no second pointer to pass; the case "the right object is called for the right slot" is the same one `RunCallsTheWaiterOfTheSlotWhoseWaitFired` and `RunCallsEachWaiterForItsOwnSlotOnly` check;
- the mix of a function and a Waiter on one Loop (step 1's `AddWithAWaiter...NoResource` filled the table with both, `RunCallsAWaiterAndAFunction...` and the closing test above ran both): with one form there is nothing to mix, and the two tests that ran the mix now run two Waiters.

## Deviations from the design
- The brief asked for the account at 75cb036b and after. A third tree was measured as well, the one before the task (75099335, in a scratch worktree removed after), so that the net of the whole task is a number and not an inference from the change records. The only change to `system/` and `examples/` between it and 75cb036b is the loop-waiter work (`git diff --stat 75099335 75cb036b -- system examples build vendor`: 15 files, all of them steps 1 to 5).
- The brief called the recommendation of C "the report's own". The report as written left the choice open ("The report does not choose between them"); the recommendation of C is in the Decisions table of `.agents/tasks/loop-waiter/PLAN.md`. The Decision in the report says the second, plainly, and does not say the first.
- Two tests of step 1 are renamed and the others folded, as the table says; the count went from 29 to 21, not to the 18 the function form had, because three cases of step 1 are cases of their own.

## Found along the way
- `docs/guide/` cites lines of the two files this step changes, and they have moved; the guide is another session's and was not touched. Where the same text is still there, `loop.hpp` old to new: the namespace comment `13-19` is `13-19`; `Waiter`'s rules `50` is `49`, `51-54` is `50-53`, `55-61` is `54-60` and `63-69` is `62-68`; the constructor's `172-173` is `156-157`, `175-179` is `159-163` and `181` is `165`; `~Loop` `189` is `173` and the deleted copies `193-196` are `177-180`; `add()`'s success and failure paragraphs `202-207` are `187-192`, its `NoResource` `210` is `194`, and the sentence that a callback may call `add()` (`214-215`) is `198-199`, now about a Waiter's `on_signals()`; `run()`'s "never returns Ok" `258-259` is `222-223` and I4 `291-294` is `254-257`. In `loop.cpp` the arm's `object_wait_async` was `85` and is `67`. The guide's prose also says "the slot's callback" or "the callback" in `01-concepts.md`, `03-protocols.md`, `04-tree-servers.md`, `05-testing-and-debugging.md` and `06-checklist.md`; a recheck would say "Waiter" and "`on_signals()`" there. Not done, by the brief.
- The word "callback" in the plain sense is still in comments of `ioservice` (`outgoing.hpp:42,54`, `loop_server.cpp:82,86,92`, `server_test.cpp:125`) and `component/route.hpp:330`: a function the Loop calls. Left, as steps 2 and 3 left it.
- The init sections of the whole task show where the memory went: all of the file growth (22,432 bytes of 23,664) is the five resource servers' storages, `docs/repairs/261029/init.md` item 1, which is still open. This step could not touch it.
- `pools.hpp` counts three Loop slots per component (the outgoing directory's client end, the process, and the `/svc` server end) and `Realm` registers two Waiters per `Instance`; the third is served as a connection of `BootServer`, I did not trace whether the table counts it twice. The change record leaves it as the "8 slots with nothing to pay back" of the arithmetic. Not recorded as a repair: the table is large enough either way.

## Not done
- A timing of the real `Loop::run()`, before and after. A proposal for the user, not taken: a host bench that links `system/lib/loop/test/fake_kernel.cpp`, fills the fake port with N packets across six Waiters of six classes, and times `run(0)` per packet at 75cb036b and at this commit, interleaved, 30 rounds three times as the report did. What it could show: the step removes one indirect branch that the predictor always got right (`call_waiter()` was `ldr, ldr, br`, three instructions) and two loads, so the change is expected to be below the host's noise, and the fake's own queue (a standard container) costs more per packet than the Loop does, so the number would bound the difference and not measure it. It is worth taking only if the user wants a before-and-after for the report; the honest figure for the target is on x86-64 hardware, which no bench here can give.
- The line numbers and the word "callback" in `docs/guide/`, as above.
- The five resource storages in `.data`, `docs/repairs/261029/init.md` item 1.
- `.agents/tasks/loop-waiter/PLAN.md` still says Proposed: the parent plan's Status is for the user.
