# loop-waiter: six steps that replace loop::Callback with an abstract Waiter

**Date**: 2026-10-29
**Status**: Done

## Request
The user asked on 2026-10-28 whether the Loop's callback, a function pointer and a `void *context`, should become an abstract class or a function reference, as the other function-pointer-and-context structs of the component framework had (`docs/changes/261028/ioservice.md`, `svc.md`, `launch.md`), and chose to measure first. The measurement is `docs/perf/261028/loop-dispatch.md`: a virtual call costs 0.76 ns more than today's call, and only when the processor mispredicts the target; a function reference costs the same as today's; one `write(2)` takes about 580 ns on the host. On 2026-10-29 the user chose the abstract class. The scope ends at the Loop's callback and its six registration sites; the Loop's dispatch rules, its slot table's sizing and the kernel are not touched.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| The shape of the Loop's callback | (a) keep the function pointer and context; (b) an abstract `Waiter` that a slot points at; (c) `ustl::FunctionRef`, a non-owning function reference | (c): it removes the hand-written trampolines and the casts, keeps a slot's size, and leaves `Realm` as it is, while (b) makes `Realm` split one `Instance` into two waiter members | Not asked | (b), 2026-10-29 |
| Where `Waiter` is declared | (a) `loop::Waiter`; (b) `Loop::Waiter`, as the report wrote it | (a): `Slot` is declared before `Loop` and holds the pointer, so `Waiter` must come first | Not asked | (a), by the plan |
| How the sites move | (a) all six in one commit; (b) the new form beside the old, one site per commit, the old form removed last | (b): every commit builds and boots, and a wrong site is found alone | Not asked | (b), by the plan |

The report's saving of "8 KiB in a table of 1024 slots" is gross. Each registered object gains a vptr, so where every slot has an object of its own the net saving is about zero; the reasons to choose (b) are the interface and the removed trampolines, not memory.

## Steps
Each step has a workspace `.agents/tasks/loop-waiter-<name>/` with its own `PLAN.md` and `RESULT.md`, a worktree made with `python3 scripts/new_worktree.py <branch> <start>`, and one commit made as `git commit -- <paths>`. Every step follows CLAUDE.md, with `ours-verify` before the commit and a change record in `docs/changes/<YYMMDD>/<module>.md`. The user is asked before each step starts.

### 1. `loop::Waiter` and `Loop::add(handle, signals, Waiter &)`, beside the old form
Background: the six sites must move one at a time and every commit must boot, so the new form arrives first and shares the old form's slot. A `Waiter` registration stores a fixed function in the slot's callback and the `Waiter` in its context; the function calls `on_signals()`. The slot's layout and `run()` do not change. Step 5 removes the old form and the detour.

- Branch `lw-api`. Workspace `loop-waiter-api`.
- `system/lib/loop/include/loop/loop.hpp`: `class Waiter` with `virtual auto on_signals(ours::Signals observed) -> Action = 0;` and a protected non-virtual destructor (the root has no heap, so no deleting destructor; follow `ioservice::Handler` and `launch::LauncherOps` for the form). It carries the rules `Callback`'s comment states today (one unit of work; never closes its own handle; Rearm only once what asserted the signal is consumed; may add()), and says the object must outlive its slot, which the Loop does not own. The overload `add(ours::Uandle, ours::Signals, Waiter &)`, documented as the old `add()` is, in terms of it. The comment on `Callback` stops saying "never a virtual call": it says a Waiter registration makes one.
- `system/lib/loop/src/loop.cpp`: the overload and its fixed function.
- `system/lib/loop/test/loop_test.cpp`: a Waiter is called with the observed signals and its Action is obeyed (Rearm, Close); a Waiter and a function-pointer registration share one Loop; a Waiter's `add()` fails like the old one (NoResource when the table is full, the handle stays the caller's).
- `system/lib/loop/README.md`, if it describes the callback; `docs/changes/261029/loop.md`.
- Verify: the loop host tests, a build of everything that includes `loop/loop.hpp` (the bench too), clang-tidy as `cpp-write-and-verify` describes, and `boot_check.py` on both images.
- Commit: `feat(loop): a slot can wait on a Waiter, an abstract class`.

### 2. `LoopServer` and `Outgoing`
`ioservice::LoopServer::Binding` becomes the `Waiter`, and `LoopServer::on_signals(void *, Signals)` goes. Their tests move with them.

### 3. `ResourceServer` and `LauncherServer`
Each connection object becomes a `Waiter`.

### 4. `EchoServer`
The example's connection object becomes a `Waiter`.

### 5. `Realm`
One `Instance` registers two waits with two behaviours, the outgoing directory's (`on_outgoing`) and the process's exit (`on_terminated`). It gets two small waiter members, each naming its `Instance`. `Realm`'s `Instance` pool is a fixed array of 8, so the cost is two vptrs and two back pointers each.

### 6. Remove `Callback`
`Slot` holds a `Waiter *`, `run()` calls `on_signals()` directly, the old `add()` and the fixed function go, the loop tests and the shell's `check_loop()` use `Waiter`, the bench's description is brought in line, and `docs/perf/261028/loop-dispatch.md` gets its Status changed from Measuring to Done with the decision recorded.

## Files
- Step 1: `system/lib/loop/include/loop/loop.hpp`, `system/lib/loop/src/loop.cpp`, `system/lib/loop/test/loop_test.cpp`, `system/lib/loop/README.md`, `docs/changes/261029/loop.md`, the step's workspace.
- Steps 2 to 6: named in each step's own plan, written when the step starts.

## Test plan
- Every step: the loop host tests and the other host tests pass (`scripts/host_tests.py` lists them), clang-tidy on the changed C++ files is clean, and `boot_check.py` passes both images with `routing_checks: passed`.
- Step 6: `grep -rn "loop::Callback" system examples` finds nothing, and the size of `loop::Slot` is `static_assert`ed smaller than before.
