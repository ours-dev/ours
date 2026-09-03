# Result

Implemented `VmArea::unmap` and closed the `VmMapping::unmap` FIXME.

## Changes

- `kernel/mm/vm_area.cpp` — `VmArea::unmap(offset, size)` is real now: it validates
  (`resolve_page_range`, `check_range`, rejects `offset > m_size` so `m_base + offset` cannot
  wrap), takes the aspace lock, and delegates to the new `VmArea::unmap_locked(base, size)`,
  which walks `m_subvmas` in address order:
  - fully covered `VmMapping` → `destroy()` (PTEs, VMO list, set entry, counter);
  - partially covered `VmMapping` → `VmMapping::unmap_locked` on the clipped overlap;
  - child `VmArea` → recursion, never destroyed.
  The iterator is advanced before any disposal, since `destroy()` erases from the set and may
  free the child on the spot.
- `kernel/mm/vm_mapping.cpp` — `unmap` split into the locked-out core `unmap_locked`; on success
  it now marks the range zero-permission in `m_protection_ranges` (the FIXME's suggested fix, so
  a later `protect()` no longer touches dead PTEs and stays consistent with `protect()`'s own
  "zero-permission means unmapped" convention). `VmMapping::destroy()` now decrements
  `m_parent->m_num_mappings` so every destroy path keeps the counter honest.
- Headers: private `unmap_locked` declarations in `vm_area.hpp` / `vm_mapping.hpp`;
  `vm_area.cpp` gained `#include <ours/lockdep.hpp>` for `Guard`.

## Verification

- `clang-format -i` on all four touched files: no formatting-only hunks left.
- `clang-tidy -p out/bringup.x86`: no new diagnostics on the changed lines (remaining warnings
  are pre-existing naming/`virtual` findings elsewhere in the same files).
- `ninja -C out/bringup.x86`: full build links (87/87), pre-existing warnings only.

## Known gaps / follow-ups

- No unit test: the repo has no VmArea test harness yet (`kernel/mm/tests/` only carries the
  frame-buddy smoke test); exercising unmap needs an `ArchVmAspace` to map against.
- A later `map()` over a zero-permission (unmapped) region would re-map PTEs with no
  permissions rather than refusing; harmless today because nothing calls `map()` that way, but
  worth a guard when the fault path (`VmMapping::fault`, still a stub) grows up.
- `VmArea::protect(offset, size, flags)` is still `Status::Unimplemented`.
- The aspace lock is not recursive, so callers that already hold it must use the `_locked`
  variants (this is why the split exists).
