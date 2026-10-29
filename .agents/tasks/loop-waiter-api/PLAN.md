# loop-waiter-api: loop::Waiter and Loop::add(handle, signals, Waiter &), beside the old form

**Date**: 2026-10-29
**Status**: Implemented

## Request
Step 1 of 6 of `.agents/tasks/loop-waiter/PLAN.md`. The Loop's callback, a function pointer and a `void *context`, is to become an abstract class (the user's choice of 2026-10-29, after `docs/perf/261028/loop-dispatch.md`). The six registration sites move one per step and every commit must build and boot, so the new form arrives first, beside the old one, over the same slot. The scope ends at `system/lib/loop`: the slot's layout and `run()` do not change, and no registration site moves.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Where the fixed function lives | (a) in the anonymous namespace of `loop.cpp`; (b) a private static member of `Loop` | (a): it needs nothing of `Loop` or `Slot`, only `Waiter`, which is public | Not asked | (a), by the brief's first choice |
| Whether `Waiter`'s copy is deleted | (a) deleted; (b) protected and defaulted, as `Delegate` and `LauncherOps` have it | (b): the sibling classes default it, so that a derived class stays copyable where its owner needs that; the brief said to follow them | Not asked | (b), by the brief |
| Whether `Waiter`'s constructor is `constexpr` | (a) yes, as `LauncherOps`; (b) no, as `Delegate` | (a): the next steps make `LoopServer::Binding` and the root's connection pools `Waiter`s, and those are `constinit` | Not asked | (a), by this step |
| Where the rules of `on_signals()` are written | (a) in both `Waiter` and `Callback`; (b) in `Waiter`, with `Callback` pointing at it | (b): `Callback` goes in step 6, so the rules should already be where they will stay | Not asked | (b), by this step |

## Steps
1. `loop.hpp`: `class Waiter` declared between `Action` and `Callback`, with the rules `Callback`'s comment had, the lifetime rule and the destructor reason. `Callback`'s comment stops saying "never a virtual call" and says what the detour is. The overload `add(handle, signals, Waiter &)`. The comments of the namespace and of `Slot::m_callback` say what a Waiter registration stores.
2. `loop.cpp`: `call_waiter()` in the anonymous namespace, and the overload, which calls the existing `add()` with it and `&waiter`.
3. `loop_test.cpp`: `WaiterClient`, a `Waiter` that does what the `Client` inside it does, `LoopTest::add_waiter()`, four tests of `add()` and seven of `run()` (`Run*Waiter*`).
4. Edit-and-revert the fixed function to see the tests catch it.
5. `README.md`; `docs/changes/261029/loop.md`.
6. Build everything that includes `loop/loop.hpp`, clang-tidy, `boot_check.py`, one commit: `feat(loop): a slot can wait on a Waiter, an abstract class`.

## Files
- `system/lib/loop/include/loop/loop.hpp` — `Waiter`, the overload, the comments
- `system/lib/loop/src/loop.cpp` — `call_waiter()` and the overload
- `system/lib/loop/test/loop_test.cpp` — `WaiterClient` and eleven tests
- `system/lib/loop/README.md` — names `Waiter` and the detour
- `docs/changes/261029/loop.md` — new
- `.agents/tasks/loop-waiter-api/PLAN.md`, `RESULT.md` — this step's workspace

## Test plan
- `out/bringup.x86/host/obj/system/lib/loop/test/loop` passes: the 18 tests of before and the 11 new ones.
- A fixed function that answers `Close` whatever `on_signals()` said, and one that passes `Signals::None` as `observed`, each make tests fail; the source is put back.
- `//:default` and `phony/benches/benches` build, which covers every user of `loop/loop.hpp` (init, echo, the shell, ioservice, the bench).
- clang-tidy on `loop.cpp` and `loop_test.cpp` against `out/bringup.x86`, with the header filter on `system/lib/loop`, reports nothing.
- `boot_check.py`: both images boot, `routing_checks: passed` on rel, host tests pass.
