# public-syscall-headers: review of the per-domain public surface refactor

**Date**: 2026-11-03

## Scope and method

Reviewed the four commits on `exp/public-syscalls` (0b02c363, 568f656b, 3f20c57f, a84831f2): the split description, the generator's per-category outputs, the 17 domain headers, and the surface cleanup. Authorities: `kernel-design` (layering), the Zircon reference tree (`zircon/vdso/*.fidl`, `zircon/system/public/zircon/{assert.h,syscalls/}`), `cpp-write-and-verify` (clang-format/clang-tidy run against `out/bringup.x86`). Boot evidence from `boot_check.py`, not read-only judgement.

## What is done well

- `system/public/ours/syscalls.gni:47` — the concatenation spells the number order in one place, visibly, with the renumbering rationale in the comment above it.
- `system/public/ours/vdso.hpp:26` — the umbrella is a flat include list of domain headers; a reader sees the whole surface at a glance.
- `system/public/ours/pager.hpp:12` — the dependency that makes on-demand includes work (`pager_create_vmo` names `VmoOptions`) is the header's own include of `vmo.hpp`, not a hidden transitive.
- `system/public/ours/assert.hpp:24` — the hook contract is written where the hook is declared, with the PHYS_EXPORT reason stated.

## High severity

None.

## Medium severity

### M1. `object.hpp` depends on `port.hpp` for one option word

`system/public/ours/object.hpp:11`. `object_wait_async`'s signature names `WaitAsyncOptions`, which lives in `port.hpp`, so the object header pulls the port header. Zircon does not have this edge (`syscalls/object.h` does not include `syscalls/port.h`; its `zx_object_wait_async` takes a plain `uint32_t`). If the object domain grows more wait-async options, consider moving `WaitAsyncOptions` beside `object_wait_async` (its only caller) and leaving `port.hpp` with the packet types. Not done now: moving it would touch the port header's internal logic comments for no functional gain today.

## Minor and style

- `system/public/ours/time.hpp` (clock calls) vs `kernel/include/ours/chrono.hpp` (time vocabulary): two near-synonym names for time things. Documented in both files; if the clock header ever moves, `ours/clock.hpp` would read better and free `time.hpp` for the vocabulary.
- `docs/changes/261103/public-syscalls.md` says "32 headers"; the exact count belongs to `public_headers.gni` (which is diff-checked against the directory), so the prose number can drift — the check is the authority, not the sentence.

## Teaching opportunities

### T1. The include-order shadow the refactor had to dodge

`kernel/products/BUILD.gn:10` puts `//kernel/include` and `//system/public` on the same kernel-toolchain include path, in that order. Two headers with the same `ours/…` path in both roots would resolve differently per toolchain — the trap `format_gn.py`'s docstring warns about in another form. Hence `chrono.hpp`. The general rule: a path under `ours/` must mean the same file on both roots or exist on exactly one.

### T2. Why renumbering was free

A syscall number appears in exactly two generated files (`numbers.hpp` for the kernel's table, `vdso_stubs.inc` for the vDSO) and nowhere in a program: the vDSO's symbols are the ABI. That is what design D8 bought; this refactor is the first change to spend it. Zircon has spent the same freedom repeatedly — its numbers have been reordered across releases with no user-visible effect.

### T3. The hook pattern, three examples

`vpanic(std::string_view)` is declared in a public header and defined per environment: kernel (`kernel/top/panic.cpp`), phys (`kernel/phys/panic.cpp`), host test (`profdata_test.cpp`). Zircon's `__zx_panic` is the same shape; libc's `abort` is the same idea one layer down. The header declares the contract; the environment pays for it.

## Suggested follow-up order

1. Land the vdso-published-data branch; fold its four time-domain entries into `ours/syscalls/time.gni` and list `version.hpp`.
2. When a sysroot step is wanted, `public_headers.gni` is ready as its input; nothing else needs to change.
3. Optionally move `WaitAsyncOptions` beside `object_wait_async` (M1) the next time the wait-async surface changes anyway.
