# block-driver-kit: the block-driver library landed

**Date**: 2026-11-03

## Delivered

- `//system/lib/block-server` — `block_server::BlockServer<Derived, MAX_CLIENTS, MAX_INFLIGHT>`: the slot table, the `ours.block` dispatch, `complete()`, `ShouldWait` backpressure, and the parked-channel (channel, generation) check. 1bf30da5.
- The nine-case host suite on the fake kernel, plus the hardenings it forced: the completion contract (an Ok Read carries exactly `count × BLOCK_SIZE`; a violation is said on the console and ends the connection) and the per-op backpressure form. 5822bcf9.
- The fake kernel as a shared test target `//system/lib/testing/fake_kernel`; the ioservice suites point at it unchanged. 9bc2f6fb.
- The server base's deferred-reply pattern documented, and the server library's first README. 7f3adb73.
- virtio-blk on the library: the private server class deleted (−114 lines of plumbing), the driver down to `geometry()` and `start()`. 0b86dfdc.
- Design and plan: 3a1f4c5f. Change record: `docs/changes/261103/block-server.md`. The block-server README.

## Verification

- Host: 160 passed, 0 failed (the 9 block-server cases among them; the ioservice and all other suites unchanged).
- `boot_check.py`: dbg PASS (45), rel PASS (43, 7/7 required, `routing_checks: passed`).
- Disked (`make_gpt_disk.py` volume): rel PASS, `routing_checks: passed` with the GPT suite running — on the migrated driver.
- clang-format clean on every changed C++ file; GN formatted; documents linted.

## Deviations from the design

- The interrupt-driven completion did not land: no path delivers an interrupt into a loop::Loop (object_wait_async unsupported on interrupts; interrupt_bind needs a BindToInterrupt port, which the Loop's is not; interrupt_wait blocks the thread). This is the fallback the design stated; the gap is filed as `docs/repairs/261103/kernel-interrupts.md`, and virtio-blk completes in `start()` — the same inline poll as before, now inside the library's slot. The async path's proof is the host suite.
- The plan's separate deferred-reply case for the server base was folded into the block-server suite: every case there drives the base's pump with a parked reply, and a second suite would have needed a whole fake protocol to re-test the same lines.

## Found along the way

- The generation check exists because the kernel reissues handle values; nothing was broken by it today (the reply window was analysis, not an observed failure). It is priced into the base as one field.
- The completion contract (data size) was forced by the suite: the codec refuses an Ok Read reply of the wrong size, and the library first swallowed that as a silent connection close.

## Not done

- The general driver kit (startup skeleton, PCI/IRQ/DMA helpers, driver templates, a guide chapter) — the next round, recorded in the task's DESIGN.md non-goals.
- An IO scheduler, client-side pipelining, C++20 coroutines — each recorded in the change record with its reasons.
- The interrupt-to-Loop capability: `docs/repairs/261103/kernel-interrupts.md`, claimable on its own.
