# loop-waiter-remove: a slot waits on a Waiter, and the function-pointer form goes

**Date**: 2026-10-29
**Status**: Implemented

## Request
Step 6 of 6, the last, of `.agents/tasks/loop-waiter/PLAN.md`. Steps 1 to 5 added `loop::Waiter` and `Loop::add(handle, signals, Waiter &)` beside the function-pointer form and moved the six registration sites (`LoopServer::Binding`, the root's `ResourceConnection` and `LauncherConnection`, echo's `Client`, `Realm`'s two waiters). Nothing outside the Loop's own tests and the shell's `check_loop()` registers with `add(handle, signals, Callback, void *)` any more. This step removes that form and the detour that carried the Waiter over it, moves the tests and `check_loop()` to a `Waiter`, closes `docs/perf/261028/loop-dispatch.md` with the decision, and measures the whole task's memory bill. The scope ends at the Loop, its tests, the shell's `check_loop()`, the bench's alias and the documents: the dispatch rules (the four steps of `run()`, I1 to I4, the generation check), the registration sites and `docs/guide/` do not change.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Where the `sizeof(Slot)` assertion goes | (a) the header; (b) the test file | (b): the library should not assert the layout of the targets it is built for, and the test is built on the host, where pointers are 8 bytes as on the target | Not asked | (b), by the brief |
| The tests of the function form | (a) keep both forms' tests; (b) move each to a `Waiter` and fold the 11 of step 1 in; (c) drop them | (b): every case is kept except those that were about the function pointer itself | Not asked | (b), by the brief |
| What the bench does with form A | (a) leave it on `loop::Callback`, which is gone; (b) define the alias locally with a comment that says it is the Loop's former form | (b): form A is the baseline of the report, and what it measures must not change | Not asked | (b), by the brief |
| The tree the memory bill starts from | (a) 75cb036b, the tree before this step; (b) also the tree before the task, 75099335 | (b): (a) shows only this step's saving, and the user asked for the net of the whole change; the only change to `system/` and `examples/` between 75099335 and 75cb036b is the loop-waiter work, so a build of it is a fair start | Not asked | (b), by this step (a scratch worktree, removed after) |

## Steps
1. In a worktree at 75cb036b, build `out/bringup.x86` and keep `llvm-size -A`, the stripped size and `llvm-readelf -l` of `init` and `echo_server`, and the layout of `Slot`. Build a scratch worktree at 75099335 and keep the same; remove it.
2. `loop.hpp`, `loop.cpp`: `Slot` holds `Waiter *m_waiter`; `Callback`, the old `add()` and `call_waiter()` go; the Waiter overload is the one `add()`; `run()` calls `slot.m_waiter->on_signals()`; the slot is cleared of its Waiter where it is reset. Every comment that spoke of the older form, a later step, the callback or the context names the Waiter; "later `ioservice::Outgoing`'s" is corrected, in the README too.
3. `loop_test.cpp`: `Client` is the `Waiter`; the 18 tests and the 11 of step 1 become 21; `static_assert(sizeof(loop::Slot) == 24)`. `shell_tests.cpp`: `LoopClient` is a `Waiter`. The bench: a local `Callback`.
4. Build everything and the benches; run the loop, `ioservice`, `outgoing`, `loop_server` and `autorun` host tests and `loop-dispatch-bench check`; `grep` for the removed names; clang-tidy on the changed C++ files.
5. Measure the same figures after the change; compare the objects' `sizeof` of steps 2 to 5 with the slot's saving; compare the Loop's object file before and after.
6. Two deliberate breakages of `run()`, each undone with `cmp` against a saved copy: step 2 passing `Signals::None`, and step 3 skipping the re-arm. Each must fail a host test and the target's `check_loop()`.
7. `docs/perf/261028/loop-dispatch.md` (Status Done, the gross saving corrected, a Decision), `docs/changes/261029/loop.md` (a section appended), `docs/repairs/261029/loop.md` deleted, this workspace.
8. `boot_check.py --out out/lw-remove` and one commit: `refactor(loop): a slot waits on a Waiter, and the function-pointer form is gone`.

## Files
- `system/lib/loop/include/loop/loop.hpp`, `system/lib/loop/src/loop.cpp` — the Loop
- `system/lib/loop/test/loop_test.cpp`, `system/lib/loop/test/fake_kernel.hpp` — the tests and one comment
- `system/lib/loop/README.md` — the account of the slot and the corrected sentence
- `system/app/shell/shell_tests.cpp`, `shell_tests.hpp` — `check_loop()`
- `benches/system/lib/loop/loop_dispatch_bench.cpp` — the local alias and comments
- `docs/perf/261028/loop-dispatch.md` — Done and the Decision
- `docs/changes/261029/loop.md` — a section appended
- `docs/repairs/261029/loop.md` — deleted
- `.agents/tasks/loop-waiter-remove/PLAN.md`, `RESULT.md` — this step's workspace

## Test plan
- `prebuilt/ninja/ninja -C out/bringup.x86` and `phony/benches/benches` build; the loop (21), `ioservice` (42), `outgoing` (9), `loop_server` (13) and `autorun` (5) host tests pass; `loop-dispatch-bench check` prints `check ok`.
- `grep -rn "loop::Callback\|Callback\b" system examples kernel benches --include='*.cpp' --include='*.hpp'` finds nothing that names the removed alias but the bench's own.
- clang-tidy on the changed C++ files against `out/bringup.x86` reports nothing on changed lines.
- With `run()` passing `Signals::None`, or skipping the re-arm, a loop host test and the target's `check_loop()` fail; with the sources put back, both pass.
- `boot_check.py --out out/lw-remove`: both images boot, rel logs `routing_checks: passed`, both logs hold `shell: loop checks passed`, and the host tests pass.
