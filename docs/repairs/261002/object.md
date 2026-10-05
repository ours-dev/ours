# object outstanding repairs

## 1. The check that a sub-region allows no more than its parent cannot be reached from ring 3, and is not tested

**Severity**
Low: a missing test.

**Symptom**
None visible. The AccessDenied branch in `VmaDispatcher::allocate()` (`kernel/object/vma_dispatcher.cpp:317-319`) never runs in any test.

**Cause**
The system call first requires the parent handle to hold Read, Write or Execute for each CanMap bit (`kernel/lib/syscall/src/vma.cpp:57-72`). A VMA handle's rights come from its region's allowance (`vma_dispatcher.cpp:53-63`), and `handle_duplicate` can only narrow them. So the handle check refuses first, and the shell's "nested sub-region asking for Execute" check exercises that check, not this branch. Without the branch, `validate_vmaflags()` in `create_subvma()` (`kernel/mm/vm_area.cpp:93`) would still refuse, with InvArgs. The branch fixes the status code for a kernel caller.

**Change**
Test it where it can be reached: a `kernel/commands` test, as `vma_seal_test.cpp` does for refusals ring 3 cannot reach. It wraps a read-write `VmArea` in a `VmaDispatcher`, calls `allocate(CanMapRead | CanMapExecute, ...)` and expects AccessDenied. Say in the shell's comment which check its nested case exercises.

**Reference**
Zircon `VmAddressRegion::CreateSubVmarInner()` (`zircon/kernel/vm/vm_address_region.cc:122`), whose `vmar_flags & ~flags_ & VMAR_CAN_RWX_FLAGS` check is the counterpart.

**Acceptance**
The kernel command passes on both images, and fails with the branch removed.
