# loop-waiter-notify: the Loop tells a Waiter when it cannot arm its wait again

**Date**: 2026-10-30
**Status**: Implemented

## Request
The maintainer chose option (a) of `docs/repairs/261029/ioservice.md` item 1: when `Loop::run()` cannot arm a slot's wait again after a `Waiter` answered `Rearm`, it tells the `Waiter`, so that an owner who keeps an entry per slot can release it. The scope is the Loop's spec and code, every `Waiter` that holds an entry per slot (`LoopServer::Binding`, `ResourceConnection`, `LauncherConnection`, echo's `Client`), their tests and records, and the two repair items. `docs/guide/` and `examples/hello/` are not touched: a following step updates them from `RESULT.md`.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| How the Loop tells the Waiter | (a) a second virtual function, called after the slot is free; (b) a failed re-arm treated as an answer of `Close`; (c) leave it | (a) | Not asked | (a), by the maintainer, 2026-10-30 |
| The default of the new function | empty inline body; pure virtual | empty inline: a `Waiter` that only answers `Close` is never told | Not asked | empty inline body, by the brief |
| Whether `Binding` logs the cause | log; do not | do not: `ioservice` has no debug-line dependency and gains none for a message | Not asked | do not, by the brief |
| What the new tests use for the ioservice side | extend the ioservice fake kernel with `fail_next_wait_async`; a new fake | extend the existing fake, mirroring the Loop's own | Not asked | extend, by this step |
| The repair items' numbers after the deletions | close the numbering up; keep the numbers | keep: other documents cite items 2 and 3 of `ioservice.md` and item 3 of `echo.md` by number | Not asked | keep, by this step |

## Steps
1. `loop.hpp`, `loop.cpp`: `Waiter::on_rearm_failed(cause)`; `run()` step 4 tells the `Waiter` after the slot is free; the spec (steps 3 and 4, I1, I3, the `Waiter` comment) says it. Verified by `loop_test.cpp` (six new tests) and five deliberate breakages.
2. `loop_server.hpp`, `loop_server.cpp`, the ioservice fake kernel and `loop_server_test.cpp`: `Binding` releases its connection. Verified by two new tests (the acceptance of the repair item) and three breakages.
3. `resource_server`, `launcher_server`, `echo_server.cpp`: the same, with the debug line each already uses; `realm.hpp` comments. Verified by the build, clang-tidy and the boot (no host test exists for them).
4. Sizes of `init` and `echo_server` before (a baseline worktree at `a8489d18`) and after, with the attribution of every byte of the growth.
5. Records: `docs/changes/261029/{loop,ioservice,init,echo}.md`, the two repair items deleted, `docs/repairs/261029/abi.md` for what the sizes showed, `README.md` of `loop` and `ioservice`, this workspace.
6. Verification (build, host tests, clang-tidy, `boot_check.py`) and one commit.

## Files
- `system/lib/loop/include/loop/loop.hpp`, `src/loop.cpp`, `test/loop_test.cpp`, `README.md` — the Loop
- `system/lib/ioservice/include/ioservice/loop_server.hpp`, `src/loop_server.cpp`, `test/fake_kernel.hpp`, `test/fake_kernel.cpp`, `test/loop_server_test.cpp`, `README.md` — `Binding`
- `system/service/init/resource_server.hpp`, `resource_server.cpp`, `launcher_server.hpp`, `launcher_server.cpp`, `realm.hpp` — the root's pools, and the comments of the two waiters that keep the default
- `examples/echo/echo_server.cpp` — echo's client
- `docs/changes/261029/loop.md`, `ioservice.md`, `init.md`, `echo.md` — appended
- `docs/repairs/261029/ioservice.md` (item 1), `echo.md` (item 2) — deleted items; `docs/repairs/261029/abi.md` — new
- `.agents/tasks/loop-waiter-notify/PLAN.md`, `RESULT.md` — this workspace

## Test plan
- `prebuilt/ninja/ninja -C out/bringup.x86` and `phony/benches/benches` build; from `out/bringup.x86`, `host/obj/system/lib/loop/test/loop` 27 passed, `.../ioservice/test/loop_server` 15, `.../ioservice/test/outgoing` 9, `.../ioservice/test/ioservice` 42, `host/obj/system/service/init/test/autorun` 5.
- Each new test fails when its part of the change is removed (breakages put back and compared with `cmp`).
- clang-tidy against `out/bringup.x86` on the changed C++ files reports nothing.
- `boot_check.py --out out/lw-notify`: both images boot, `routing_checks: passed` and `shell: loop checks passed`, host tests pass.
