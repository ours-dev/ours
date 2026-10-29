# loop-waiter-initservers: the root's Resource and Launcher connections become the Loop's Waiters

**Date**: 2026-10-29
**Status**: Implemented

## Request
Step 3 of 6 of `.agents/tasks/loop-waiter/PLAN.md`. After step 1 (`loop::Waiter` and `Loop::add(handle, signals, Waiter &)`, beside the function-pointer form), the root's two protocol servers, `ResourceServer` and `LauncherServer` in `system/service/init/`, register their connections as Waiters. The scope ends at those two servers: `realm.*`, `boot_server.*`, `init.cpp` and `pools.hpp` do not change, and neither does anything under `system/lib`.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| What the Waiter is | (a) the connection entry itself; (b) a separate member of the entry; (c) the server | (a): one connection has one slot and one behaviour, and the entry is what the pool already keeps at a fixed address; (c) is not one object per slot | Not asked | (a), by the brief |
| Where `on_signals()` is declared and defined | (a) private override, defined in the `.cpp`; (b) public, inline | (a): the server's `handle()` is private and its type is incomplete in the header, and only the Loop calls it, through `Waiter` | Not asked | (a), by the brief |
| How the Launcher's connection takes a second base | (a) `private LauncherOps, public Waiter`; (b) a member `Waiter` | (a): both interfaces are abstract with no data and no common names; `Waiter` must be public for `bind()` to convert | Not asked | (a), by the brief |
| A host test | (a) add one; (b) none | (b): the servers call the kernel's channel and handle system calls, and the host fake covers `ioservice` and `loop` only | Not asked | (b), by the brief's own condition |

## Steps
1. `resource_server.hpp` and `.cpp`: `ResourceConnection` is `final: public loop::Waiter` with a private `on_signals()` override, defined in the `.cpp` as `m_server->handle(*this, observed)`; `ResourceServer` befriends the connection and loses its static `on_signals()`; `bind()` calls `add(channel, CONNECTION_SIGNALS, connection)`.
2. `launcher_server.hpp` and `.cpp`: the same for `LauncherConnection`, which derives `private launch::LauncherOps, public loop::Waiter`.
3. Every comment that names the callback or the `void *` context names `on_signals()`.
4. Measure `sizeof` of the connections and the two storages, and the sections of `init`, before and after.
5. Edit-and-revert both `on_signals()` to answer `Close` to see the boot depend on them.
6. `docs/changes/261029/init.md` (a section appended), `docs/repairs/261029/init.md` for the `.data` growth, this workspace.
7. Build, host tests, clang-tidy, `boot_check.py`, one commit: `refactor(init): the Resource and Launcher connections are the Loop's Waiters`.

## Files
- `system/service/init/resource_server.hpp`, `system/service/init/resource_server.cpp` — the connection is a Waiter
- `system/service/init/launcher_server.hpp`, `system/service/init/launcher_server.cpp` — the same
- `docs/changes/261029/init.md` — a section appended
- `docs/repairs/261029/init.md` — new: the resource servers' pools are written into init's image
- `.agents/tasks/loop-waiter-initservers/PLAN.md`, `RESULT.md` — this step's workspace

## Test plan
- `prebuilt/ninja/ninja -C out/bringup.x86` builds everything; the loop (29), `ioservice` (42), `outgoing` (9), `loop_server` (12) and `autorun` (5) host tests pass.
- clang-tidy on `resource_server.cpp` and `launcher_server.cpp` against `out/bringup.x86`, header filter on `system/service/init`, reports nothing.
- With both `on_signals()` answering `Close`, the rel image fails `routing_checks` on every resource `Get` and every Launch; with the source put back it passes.
- `boot_check.py --out out/lw-initservers`: both images boot, rel logs `routing_checks: passed`, and the host tests pass.
