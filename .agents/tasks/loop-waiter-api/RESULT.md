# loop-waiter-api: a slot can wait on a Waiter, beside the callback function

**Date**: 2026-10-29

## Delivered
Step 1 of 6 of `.agents/tasks/loop-waiter/PLAN.md`, one commit on the branch `lw-api`, `feat(loop): a slot can wait on a Waiter, an abstract class` (its hash is in the report that handed this step back, since a commit cannot name itself), on top of `ec327f51`:

- `loop::Waiter` in `system/lib/loop/include/loop/loop.hpp`: pure virtual `on_signals(observed) -> Action`, a protected non-virtual destructor, a `constexpr` constructor, protected defaulted copy. Its comment carries the rules `Callback`'s comment had, the lifetime rule and the reason for the destructor. `Callback`'s comment no longer says "never a virtual call"; it points at `Waiter` for the rules and says what the detour is.
- `Loop::add(handle, signals, Waiter &)`, documented in terms of the old `add()`, with the one `InvArgs` cause fewer. In `loop.cpp` it is `add(handle, signals, &call_waiter, &waiter)`, with `call_waiter()` in the anonymous namespace. `Slot` and `run()` are not touched.
- Eleven new host tests in `loop_test.cpp` (`WaiterClient`, `LoopTest::add_waiter()`); `README.md`; `docs/changes/261029/loop.md`.

## Verification
- `out/bringup.x86/host/obj/system/lib/loop/test/loop`: 29 passed (18 before, 11 new).
- Three deliberate breakages of `call_waiter()`, each caught and the source put back: always `Close` (2 tests fail), `Signals::None` as `observed` (4 fail), always `Rearm` (the run spins and has to be killed, the spin `Waiter`'s comment warns of).
- `prebuilt/ninja/ninja -C out/bringup.x86` (1043 steps) and `phony/benches/benches` build; `loop-dispatch-bench check` prints `check ok`.
- clang-tidy against `out/bringup.x86` on `loop.cpp` and `loop_test.cpp`, header filter on `system/lib/loop`: no diagnostic. `format_cpp.py --check`: clean. `lint_docs.py --changed` on each document: no violations.
- `boot_check.py --out out/lw-api` in `.worktrees/lw-api` with the final tree: build ok in 7.6 s; dbg PASS (44 passed, 4 skipped, 0 failed, 0 error lines); rel PASS (41 passed, 8 skipped, 0 failed, 6/6 required lines, 0 error lines, `routing_checks: passed`); host tests PASS (148 passed, 0 failed). Both logs hold `shell: loop checks passed`.
- Not measured: the cost of the detour by itself, and anything on x86-64 hardware.

## Deviations from the design
- `Waiter`'s copy operations are protected and defaulted, not deleted: the brief said to delete them if the sibling abstract classes do, and `Delegate` and `LauncherOps` default them.
- The constructor is `constexpr`, which the brief did not name: `LauncherOps` has it, and the `constinit` pools of steps 2 and 3 will need it.
- The rules of `on_signals()` are written once, in `Waiter`, and `Callback`'s comment points at them, where the brief read as keeping them in both. `Callback` goes in step 6, so they are already where they stay.
- `boot_check.py` was run with `--out out/lw-api`, not the default, so that its logs in `$TMPDIR` do not meet another session's (`docs/repairs/261029/ours-verify.md`).

## Found along the way
- The tests of the function form and of the Waiter form alike turn a callback that answers `Rearm` with its message unread into a hang, not a failing assertion: `run(0)` never finds the port empty. That is the behaviour the spec describes, so it is not a defect of the Loop, but a mutation of `run()` shows up as a test binary that does not end. No repair item; a test timeout would be the tool's job.

## Not done
- Steps 2 to 6 of `.agents/tasks/loop-waiter/PLAN.md`: each waits for the user's go-ahead.
- The cost of the detour is not measured on its own; it goes in step 6.
