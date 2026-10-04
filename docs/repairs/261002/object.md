# object outstanding repairs

## 1. vma_map keeps a Sealed mapping, and the vDSO's claim, when it cannot write the address back

**Severity**
Medium: wrong on a path reachable today, by a caller that passes an address it cannot write.

**Symptom**
`vma_map(vma, PermRead | PermExecute | Sealed, 0, vdso, 0, size, (VirtAddr *)KERNEL)` answers InvArgs, yet the vDSO stays mapped, claimed and registered as the process's vDSO range. A retry with a good pointer answers AccessDenied, because the claim is taken, and the caller never learns where the vDSO is. Every Sealed mapping behaves the same way: the call reports a failure and the mapping stays.

**Cause**
When the system call cannot copy the address out, it undoes the mapping with `region->unmap(mapped, length)` (`kernel/lib/syscall/src/vma.cpp:172-174`) and discards the answer. `VmArea::unmap()` refuses any range that holds a sealed mapping (`kernel/mm/vm_area.cpp:380-387`): sealing exists to make that refusal. So nothing is undone. For the vDSO, `VmaDispatcher::map()` has already taken the claim and registered the code range (`kernel/object/vma_dispatcher.cpp:192-206`) before it returns.

**Change**
Undo by identity, not by range (item 2). Destroy the mapping just made: nobody has been told about it, so its seal has promised nothing yet. Give back the vDSO claim and range in the same undo. The alternative is to register them only after the address has been written back.

**Reference**
Zircon `sys_vmar_map()` (`zircon/kernel/lib/syscalls/vmar.cc:186`) destroys the mapping it made, `map_result->mapping->Destroy()`, when the address cannot be copied out.

**Acceptance**
After the call above, a second `vma_map` of the vDSO with a good pointer is Ok. A shell check pins it on both images.

## 2. vma_allocate and vma_map undo a failed call by address range, not by the object they made

**Severity**
Medium: reachable since `vma_unmap` landed (2026-10-04, component-framework step 9), but only by threads that share the region handle racing each other, which in practice means one program hurting itself.

**Symptom**
Not observed. A failing `vma_allocate` can remove another thread's mapping or sub-region, and a failing `vma_map` another thread's mapping. The timeline: thread A's call makes its object and then fails to write the result back; before A's undo runs, thread B, holding the same region handle, unmaps A's new object with `vma_unmap` and maps or reserves something of its own at the same range; A's undo then removes B's object. Since step 9, `vma_map`'s undo passes OpChildren No, so it no longer destroys a sub-region it finds there; `vma_allocate`'s has to pass Yes, since what it undoes is a sub-region.

**Cause**
Both system calls undo a call whose result they cannot hand back by unmapping `[base, base + size)` in the parent (`kernel/lib/syscall/src/vma.cpp:81`, `:89` and `:183`). The new object has not been published, but its range has: `vma_unmap` lets another thread or another process holding the same region handle unmap the new object and map something of its own there between the two steps. A kernel-chosen address is easy to predict, because placement takes the lowest room that fits. The undo then removes that other object.

**Change**
Keep the object the call made: the child's `Arc<VmArea>` (`allocate()` already holds it), and the `Arc<VmMapping>`, which needs `VmaDispatcher::map()` to return it as well as its address. On failure, destroy exactly that object under the address-space lock, if it is still linked. Item 1 is the same change seen from the sealed side, and item 3 can use it too.

**Reference**
Zircon `sys_vmar_allocate()` (`zircon/kernel/lib/syscalls/vmar.cc:65`, `vmar_dispatcher->Destroy()`) and `sys_vmar_map()` (`:186`, `map_result->mapping->Destroy()`) both undo by identity.

**Acceptance**
It was meant to land before `vma_unmap`, and did not: step 9 of `.agents/tasks/component-framework/PLAN.md` added the call without it (`docs/changes/261004/syscall.md`). The undo paths name the object, not the range; the shell's write-back checks still pass on both images.

## 3. vma_allocate leaves the sub-region reserved when VmaDispatcher::create() fails

**Severity**
Medium: a leak of address space, reachable only once allocation can fail (`docs/repairs/260908/heap.md` item 3).

**Symptom**
Latent, cannot be triggered today. If making the child's dispatcher fails, the call answers OutOfMem and the sub-region stays reserved, with no handle to it, until the address space dies. With kernel placement the caller was never even told where it is.

**Cause**
`VmaDispatcher::allocate()` links the child into its parent with `create_subvma()` and then makes the dispatcher with `create()` (`kernel/object/vma_dispatcher.cpp:274-281`). `create()` can return OutOfMem (`:35-38`, `new (mm::GAF_USER) Self`). The system call returns that status without an undo (`kernel/lib/syscall/src/vma.cpp:88-91`). Allocation failure is unreachable today because allocations spin when memory runs out (`heap.md` item 3).

**Change**
When `create()` fails, destroy the child just made before returning, with the identity undo of item 2.

**Reference**
Zircon `VmAddressRegionDispatcher::Allocate()` has the same shape, so there is no counterpart to copy. Zircon's undo in `sys_vmar_allocate()` (`vmar.cc:65`) covers only the failures after the dispatcher exists.

**Acceptance**
With allocation failure injectable, a failed `vma_allocate` leaves its range free: the same request placed again lands at the same address.

## 4. The check that a sub-region allows no more than its parent cannot be reached from ring 3, and is not tested

**Severity**
Low: a missing test.

**Symptom**
None visible. The AccessDenied branch in `VmaDispatcher::allocate()` (`kernel/object/vma_dispatcher.cpp:268-271`) never runs in any test.

**Cause**
The system call first requires the parent handle to hold Read, Write or Execute for each CanMap bit (`kernel/lib/syscall/src/vma.cpp:68-83`). A VMA handle's rights come from its region's allowance (`vma_dispatcher.cpp:45-55`), and `handle_duplicate` can only narrow them. So the handle check refuses first, and the shell's "nested sub-region asking for Execute" check exercises that check, not this branch. Without the branch, `validate_vmaflags()` in `create_subvma()` (`kernel/mm/vm_area.cpp:90`) would still refuse, with InvArgs. The branch fixes the status code for a kernel caller.

**Change**
Test it where it can be reached: a `kernel/commands` test, as `vma_seal_test.cpp` does for refusals ring 3 cannot reach. It wraps a read-write `VmArea` in a `VmaDispatcher`, calls `allocate(CanMapRead | CanMapExecute, ...)` and expects AccessDenied. Say in the shell's comment which check its nested case exercises.

**Reference**
Zircon `VmAddressRegion::CreateSubVmarInner()` (`zircon/kernel/vm/vm_address_region.cc:122`), whose `vmar_flags & ~flags_ & VMAR_CAN_RWX_FLAGS` check is the counterpart.

**Acceptance**
The kernel command passes on both images, and fails with the branch removed.
