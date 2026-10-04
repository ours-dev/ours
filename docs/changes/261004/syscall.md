# syscall changes

## Title: task_create_exception_channel and object_set_property, for a process's exception channel (2026-10-04)

**Files**
- `system/public/ours/syscalls.gni` — the two entries.
- `system/public/ours/vdso.hpp`, `kernel/lib/syscall/include/syscall/handlers.hpp` — include `<ours/exception.hpp>` and `<ours/object_property.hpp>`, whose types the calls take.
- `kernel/lib/syscall/src/exception.cpp` — new: `task_create_exception_channel()`.
- `kernel/lib/syscall/src/object.cpp` — `object_set_property()`.
- `kernel/lib/syscall/BUILD.gn` — the new file.
- `examples/shell/shell_tests.cpp` — every fault probe gets a channel, checked; the refusals once; a verdict line.
- `.agents/tasks/exception-channels/PLAN.md` — step 4 done.

**Symptom**
None: nothing is sent on a channel yet. Step 4 of `.agents/tasks/exception-channels/`.

**Cause**
The exceptionate (step 3) has no way in from user space. Zircon has two calls for it: one to bind a channel to a task, and the generic property call a handler answers an exception with.

**Change**
- `task_create_exception_channel(handle, options, &out)`. `options` must be None. The handle must name a process and grant Inspect, Duplicate, Transfer, ManageThread and Enumerate, Zircon's rights. A thread or a job answers NotSupported rather than BadType: Zircon binds to both, and they are missing here, not wrong. Then a channel pair is made. The reading end gets `Transfer | Wait | Read`, Zircon's `kExceptionChannelRights`, because the kernel's end never reads: anything a program could write to it would sit there as long as the process lives. The order follows design 4.1: the reading end's value is written to the program, then the kernel's end is bound (`Exceptionate::set_channel()`, which may still answer AlreadyBound or BadState), and only then is the reading end installed. A refusal at any point leaves nothing installed.
- `object_set_property(handle, property, value, size)`, Zircon's signature. The property is looked at first, as `object_get_info()` looks at the topic first: the table has no lookup that does not name the object's type. ExceptionState wants SetProperty on an exception handle (BadType for anything else) and at least 4 bytes (BufferTooSmall). The value is read as a plain `u32` and checked against TryNext and Handled (InvArgs otherwise) before it becomes an `ExceptionState`, since a value read straight from user memory could be any number. An unknown property is InvArgs, Zircon's answer, though here it comes before the handle check.
- The shell binds a channel to each of the 16 fault probes after the launcher returns the process and before the processargs go out (the probe waits for them, so it cannot fault first). It checks that a second bind is AlreadyBound, that `channel_write` on the reading end is AccessDenied, and that the channel reports PeerClosed, with nothing to read, once the probe is dead. Once: the root job answers NotSupported, and `object_set_property(ExceptionState)` on the job is BadType. `shell: exception channel checks passed` or FAILED carries the verdict to `boot_check.py`.

**Reference**
Zircon `sys_task_create_exception_channel()` (zircon/kernel/lib/syscalls/exceptions.cc) and `sys_object_set_property()`'s `ZX_PROP_EXCEPTION_STATE` case (zircon/kernel/lib/syscalls/object.cc).

**Verification**
- Isolated copy of HEAD with only this change: `boot_check.py --out out/task-wake`, dbg PASS (23 passed: the exception channel checks are new), rel PASS (20 passed), 0 error lines.
- dbg_1's log: `task_create_exception_channel(job) = NotSupported`, `object_set_property(job, ExceptionState) = BadType`, `exception channels behaved on 16 of the probes`, `exception channel checks passed`.
- clang-tidy: no finding in the new file or on the changed lines.
- Not exercised: `object_set_property` on a real exception (step 5 creates the first one).

**Deliberately not done**
- Sending on the channel: step 5. Until it lands the probes still die unannounced, so a bound channel shows only PeerClosed.
- Threads and jobs, the debugger option, and `exception_get_thread` / `_process`: not in slice 1.

**How it was found**
None: a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can the reading end not be written, and what would a writable one let a program do?
- Why is the reading end's value written to the program before the channel is bound, and what does a program hold when the bind is then refused?
- Why is the value of `object_set_property` read as a `u32` and checked before it becomes an `ExceptionState`?

## Title: vma_unmap, so that a program can take mappings and sub-regions back out of a region it holds (2026-10-04)

**Files**
- `system/public/ours/syscalls.gni` — the `vma_unmap(vma, addr, length)` entry, right after `vma_map`; `vma_allocate`'s doc now says a sub-region also goes with an unmap of its parent that covers it whole.
- `kernel/lib/syscall/src/vma.cpp` — `syscall::vma_unmap()`; the undo paths of `vma_allocate` and `vma_map` say which kind of unmap they want (OpChildren Yes and No).
- `kernel/object/include/ours/object/vma_dispatcher.hpp`, `kernel/object/vma_dispatcher.cpp` — `VmaDispatcher::unmap()` refuses an unaligned base and takes an `OpChildren`; `VmaDispatcher::op_children_from_rights()`; two comments that said `vma_unmap` was still to come.
- `kernel/mm/include/ours/mm/vm_area.hpp`, `kernel/mm/vm_area.cpp` — `mm::VmAreaOpChildren`; `VmArea::unmap()` takes it (Yes by default, for the kernel's own callers) and asks `is_active()` under the address-space lock; `unmap_locked()` refuses a wholly covered sub-area when it is No.
- `examples/shell/shell_tests.cpp` — `check_vma_unmap()` and `check_unmap_sealed()`, run right after `check_vdso_map()`; `read_vdso_layout()`, taken out of `check_vdso_map()` so that both can find the vDSO's code; a comment that said no program can unmap.
- `kernel/commands/vma_seal_test.cpp` — its header no longer says that no program can reach the unmap refusals.
- `docs/repairs/261002/object.md` — item 2 is reachable now.
- `docs/repairs/261004/mm.md` — new: a partial unmap keeps the mapping's whole range; `VmArea::protect()` asks whether the area is alive before taking the lock.

**Symptom**
None: step 9 of `.agents/tasks/component-framework/PLAN.md`. A program could put mappings and sub-regions into its address space (`vma_map`, `vma_allocate`) and never take one out: whatever it mapped stayed until the process died. Under decision D14 (b) of `docs/design/261004/component-framework.md`, the root's Launcher maps each client's program VMO into its own address space to check it and read it in place, then has to let go of it (step 11). Without an unmap, every launch would leave a mapping behind in the most privileged process.

**Cause**
The call is simple to write; what takes thought is everything it must not be able to take down. Four things, each with its own check:

- **Sealed mappings, the vDSO's code above all.** The kernel accepts a `syscall` instruction only from the vDSO's code range, which is recorded once per address space when the code is mapped (`VmAspace::set_vdso()`) and never cleared, so that `vdso_contains()` can read it without a lock on every system call. If a program could unmap that code and map pages of its own at the same address, a `syscall` it wrote itself would pass the check, and every guarantee that rests on "only the vDSO traps" would be gone. That is why the vDSO's code must be mapped Sealed, and why `vma_unmap` refuses any range that overlaps a sealed mapping, here or in any sub-region below (`has_sealed_mapping_locked()`, which walks the mappings flattened across sub-regions). The refusal is asked before anything is torn down: a range that covers an unsealed neighbour and then a sealed page would otherwise unmap the neighbour and only then fail, and half an unmap cannot be put back. The shell checks exactly that: after three refused ranges that all covered the page below a sealed mapping, that page is still there (a Specific `vma_map` over it answers AlreadyExists).
- **Sub-regions, and who may destroy them.** A sub-region is a reservation of address space, often reached through a handle someone else holds: the launcher reserves a program's whole span and maps each segment into it. A range that covers only part of one has no good meaning (shrink it? split it? leave a gap?), so it is refused, as Zircon refuses it. A range that covers one whole destroys it, with everything in it, and every handle to it answers BadState from then on. Whether a handle may do that is what the OpChildren right says: a handle narrowed to drop it can still unmap the mappings beside a sub-region, but not the sub-region. Every VMA handle carries OpChildren unless a copy drops it (`RIGHTS_DEFAULT_VMA`, as Zircon's `ZX_DEFAULT_VMAR_RIGHTS`), and `mm::VmArea::unmap_locked()` already had a comment saying the gate was missing only because no system call needed it yet. Without the gate the right would mean nothing for unmap. A refusal, not a skip: skipping the sub-region and answering Ok would leave something in a range the caller was told is empty.
- **An unaligned address.** `mm::resolve_page_range()` rounds the base down and the length up, each on its own, which suits a kernel caller that names "the page this address is in". For a program it turns a mistake into damage: `vma_unmap(vma, 0x1008, 0x1000)` would become `[0x1000, 0x2000)`, taking 8 bytes the caller did not name and leaving 8 it did. So `VmaDispatcher::unmap()` refuses it with InvArgs before mm sees it, which is where Zircon puts the same check.
- **A region that is already gone.** `VmArea::unmap()` used to ask `is_active()` before it took the address-space lock, while `destroy()` clears Active under that lock. That was harmless while only the kernel unmapped; with `vma_unmap` two threads can race. Objects: a sub-region S, the root region's handle, S's own handle. Events: thread B calls `vma_unmap(S, ...)` and reads Active, true; thread A calls `vma_unmap(root, all of S)`, takes the lock and destroys S, emptying it; B takes the lock, finds no mappings in S and answers Ok, having done nothing to a region that no longer exists. With the check moved under the lock, B answers BadState, as Zircon does. `VmArea::protect()` has the same shape; it has no system call yet, so it is recorded (`docs/repairs/261004/mm.md`, item 2) rather than changed here.

One more consequence reaches the existing undo paths. `vma_map` and `vma_allocate` undo a call whose result they cannot write back by unmapping its range, and `docs/repairs/261002/object.md` item 2 explains why that is wrong once something else can unmap: between the map and the undo, another thread can unmap the new object and put its own there, and the undo then removes that. This change does not fix that (see "Deliberately not done"), but it narrows it: `vma_map`'s undo now passes OpChildren No, so if it finds a sub-region in its range it is refused rather than destroying another thread's reservation. `vma_allocate`'s undo must pass Yes, because what it undoes is a sub-region.

**Change**
`VmArea::unmap()` before:

```cpp
auto VmArea::unmap(VirtAddr base, usize size) -> Status {
    if (!is_active()) {
        return Status::BadState;
    }
    auto [aligned_base, aligned_size] = resolve_page_range(base, size);
    // ... InvArgs for size 0, InvArgs for a range outside this area
    Guard guard{get_lock()};
    if (has_sealed_mapping_locked(aligned_base, aligned_size)) {
        return Status::AccessDenied;
    }
    return unmap_locked(aligned_base, aligned_size);
}
```

after, in Zircon's order (size, lock, state, range, then the walk):

```cpp
auto VmArea::unmap(VirtAddr base, usize size, VmAreaOpChildren const op_children) -> Status {
    auto [aligned_base, aligned_size] = resolve_page_range(base, size);
    if (!aligned_size) {
        return Status::InvArgs;
    }
    Guard guard{get_lock()};
    if (!is_active()) {
        return Status::BadState;
    }
    if (!check_range(aligned_base, aligned_size)) {
        return Status::InvArgs;
    }
    if (has_sealed_mapping_locked(aligned_base, aligned_size)) {
        return Status::AccessDenied;
    }
    return unmap_locked(aligned_base, aligned_size, op_children);
}
```

and in `unmap_locked()`'s check pass, which runs before anything is destroyed:

```cpp
if (child->base() < base || child->last() > last ||
    op_children == VmAreaOpChildren::No) {
    return Status::InvArgs;
}
```

The system call resolves the handle with its rights, asking for none, and hands the OpChildren bit down:

```cpp
auto vma_unmap(ours::Uandle const vma, VirtAddr const addr, usize const length) -> Status {
    // ... get_dispatcher_with_rights(vma, Rights::None, &region, &rights)
    return region->unmap(addr, length, object::VmaDispatcher::op_children_from_rights(rights));
}
```

`VmaDispatcher::unmap()` refuses an unaligned base and passes the rest to mm. The system call may not name `mm::` (`.agents/skills/ours-kernel-design/references/layering.md`), so the dispatcher re-exports the enum as `VmaDispatcher::OpChildren`.

Options rejected:
- The handle's `Rights` as the dispatcher's parameter instead of the enum. Then `vma_allocate`'s undo, which must destroy the sub-region it made, would depend on whether the caller's parent handle had OpChildren, and fail to undo when it did not.
- No default for `VmArea::unmap()`'s new parameter. Every kernel caller (vmalloc and three test commands, twelve calls) owns what it unmaps and wants Yes; the default keeps them as they were.
- Skipping a covered sub-region without OpChildren instead of refusing: see Cause.

The shell's new check runs on both images, before `check_handle_close()`, because on dev.dbg_1 that closes the root job and the sealed half needs the job to make a process. On the shell's own root region, in a stretch no other check uses, it pins down: a mapping unmapped whole can be mapped again at the same place, and a load through the new mapping reads the new VMO, not the page the first load put in the TLB; an empty range is Ok; unmapping the first and last pages of a three-page mapping leaves the middle readable, and unmapping the rest frees the whole range; through a sub-region's handle, ranges past its end, from before its start or running out of it are InvArgs; through the root's, half a sub-region is InvArgs, the whole of it InvArgs from a copy without OpChildren and Ok from the root's own handle, after which the sub-region's handle answers BadState for both a map and an unmap and its range can be reserved again; an unaligned address, a length of 0, one too large to round up and the kernel half are InvArgs, a VMO's handle BadType, an invalid one BadHandle. On a process made for the purpose and never started, with the vDSO laid out as a launcher lays it out (its first page read-only at a chosen offset, its code sealed after it): the whole image, the code, and the code with the page after it are AccessDenied; the read-only page before the code goes; the code is still in place afterwards. An ordinary two-page sealed mapping with an unsealed page on each side: its second page, a range from the page below into it, one from it into the page above, and all four pages are AccessDenied, and the page below survives all of them; a sub-region holding a sealed mapping cannot be destroyed through its parent and goes on taking mappings.

Where this departs from the plan: the plan's text has the sealed-vDSO refusals in "a new shell check"; they are, but on a process the check makes rather than on the shell's own vDSO, as Zircon's own test does. Had the refusal been broken, unmapping the shell's own vDSO would have killed the shell and hidden every later check's result.

**Reference**
- Zircon `sys_vmar_unmap()` (`zircon/kernel/lib/syscalls/vmar.cc:251`): `GetDispatcherAndRights()` with no right required (a TODO there, fxbug.dev/42107929), then `vmar->Unmap(addr, len, op_children_from_rights(vmar_rights))`.
- Zircon `VmAddressRegionDispatcher::Unmap()` (`zircon/kernel/object/vm_address_region_dispatcher.cc:289`): `!IsPageRounded(base)` is ZX_ERR_INVALID_ARGS, before the VMAR is asked. `op_children_from_rights()` (`zircon/kernel/object/include/object/vm_address_region_dispatcher.h:63`) is the counterpart of `VmaDispatcher::op_children_from_rights()`.
- Zircon `VmAddressRegion::Unmap()` (`zircon/kernel/vm/vm_address_region.cc:858`): `RoundUpPageSize(size)`, 0 is INVALID_ARGS, then the locks, then `state_ != ALIVE` is BAD_STATE, under them. `UnmapInternalLocked()` (`:877`): outside the region is INVALID_ARGS, `IntersectsVdsoCodeLocked()` is ACCESS_DENIED, and a child VMAR partly covered, or covered at all when `can_destroy_regions` is false, is INVALID_ARGS, all before the first child is destroyed. Two differences: Zircon refuses only the vDSO's code, where this refuses every Sealed mapping (the seal is a general option here, which the launcher asks for); and Zircon splits a mapping partly in the range (`VmMapping::UnmapLocked()`, `zircon/kernel/vm/vm_mapping.cc:445`), where this one keeps its range (`docs/repairs/261004/mm.md`, item 1).
- Zircon's test of the same refusals: `VdsoTests.vdso_map_change_test` in `src/zircon/tests/vdso/vdso.cc`, on a scratch process, which is why `check_unmap_sealed()` makes one.
- This project's interface design already specified the call: `docs/design/260923/userspace-interface.md:135` (unaligned address refused by the dispatcher because mm would round it, vDSO AccessDenied from mm, a partly covered sub-region refused, a wholly covered one needing OpChildren).
- Linux, for contrast: `do_vmi_munmap()` (`mm/vma.c:1630`) refuses an unaligned start with EINVAL, rounds the length with `PAGE_ALIGN()`, and answers 0 when nothing is mapped there, as here. `vms_gather_munmap_vmas()` (`mm/vma.c:1398`) refuses a VMA sealed with `mseal(2)` with EPERM while it is still gathering, before anything is unmapped: the same asked-first rule. Linux has no handles, so it has nothing like OpChildren: whoever shares the mm may unmap anything in it.

**Verification**
- `boot_check.py`: dbg PASS (25 passed, 1 skipped, 0 failed), rel PASS (23 passed, 3 skipped, 0 failed), 0 error lines; both logs end the new check with `shell: vma_unmap checks passed`. Before the change the counts were 24 and 22.
- `kshell_run.py` on the dbg image, since `VmArea::unmap()` changed: `vmaseal.test: ok -- 13 checks`, `vmosnapshot.test: ok -- 205 checks`, `usercopy.test: ok -- 18 checks`.
- clang-tidy on `vma.cpp`, `vm_area.cpp`, `vma_dispatcher.cpp` and `shell_tests.cpp` against `out/bringup.x86`: no finding on a changed line. `scripts/format_cpp.py --check`: clean.
- Not measured: the cost of an unmap and its TLB shootdown. The boot runs four CPUs, but the stale-TLB check loads from one thread right before and right after, so it catches a missing invalidation on the CPU that thread runs on, and a missing remote one only if the thread moved in between.

**Deliberately not done**
- `docs/repairs/261002/object.md` item 2 (undo by identity) asked to land before `vma_unmap`. Doing it means `VmaDispatcher::map()` handing back the mapping it made, a way for mm to destroy exactly that object if it is still linked, and for a sealed vDSO mapping, item 1, giving back a registration that is designed never to be cleared. Those are design choices of their own, so the item stays open, with its severity raised and the `vma_map` side narrowed as described in Cause.
- Splitting a partly unmapped mapping, and `vma_protect` and `vma_destroy`: not needed by the Launcher. The first is recorded in `docs/repairs/261004/mm.md`.
- The stale "there is no vma_unmap" comments in `system/lib/launch/include/launch/bootfs.hpp` and `kernel/lib/userabi/userboot/userboot.cpp` are left for step 11, which changes both files and decides whether those mappings now go.

**How it was found**
None: a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must the vDSO's code be impossible to unmap, given that the kernel records its range once and checks every `syscall` against that record?
- A range covers an unsealed page and then a sealed one. Why is the sealed check run over the whole range before anything is unmapped, and what would a program observe if it ran page by page instead?
- What does a VMA handle without OpChildren still allow, what does it no longer allow, and why is that a property of the handle rather than of the region?
