# block-driver-kit: the seven steps that land the block-driver library

**Date**: 2026-11-03
**Status**: Proposed

## Request

"完善一下那个块设备的通用库吧，我感觉我们需要提供一个完整的驱动库给开发者，以便它们对我们的内核能够进行驱动开发，好让其它设备适配我们的内核。" Scoped with the user to: deepen the block library only (deferred-reply core, library BlockServer, tests, virtio-blk migration as acceptance). The general driver kit, templates and guide chapter are the next round.

## Decisions

See DESIGN.md's table; the scope, completion model and scheduler-seat decisions were made there on 2026-11-03.

## Steps

1. **Task workspace.** This document and DESIGN.md. No code. Commit: `docs(block-driver-kit): the design and the plan`.
2. **Shared fake kernel.** The fake kernel in the ioservice tests moves to a testonly target (`//system/lib/testing/fake_kernel`); ioservice's BUILD.gn files point at it; nothing else changes. Verified by the ioservice suite staying green. Commit: `refactor(testing): the fake kernel becomes a shared test target`.
3. **Base documentation.** `server.hpp`'s header comment documents the deferred-reply pattern (park channel + txid, copy the context out, reply at completion time; the shared buffers are transient within one callback); `system/lib/server/README.md` written. Commit: `docs(server): the deferred-reply pattern`.
4. **The block-server library.** Copier scaffold `system/lib/block-server`, then `BlockServer<Derived, MAX_CLIENTS, MAX_INFLIGHT>` per DESIGN.md: `on_message` dispatch, slot table, `geometry()`/`start()` CRTP hooks, `complete()`, backpressure status. Compiles; not yet used. Commit: `feat(block-server): the block-driver library`.
5. **Host tests.** The block-server suite (slot allocation, exhaustion backpressure, READ data integrity, WRITE status reply, disconnect-while-parked, epitaph paths) and one deferred-reply case for the server base. Commit: `test(block-server): the slot model on the fake kernel`.
6. **virtio-blk migrates.** The private `BlockServer` in the driver is deleted; the driver uses `//system/lib/block-server`, moves from inline polling to interrupt-driven completion (IRQ wired into the Loop — the wiring option chosen then), gains the descriptor→slot map. Verified by full `boot_check.py` and the disked run. Commit: `feat(virtio-blk): the library's server, completions on the interrupt`.
7. **Records.** Change records under `docs/changes/261103/`, the block-server README, `RESULT.md`. Full verification rerun. Commit: `docs(block-driver-kit): the record`.

## Files

- Created: `.agents/tasks/block-driver-kit/{DESIGN,PLAN,RESULT}.md`; `system/lib/testing/fake_kernel/**` (moved from ioservice tests); `system/lib/server/README.md`; `system/lib/block-server/**`; `docs/changes/261103/{server,block-server,virtio-blk}.md`; `docs/repairs/261103/` if findings arise.
- Changed: `system/lib/server/include/server/server.hpp` (comments only); `system/lib/ioservice/test/**` (include paths); `system/driver/virtio-blk/virtio_blk.cpp` and `BUILD.gn`; `docs/guide` untouched.

## Test plan

Every step: `boot_check.py --out out/bdk.x86` (host tests included) green before its commit. Step 6 adds the disked run: `make_gpt_disk.py` volume, `DISK=<file> boot_check.py`, expecting `routing_checks: passed` with no not-run line. Final: clang-format/tidy on changed C++ via the `cpp-write-and-verify` skill's scripts; pre-commit hook green on every commit.
