# public-syscall-headers: implementation plan

**Date**: 2026-11-03
**Status**: Accepted

## Request

Refactor `system/public`: keep only the userspace system headers; put each
syscall and its parameter/option structures in the same per-domain file so a
program includes just the domain it needs, instead of everything being
centralised. Approved via DESIGN.md decisions, 2026-11-03.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Scope | (a) full carve-out; (b) split gni + decls only | (a) | — | (a), with the rule: a syscall and its parameter structures share one file; domain headers are self-contained for on-demand include (2026-11-03) |
| panic/assert | move panic to kernel, assert declares its hook; or keep | Zircon shape | — | Zircon shape (2026-11-03) |
| resource.hpp license | MIT; or keep GPL-2.0 | MIT | — | MIT (2026-11-03) |
| gni location | `ours/syscalls/` under the existing root; or outside include dir | under root | — | `ours/syscalls/` (default, unobjected) |

## Steps

1. **Split the description.** Assign every entry of `ours/syscalls.gni` to the
   `kernel/lib/syscall/src/<domain>.cpp` that defines it; write
   `ours/syscalls/<category>.gni` per category (entries verbatim + a
   `category` key); `syscalls.gni` imports them and concatenates. A category
   is contiguous, which the original entry order was not (vmo appeared in
   four scattered runs), so the concatenation reorders entries and every call
   is renumbered -- allowed by design: the numbers are private to the kernel
   and the vDSO, both regenerate from this list, and the vDSO's symbols and
   signatures, the only ABI a program sees, do not move. `gen_syscalls.py`
   learns the key. Verify: build, boot, the 62-name set unchanged, vDSO ABI
   check passes.
2. **Per-category generated declarations.** The generator emits
   `public/ours/vdso_decls/<category>.inc` plus an `all.inc` that includes
   them; BUILD.gn declares the outputs; `<ours/vdso.hpp>` includes `all.inc`.
   Private outputs stay flat. Verify: build, boot, vdso ABI check passes.
3. **Per-domain public headers.** Each domain's option enums and limits move
   from `ours/syscall.hpp` beside that domain's declarations; new
   `channel/process/thread/task/vmo/vma/handle/object/debug/command/ioport/pager/time`
   headers, existing `port/interrupt/bti/sampler/exception` gain their decls;
   every domain header is self-contained (core vocabulary + the domain types
   its signatures name). ~60 include lines updated. Verify: build, boot.
4. **Header cleanup.** `panic.hpp` to `kernel/include/ours/`, `assert.hpp`
   declares its failure hook; `syscall/time.hpp` to
   `kernel/include/ours/time.hpp` (18 includes); `config/annotates.hpp`
   deleted; `syscall/resource.hpp` becomes `ours/resource.hpp`, relicensed
   MIT, listed; `clock_page/time_values/version` added to the list. Verify:
   build, boot, grep guards.
5. **Records.** Change record `docs/changes/261103/public-syscalls.md`,
   kernel-design `layering.md` updated (the "adding a syscall" recipe now
   points at `ours/syscalls/<category>.gni`), RESULT.md, REVIEW.md.

One step per commit; `verify` (ours-verify in this worktree) before each
commit.

## Files

- `system/public/ours/syscalls.gni` + `system/public/ours/syscalls/*.gni` (new)
- `system/public/ours/<domain>.hpp` (13 new), `port/interrupt/bti/sampler/exception.hpp` (edited),
  `syscall.hpp`, `assert.hpp`, `vdso.hpp`, `resource.hpp` (renamed+relicensed)
- `system/public/ours/panic.hpp`, `syscall/time.hpp`, `config/annotates.hpp` (moved/deleted)
- `kernel/include/ours/panic.hpp`, `kernel/include/ours/time.hpp` (new homes)
- `build/abi/gen_syscalls.py`, `system/public/BUILD.gn`, `public_headers.gni`
- include-line updates across `kernel/` and `system/` (~60 lines)
- docs per step 5

## Test plan

- `verify` per step: kernel builds, both images boot in QEMU.
- `numbers.hpp` diff after step 1: empty (numbers unchanged).
- `//kernel/lib/userabi/vdso:vdso.abi_check` continues to pass (exports ==
  libours.ifs, derived from the same list).
- grep guards after step 4: no references to the moved/deleted paths.
