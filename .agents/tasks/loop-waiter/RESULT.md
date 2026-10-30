# loop-waiter: the Loop's slot waits on an abstract Waiter, and the function-pointer callback is gone

**Date**: 2026-10-29

## Delivered
`loop::Loop` no longer calls a function pointer with a `void *context`. A slot holds a `loop::Waiter *`, and `run()` calls its virtual `on_signals()`. Every registration site is a `Waiter`. The plan is `PLAN.md` here; each step has its own workspace `.agents/tasks/loop-waiter-<step>/` with a `PLAN.md` and a `RESULT.md`. The user chose this shape over the function reference that the plan's Decisions table recommended, on 2026-10-29.

- Plan: `ec327f51`.
- Step 1, `23b6e9e7`: `loop::Waiter` and `Loop::add(handle, signals, Waiter &)` beside the old form, made over it through one fixed function.
- Step 2, `c2b7a75a`: `ioservice::LoopServer::Binding` is the Waiter of its connection's slot.
- Step 3, `94a62227`: the root's `ResourceConnection` and `LauncherConnection`.
- Step 4, `ff090c31`: echo's `Client`.
- Step 5, `75cb036b`: `Instance`'s `OutgoingWaiter` and `TerminatedWaiter`, one for each of the two waits an instance has.
- Step 6, `3cb0d8a5`: `Slot` holds a `Waiter *`; the `Callback` alias, the old `add()` and the fixed function are removed; the report `docs/perf/261028/loop-dispatch.md` is closed with the decision recorded.

## Verification
- The Loop's host tests: 21 pass. Mutation checks: passing `Signals::None` to the Waiter fails 8 of them, and skipping the re-arm fails 6; each also fails the shell's `check_loop()` on the target (`loop checks FAILED`).
- `ioservice` 42, `outgoing` 9, `loop_server` 13 and `autorun` 5 host tests pass, and `loop-dispatch-bench check` prints `check ok`.
- `boot_check.py` on the tree of step 6: dbg 44 passed, 0 failed; rel 41 passed, 6 of 6 required lines including `routing_checks: passed` and `init: echo exited with 0`; 148 host tests passed. Both logs show `shell: loop checks passed`. Step 4 was the first boot of steps 1 to 4 together.
- Each step's deliberate breakage made a boot check or a host test fail: step 3 by answering `Close` in both connections (8 `routing_checks` failures), step 4 by answering `Close` in the echo client, step 5 by dropping the `TerminatedWaiter`'s handler (a required line missing).
- The memory account, measured at the tree before the task (75099335), before step 6 and after it (`.agents/tasks/loop-waiter-remove/RESULT.md`, `docs/changes/261029/loop.md`): `sizeof(Slot)` 32 to 24 bytes. A Waiter object gains a vptr, so the net per connection is 0 (Binding 16 to 24, ResourceConnection 16 to 24, echo Client 16 to 24, LauncherConnection 5112 to 5120) and +16 per `Instance` (224 to 256, which holds two waiters). `init` takes +64 bytes of RAM over the task (1,472 paid in vptrs against 1,408 saved in the slot table); echo takes none. `.text` falls by about 400 bytes in each program, from the second `add()` and the fixed function. The report's earlier "8 KiB in a table of 1024 slots" was gross; the net is the figure above.

## Deviations from the design
- `Waiter`'s copy operations are protected and defaulted, as `Delegate` and `LauncherOps` have them, and not deleted.
- `LauncherConnection` has two abstract bases, `launch::LauncherOps` privately and `loop::Waiter` publicly, and so two vptrs.
- `Instance` has two waiter members, not one `Waiter`, and sets their back pointers in `attach()`, not in a constructor: the Realm resets a pool entry with `= Instance{}`, which copies the waiters' fields from a temporary.
- The plan said the 8 KiB saving was the point of the change; it is not, and the plan's Decisions table says so. The reasons for the shape are the interface and the removed trampolines.

## Found along the way
- A failed re-arm frees the Loop's slot and leaves the owner's connection held, in `LoopServer`, in echo and in the root's two connection pools. `docs/repairs/261029/ioservice.md` item 1 and `docs/repairs/261029/echo.md` item 2. It cannot happen today and needs the user's decision on the Loop's spec.
- The five resource servers' pools moved from `.bss` to `.data` when their connections gained a vptr, which costs init's file about 22 KiB of zeros. `docs/repairs/261029/init.md` item 1, with options for the user.
- No boot check covers the clearing of an instance's `m_outgoing`: with the `OutgoingWaiter` not clearing it, the boot still passes, because `Realm::forward()` also tests `m_state`. The code is right; the gap is in what the boot exercises.

## Not done
- A timing of the real `Loop::run()` before and after, which the perf report does not hold; proposed in `.agents/tasks/loop-waiter-remove/RESULT.md`, not taken.
- The two decisions above (the Loop's spec for a failed re-arm, and the resource pools' `.data`).
- Loose uses of the word "callback" in `ioservice` and `component` comments.
- The developer's guide under `docs/guide/` still cites `loop.hpp`, `loop.cpp` and `echo_server.cpp` by their old lines, and shows no `Waiter` code yet; the old-to-new line map is in the step 6 workspace.
