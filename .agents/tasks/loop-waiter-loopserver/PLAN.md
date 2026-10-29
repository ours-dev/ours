# loop-waiter-loopserver: a LoopServer connection's Binding is the Loop's Waiter

**Date**: 2026-10-29
**Status**: Implemented

## Request
Step 2 of 6 of `.agents/tasks/loop-waiter/PLAN.md`. `ioservice::LoopServer::Binding` becomes a `loop::Waiter`, so that the first of the six registration sites moves from the Loop's function pointer and `void *context` to the abstract class the user chose on 2026-10-29. The scope ends at `LoopServer`: `Outgoing`, `BootServer` and the init service are checked and changed only if they must be. The Loop, `Slot`, and the other four sites are not touched.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Where `Binding::on_signals()` is defined | (a) in the class; (b) in `loop_server.cpp` | (b): it is then the first non-inline virtual function, so the vtable is emitted once, in that file, as `Delegate`'s is through `write_failed()` | Not asked | (b), by this step |
| How `Binding` derives from `Waiter` | (a) public; (b) private, with `LoopServer` as friend | (a): the brief says `public`, and the overload takes a `Waiter &` | Not asked | (a), by the brief |
| Whether to add a test | (a) none, naming the older tests that cover it; (b) one test | (b): a deliberate breakage that registers `bindings[0]` for every connection passed all 21 older tests, so none of them pinned that a connection is reached through its own Binding | Not asked | (b), by the mutation result |
| The comments of `Outgoing` and `BootServer` that say "a connection's callback" | (a) reword them; (b) leave them | (b): neither file otherwise changes, and `on_signals()` is still a callback in the plain sense | Not asked | (b), by the brief |

## Steps
1. `loop_server.hpp`: `Binding` is `final: public loop::Waiter` with `on_signals(observed) -> loop::Action override`; the static `LoopServer::on_signals(void *, Signals)` is deleted; the comments that name the callback or the context say a Binding's `on_signals()`.
2. `loop_server.cpp`: `LoopServer::Binding::on_signals()` is `m_server->handle(m_connection, observed)`; `watch()` calls the Waiter overload of `add()`.
3. Read `Outgoing`, `BootServer`, init's and echo's storage, and the tests, for registrations of their own, for `constinit` storage holding Bindings, and for any copy of a Binding.
4. `loop_server_test.cpp`: edit-and-revert four breakages of the wiring to see which tests catch them; add the test the fourth shows to be missing.
5. `README.md`; `docs/changes/261029/ioservice.md`; `docs/repairs/261029/ioservice.md` for the one defect found.
6. Build everything and `phony/benches/benches`, clang-tidy, `boot_check.py --out out/lw-loopserver`, one commit: `refactor(ioservice): a LoopServer connection's Binding is the Loop's Waiter`.

## Files
- `system/lib/ioservice/include/ioservice/loop_server.hpp` — `Binding` derives from `Waiter`; the static function goes; comments
- `system/lib/ioservice/src/loop_server.cpp` — `Binding::on_signals()`; `watch()` registers the Binding
- `system/lib/ioservice/test/loop_server_test.cpp` — one test and two helpers
- `system/lib/ioservice/README.md` — names the Binding as a `loop::Waiter`
- `docs/changes/261029/ioservice.md` — new
- `docs/repairs/261029/ioservice.md` — new, one item
- `.agents/tasks/loop-waiter-loopserver/PLAN.md`, `RESULT.md` — this step's workspace

## Test plan
- `out/bringup.x86/host/obj/system/lib/ioservice/test/{loop_server,outgoing,ioservice}` and `.../loop/test/loop` pass: 13, 9, 42 and 29 tests, the older ones unchanged.
- Four breakages of the wiring each make tests fail, and the source is put back.
- `//:default` and `phony/benches/benches` build.
- clang-tidy on the changed C++ files and on `outgoing.cpp`, `boot_server.cpp` and `echo_server.cpp` reports nothing.
- `boot_check.py --out out/lw-loopserver`: both images boot, `routing_checks: passed` on rel, `shell: loop checks passed` in both logs, host tests pass.
