# loop-waiter-realm: a component instance's two waits become the Loop's Waiters

**Date**: 2026-10-29
**Status**: Implemented

## Request
Step 5 of 6 of `.agents/tasks/loop-waiter/PLAN.md`. After step 1 (`loop::Waiter` and `Loop::add(handle, signals, Waiter &)`, beside the function-pointer form) and steps 2 to 4, the root's `Realm` is the last caller of `Loop::add(handle, signals, Callback, void *)`. One `Instance` registers two waits with two behaviours, the outgoing directory's client end (`Realm::on_outgoing`) and the process (`Realm::on_terminated`), each with the `Instance` as its `void *` context. The scope ends at `system/service/init/realm.hpp` and `realm.cpp` and this step's documents: `init.cpp`, `pools.hpp`, the servers and everything under `system/lib` do not change. Step 6, which removes the old form, waits for the user's go-ahead.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| What the Waiters are | (a) one Waiter, the `Instance`, which switches on which wait fired; (b) two small members of the `Instance`, one per wait; (c) one member holding both behaviours | (b): one wait has one behaviour, and the `observed` signals do not say which wait fired (a signal bit is read per object type), so (a) would need a tag of its own, which is a second member in disguise | Not asked | (b), by the brief |
| How a member finds its `Instance` | (a) a back pointer set by `Instance::attach()` where the entry is set up; (b) a back pointer set by `Instance`'s constructor; (c) `offsetof` from the member to the `Instance` | (a): `Realm` resets an entry with `child = Instance{}`, which copies the waiters' fields, so a constructor that pointed them at `this` would leave the entry naming the destroyed temporary; (c) is undefined on a polymorphic class | Not asked | (a), by this step |
| `on_outgoing` and `on_terminated` | (a) private non-static `Realm` members that take `Instance &`; (b) private static `Realm` members that take `Instance &`; (c) the bodies move into the waiters | (a): the brief's choice. They use no `Realm` state today, as `forward()` does not, and the two wait behaviours stay in the class that owns the supervision of instances; (c) would make `Instance` know about `Moniker` and the process info | Not asked | (a), by the brief |
| A host test | (a) add one; (b) none | (b): `Realm` calls `channel_create`, `launch::start()` and `object_get_info` and needs the kernel; the host fake covers `ioservice` and `loop` only. The boot is the proof | Not asked | (b), by the brief |

## Steps
1. `realm.hpp`: `Instance` gets two private nested classes, `OutgoingWaiter` and `TerminatedWaiter`, each `final: public loop::Waiter` with a private `on_signals()` override and a back pointer; two members; a private `attach(Realm &)` that sets `m_realm` and both back pointers. `Realm::on_outgoing` and `on_terminated` become private non-static members that take `Instance &`.
2. `realm.cpp`: the two `on_signals()` definitions hand the call to the Realm; `resolve_root()` and `resolve()` call `attach()` where they set `m_realm`; the two `add()` calls pass the waiters; the two handlers take `Instance &` and keep their bodies; the two static `on_*(void *, Signals)` go.
3. Every comment that named the callbacks, the `void *` context or the slot's callback names the Waiters.
4. Measure `sizeof(Instance)`, `INSTANCES` and the sections of `init` before and after (`-fdump-record-layouts-simple`, `llvm-size`, `llvm-nm -S`).
5. Two deliberate breakages, each followed by `cmp` against the committed file: `TerminatedWaiter::on_signals()` answering `Close` without calling the Realm, and `OutgoingWaiter::on_signals()` answering `Close` without clearing `m_outgoing`.
6. `docs/changes/261029/init.md` (a section appended), this workspace.
7. Build, host tests, clang-tidy, `boot_check.py`, one commit: `refactor(init): a component instance's two waits are the Loop's Waiters`.

## Files
- `system/service/init/realm.hpp` — the waiters, `attach()`, the two handlers' declarations
- `system/service/init/realm.cpp` — the waiters' bodies, the registrations, the handlers
- `docs/changes/261029/init.md` — a section appended
- `.agents/tasks/loop-waiter-realm/PLAN.md`, `RESULT.md` — this step's workspace

## Test plan
- `prebuilt/ninja/ninja -C out/bringup.x86` builds everything; the loop (29), `ioservice` (42), `outgoing` (9), `loop_server` (13) and `autorun` (5) host tests pass.
- `grep -rn "Callback\b" system examples` finds only the Loop's own definitions and its tests.
- clang-tidy on `realm.cpp` against `out/bringup.x86` reports nothing.
- With `TerminatedWaiter::on_signals()` answering `Close` at once, `init: echo exited with 0` is missing on the rel image; with `OutgoingWaiter::on_signals()` doing the same without clearing `m_outgoing`, the route to an exited provider no longer answers NotFound. With the sources put back, both pass.
- `boot_check.py --out out/lw-realm`: both images boot, rel logs `init: echo exited with 0` and `routing_checks: passed`, and the host tests pass.
