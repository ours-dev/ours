# loop-waiter-echo: the echo example's client becomes the Loop's Waiter

**Date**: 2026-10-29
**Status**: Implemented

## Request
Step 4 of 6 of `.agents/tasks/loop-waiter/PLAN.md`. After step 1 (`loop::Waiter` and `Loop::add(handle, signals, Waiter &)`, beside the function-pointer form), `EchoServer::Client` in `examples/echo/echo_server.cpp` registers itself as a Waiter. The scope ends at that file and this step's documents: the other programs and libraries do not change, and neither does `docs/guide/`, which another session is finishing and which quotes this file by line.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| What the Waiter is | (a) the client entry itself; (b) a separate member of the entry; (c) the server | (a): one client has one slot and one behaviour, and the entry is what `Storage` already keeps at a fixed address; (c) is not one object per slot | Not asked | (a), by the brief |
| How `connect()` fills an entry, since `client = Client{.server = this, .channel = channel}` no longer compiles | (a) two assignments to the entry's public fields; (b) a member function of `Client` that sets both; (c) private fields with `m_` names and `friend class EchoServer`, as `Binding` and the root's connections have | (a): it is how this file already treats an entry (`release()` and the failed `add()` write `channel` directly), it changes one line, and no invariant needs a function: nothing runs between the two writes and the Loop learns of the entry only at `add()`. (b) would be the only verb among bare field writes; (c) renames seven uses of `channel` for no behaviour | Not asked | (a), the brief left the form to this step |
| Where `Client::on_signals()` is defined | (a) in the class; (b) after `EchoServer`, out of line | (a): the program is one translation unit, so there is no vtable-emission reason to move it, and a nested class's body may call the enclosing class's private `serve()` | Not asked | (a), by this step |
| A host test | (a) add one; (b) none | (b): the program calls the kernel's channel and process system calls, and the host fake covers `ioservice` and `loop` only; `routing_checks` exercises it on the target | Not asked | (b), by the brief |

## Steps
1. `echo_server.cpp`: `Client` is `final: public loop::Waiter` with a private `on_signals(observed)` override that is `server->serve(*this, observed)`; `EchoServer`'s static `on_signals(void *, Signals)` goes; `connect()` fills the entry field by field and registers `client` itself.
2. Every comment that names the callback, the context or the slot's callback names `on_signals()`.
3. Measure `sizeof(Client)`, `sizeof(EchoServer::Storage)` and the sections and symbols of the echo program before and after (`llvm-size`, `llvm-nm -S`), to show that `ECHO_STORAGE` stays in `.bss`.
4. Edit-and-revert `Client::on_signals()` to answer `Close` at once, to see the boot depend on it.
5. `docs/changes/261029/echo.md`, `docs/repairs/261029/echo.md` for the one latent item found, this workspace.
6. Build, host tests, clang-tidy, `boot_check.py`, one commit: `refactor(echo): an echo client is the Loop's Waiter`.

## Files
- `examples/echo/echo_server.cpp` — the client is a Waiter
- `docs/changes/261029/echo.md` — new
- `docs/repairs/261029/echo.md` — new: a client's entry stays held after a failed re-arm
- `.agents/tasks/loop-waiter-echo/PLAN.md`, `RESULT.md` — this step's workspace

## Test plan
- `prebuilt/ninja/ninja -C out/bringup.x86` builds everything; the loop (29), `ioservice` (42), `outgoing` (9), `loop_server` (13) and `autorun` (5) host tests pass.
- clang-tidy on `echo_server.cpp` against `out/bringup.x86` reports nothing.
- With `Client::on_signals()` answering `Close`, the rel image fails `routing_checks` on `EchoString` and `Stop`; with the source put back it passes.
- `boot_check.py --out out/lw-echo`: both images boot, rel logs `init: echo exited with 0` and `routing_checks: passed`, and the host tests pass.
