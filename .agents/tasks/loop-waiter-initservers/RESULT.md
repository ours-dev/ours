# loop-waiter-initservers: the root's Resource and Launcher connections are the Loop's Waiters

**Date**: 2026-10-29

## Delivered
Step 3 of 6 of `.agents/tasks/loop-waiter/PLAN.md`, one commit on the branch `lw-initservers` (its hash is in the report that handed this step back, since a commit cannot name itself), `refactor(init): the Resource and Launcher connections are the Loop's Waiters`, on top of `23b6e9e7`:

- `ResourceConnection` is `final: public loop::Waiter`; its private `on_signals()` calls `ResourceServer::handle()`. `LauncherConnection` is `final: private launch::LauncherOps, public loop::Waiter` and does the same with `LauncherServer::handle()`. The two static `on_signals(void *, Signals)` are gone, and both `bind()` calls pass the connection to `Loop::add(handle, signals, Waiter &)`.
- The comments that named the callback or the context name `on_signals()`.
- `docs/changes/261029/init.md` (a section appended), `docs/repairs/261029/init.md` (new, one item), this workspace.

## Verification
- `prebuilt/ninja/ninja -C out/bringup.x86` builds everything. Host tests, from `out/bringup.x86`: `host/obj/system/lib/loop/test/loop` 29 passed, `host/obj/system/lib/ioservice/test/ioservice` 42, `outgoing` 9, `loop_server` 12, `host/obj/system/service/init/test/autorun` 5.
- clang-tidy against `out/bringup.x86`, `--header-filter='.*/system/service/init/.*'`, on `resource_server.cpp` and `launcher_server.cpp`: exit 0, no diagnostic. `format_cpp.py --check` on the four files: clean.
- Deliberate breakage, both `on_signals()` answering `Close`, rel image only: `routing_checks` reports 8 failed checks (`Debug`, `Irq`, `Ioport` and `Sampling` `Get` = `PeerClosed`, three refused Launches = `PeerClosed`, one spawn = `NoResource`), the shell's `Debug.Get`, `Sampling.Get` and root job checks fail, and `pc-ps2` cannot get its I/O port resource. The source was put back (`cmp` against a saved copy) and rebuilt.
- `boot_check.py --out out/lw-initservers` with the final tree: build ok; dbg PASS (shell finished at 5.9 s, 44 passed, 4 skipped, 0 failed, 0 error lines); rel PASS (4.8 s, 41 passed, 8 skipped, 0 failed, 6/6 required lines, 0 error lines, `routing_checks: passed`, `pc-ps2: key a` and `pc-ps2: key A`); host tests PASS (148 passed, 0 failed). Both logs hold `shell: loop checks passed`.
- What covers the servers: no host test, since `ResourceServer` and `LauncherServer` call the kernel's channel and handle system calls and the host fake covers `ioservice` and `loop` only; on the target `routing_checks` connects to the Launcher and to the Debug, Irq, Ioport and Sampling protocols, the shell runs every command through the Launcher and checks the root job, and `pc-ps2` gets its Irq and Ioport resources.
- `sizeof`, from a compile check (`template <usize N> struct Show;` in a scratch file outside the tree): `ResourceConnection` 16 to 24, `LauncherConnection` 5112 to 5120, `ResourceServer::Storage` 4448 to 4480 (five servers), `LauncherServer::Storage` 28928 to 28960. `init`: `.data` 28928 to 51360, `.bss` 77664 to 55424, `.text` 108766 to 108798.
- Not measured: the time of a message through a connection, and anything on x86-64 hardware.

## Deviations from the design
None. One consequence the plan did not foresee, which is a cost and not a deviation: the five `ResourceServer::Storage` globals were all zeros and lay in `.bss`; a non-zero vptr moves each, buffers included, into `.data`, so init's file grows by about 22 KiB. It is the mechanism of `docs/repairs/261004/init.md` item 9 for the Launcher's pool. Recorded as `docs/repairs/261029/init.md` item 1, with options for the user; not fixed here, since it would change `init.cpp` and what a server owns.

## Found along the way
- The saving of this step is in the source, not in the machine code: the two trampolines (19 and 18 bytes) became the connections' `on_signals()` (19 and 19 bytes), and the Launcher's second base adds a 23-byte `this`-adjusting thunk, so `.text` grew by 32 bytes.
- A first sizeof probe included the main checkout's headers, because `-I../..` from the build directory reaches the main checkout from a worktree under `.worktrees/`. Its numbers agreed with the worktree's `llvm-nm -S`, since the main checkout's server files are unmodified, and the probe was redone with an include relative to the file. Not a defect of the tree.

## Not done
- Steps 2 and 4 to 6 of `.agents/tasks/loop-waiter/PLAN.md`: `LoopServer` and `Outgoing` are another session's, `EchoServer`, `Realm` and the removal of `Callback` wait for the user's go-ahead.
- The `.data` growth: `docs/repairs/261029/init.md` item 1.
