# vm-unmap

## Problem

- `VmArea::unmap(offset, size)` (`kernel/mm/vm_area.cpp`) is a stub returning `Status::Unimplemented`.
- `VmMapping::unmap` (`kernel/mm/vm_mapping.cpp`) tears PTEs down but leaves `m_protection_ranges`
  stale (its own FIXME): a later `protect()` would touch page-table entries that no longer exist.

## Requirements

1. `VmArea::unmap(offset, size)` removes the mappings inside the VMA-relative page range
   `[m_base + offset, m_base + offset + size)`:
   - child `VmMapping` fully covered → destroyed (PTEs, VMO mapping list, parent set, `m_num_mappings`);
   - child `VmMapping` partially covered → pages of the overlap torn down, object kept;
   - child `VmArea` → recursed into, never destroyed (unmap does not destroy areas).
2. `VmMapping::unmap` additionally marks the unmapped range as zero-permission in
   `m_protection_ranges`, making "no permission" the canonical representation of "unmapped"
   (the same convention `protect()` already uses to unmap zero-permission regions).
3. No lock self-deadlock: the aspace `SpinLock` is not recursive, so `VmArea::unmap` holds the
   lock and delegates to new private `*_locked` helpers instead of re-entering
   `VmMapping::unmap`.

## Design

- `VmMapping::unmap(offset, size, control)` — validate, take the aspace lock, delegate to new
  private `unmap_locked(base, size, control)` (VmArea is a friend, so it can call it).
- `VmMapping::unmap_locked` — existing per-region arch unmap loop, then
  `m_protection_ranges.update(base, size, MmuFlags{}, m_base, m_base + m_size)` on success.
- `VmArea::unmap(offset, size)` — validate (`resolve_page_range` + `check_range`, reject
  `offset > m_size` to keep `m_base + offset` from wrapping), take the aspace lock, delegate to
  new private `VmArea::unmap_locked(base, size)`.
- `VmArea::unmap_locked` — walk `m_subvmas` in address order, advance the iterator before any
  disposal; fully covered mappings go through `VmMapping::destroy()`, partial overlaps through
  `VmMapping::unmap_locked` on the clipped overlap, sub-areas recurse.
- `m_num_mappings` is decremented in `VmMapping::destroy()` (VmMapping is a friend of VmArea) so
  every destroy path keeps the counter honest.

## Validation

- clang-format + clang-tidy (prebuilt llvm-23, `out/bringup.x86` compile_commands).
- `ninja -C out/bringup.x86` compiles.

## Result

See RESULT.md.
