# mm outstanding repairs

## 1. Unmapping part of a mapping leaves the mapping's whole range reserved

**Severity**
Medium: reachable from ring 3 since `vma_unmap` (2026-10-04); address space is held that a program believes it gave back, and nothing is read or written wrongly.

**Symptom**
After `vma_unmap` of part of a mapping, the unmapped pages fault as they should, but the range stays taken: a `vma_map` with Specific over it answers AlreadyExists, and a mapping placed anywhere never lands there, until the rest of the mapping is unmapped too. The shell's `check_vma_unmap()` (`examples/shell/shell_tests.cpp`) avoids the case on purpose: it unmaps the first and last pages of a three-page mapping, reads the middle, and only maps the range again after the whole mapping is gone.

**Cause**
`VmArea::unmap_locked()` hands a partly covered mapping to `VmMapping::unmap_locked()` (`kernel/mm/vm_area.cpp:451`), which clears the page tables of the overlap and marks it permission-less in `m_protection_ranges` (`kernel/mm/vm_mapping.cpp:526-552`). The mapping object keeps its base and size, so it still occupies the whole range in its parent's `m_subvmas`, which is what placement searches. The hole is a hole in permissions, not in the address space.

A second consequence is latent: `VmArea::protect()` over the hole would write a permission into those regions again (`VmMapping::protect_locked()`, `kernel/mm/vm_mapping.cpp:407`), and the next fault would map the pages back, quietly undoing the unmap. No program can protect yet (there is no `vma_protect`), so this cannot be triggered today.

**Change**
Trim or split the mapping, as Zircon does: unmapping its first or last pages moves its base or shrinks its size; unmapping a middle range leaves two mappings, the right one with the object offset advanced and its share of the protection ranges. Every new mapping has to be allocated before anything is torn down, so that an allocation failure leaves the old mapping whole. Then a protect over a range that is not fully mapped can answer NotFound, as Zircon's does, instead of resurrecting anything. The alternative, refusing a partial unmap, would depart from Zircon, whose `zx_vmar_unmap()` splits a mapping partly in the range, and from the interface design's `vma_unmap`, which refuses only a range that partly covers a sub-region (`docs/design/260923/userspace-interface.md:135`).

**Reference**
Zircon `VmMapping::UnmapLocked()` (`zircon/kernel/vm/vm_mapping.cc:445`) creates the left and right mappings first and then replaces the old one; `VmAddressRegion::Protect()` (`zircon/kernel/vm/vm_address_region.cc:985-997`) answers ZX_ERR_NOT_FOUND when part of the range is not mapped.

**Acceptance**
A shell check: map three pages, unmap the middle one, then a Specific `vma_map` of one page into the hole is Ok and reads the new VMO, while the outer pages still read theirs. Both images boot.

## 2. VmArea::protect() asks whether the area is alive before taking the lock

**Severity**
Low: a wrong status in a race, reachable only once `vma_protect` exists.

**Symptom**
Latent, cannot be triggered today. A protect through a sub-region's handle racing with an unmap of the parent that destroys the sub-region can answer Ok, having changed nothing, where it should answer BadState.

**Cause**
`VmArea::protect()` reads `is_active()` (`kernel/mm/vm_area.cpp:555`) before it takes the address-space lock (`:575`), and `VmAreaOrMapping::destroy()` clears Active under that lock. A destroyed area has no children left, so the walk under the lock finds nothing and the call returns Ok. `VmArea::unmap()` had the same shape and was fixed with `vma_unmap` (`docs/changes/261004/syscall.md`), because that call made the race reachable; protect has no system call yet. The unlocked read is also the data race of `docs/repairs/261002/mm.md` item 3.

**Change**
Move the `is_active()` check under the lock, after the argument checks that need no state, as `unmap()` now does.

**Reference**
Zircon `VmAddressRegion::Protect()` checks `state_` under the region's lock.

**Acceptance**
`vma_protect`, when it lands, answers BadState through the handle of a sub-region destroyed by an unmap of its parent; `vmaseal.test` still passes.
