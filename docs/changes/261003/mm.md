# mm changes

## Title: mm answers for every Status it used to drop (system-abi-surface step 2, part 1) (2026-10-03)

**Files**
- `kernel/mm/vm_aspace.cpp` — `VmAspace::create()` owns the new address space before `init()`, and returns `init()`'s failure instead of publishing a half-built one.
- `kernel/mm/vm_mapping.cpp` — `protect_locked()` reports a region the page tables could not be brought to; the dropped `activate()` and `destroy()` answers are explicit.
- `kernel/mm/init_pmm.cpp` — `init_cache()` panics when `MemCache::init()` fails.
- `kernel/mm/vm_area.cpp`, `kernel/mm/page_queues.cpp`, `kernel/mm/include/ours/mm/vm_object.hpp` — the remaining drops made explicit, each with why.

**Symptom**
Step 2 of `.agents/tasks/system-abi-surface` is to make `Status` `[[nodiscard]]`. Counted first, by building everything with the attribute and `ninja -k 0`: 44 places drop a `Status` silently, all in the kernel, none in ring 3, whose code already writes `static_cast<void>(...)` for every answer it ignores. Ten are in mm. Two of them were wrong, not just unspoken:
- `VmAspace::create()` ignored `init()`. When the page tables or the root VMA could not be made, the half-built address space went on the global list and back to the caller as if it worked.
- `VmMapping::protect_locked()` ignored `ArchVmAspace::protect()` and answered Ok. A change that took a permission away, and failed to in the page tables, was reported as done.

**Cause**
The objects:
- `ours::Status`, the answer of most kernel functions and every system call, a plain enum.
- A call statement that discards its result. C++ allows it without a word unless the type or the function is `[[nodiscard]]`.

The events:
1. A function that could not fail grows a failure, or a caller is written assuming success.
2. Its result is dropped at the call. Nothing says so.
3. The failure, when it comes, continues as success.

`[[nodiscard]]` on the type turns step 2 into a build error at every call, so each drop becomes a decision: handle the answer, or discard it with `static_cast<void>` and a reason. This commit makes mm's decisions; the attribute lands last, when no drop is left.

**Change**

```cpp
// VmAspace::create(), before
aspace->init();
{ ... ALL_ASPACE_LIST.push_back(*aspace); }
*out = ustl::Arc<VmAspace>::adopt(aspace);
// after
auto owned = ustl::Arc<VmAspace>::adopt(aspace);
if (auto const status = aspace->init(); status != Status::Ok) {
    return status;   // dropping |owned| runs ~VmAspace on the half-built object
}
{ ... ALL_ASPACE_LIST.push_back(*aspace); }
*out = ustl::move(owned);
```

`~VmAspace` copes with what `init()` did not finish: it erases from the list only a linked node, its assertion wants no root VMA, and `ArchVmAspace::destroy()` frees page tables only if they were made.

`protect_locked()` does every region and answers with the first failure, since `m_protection_ranges` already records the new protection and stopping halfway would leave more regions out of step than going on.

The rest:
- `MemCache::init()` failing at boot: panic, as `init_kernel_aspace()` already does for its own cache.
- `VmArea::create_common()`'s and `VmMapping`'s `activate()`: dropped explicitly, with a FIXME naming `docs/repairs/261002/mm.md` item 2 and `docs/repairs/260908/heap.md` item 4. Unwinding a half-made area or mapping is a change of behaviour of its own; the user chose to leave it to those repairs (option A).
- `Base::destroy()`: it answers only Ok.
- `sleep_until()` in the page-queue rotator: the loop reads `m_state` next, which is what decides.
- `set_name()`: `set()` refuses only a null name.

**Reference**
Zircon's `zx_status_t` is a plain `int32_t` typedef (`zircon/system/public/zircon/types.h:62`), which cannot carry the attribute, so Zircon marks functions one by one: `[[nodiscard]] zx_status_t` appears 25 times in its kernel, for example in `object/include/object/process_dispatcher.h`. `ours::Status` is an enum class, so one attribute on the type covers every function that returns it.

**Verification**
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped).
- `scripts/kshell_run.py`, both images: `vmaseal.test`, `vmosnapshot.test` and `usercopy.test` ok. They create and destroy address spaces and protect mappings.
- The count of the remaining drops is taken again before the attribute lands.

**Deliberately not done**
- The `activate()` unwinding (option A, above).
- The attribute itself, and the other 34 drops, in later commits by module.

**How it was found**
The count described under Symptom.

**The user's prediction**
"Many, the kernel far more than ring 3." Measured: 44, all in the kernel and none in ring 3. The direction held; the size was smaller than "many".

**Three questions you should be able to answer**
- Why does `[[nodiscard]]` on the type reach every function that returns it?
- Why is it safe to drop the only reference to an address space whose `init()` failed?
- Why does `protect_locked()` finish the remaining regions instead of stopping at the first failure?

## Title: The aspace keeps its vDSO mapping under its lock, with a write-once copy for the syscall check (2026-10-03)

**Files**
- `kernel/mm/include/ours/mm/vm_aspace.hpp` — the user's `m_vdso_mapping` (an `Arc<VmMapping>` under `m_lock`) and `vdso()`, plus `claim_vdso()`, `release_vdso()`, `set_vdso()`, `has_vdso()` and `vdso_contains()`; `m_flags` is now `TA_GUARDED(m_lock)` and replaces `bool m_destroyed` and `std::atomic<bool> m_vdso_claimed`; the base/size atomics stay for the lock-free reader.
- `kernel/mm/vm_aspace.cpp` — `vdso()` and `set_vdso()` out of line; `destroy()` and the fault path test `Flags::Destroyed`, and `destroy()` drops the vDSO mapping.
- `kernel/mm/include/ours/mm/vmas_flags.hpp` — the user's `Destroyed` and `Vdso` bits, documented.
- `kernel/object/vma_dispatcher.cpp` — registers with `set_vdso(mapping)` instead of `set_vdso_code_range(base, size)`.
- `kernel/mm/include/ours/mm/vm_object_physical.hpp`, `kernel/mm/vm_object_physical.cpp` — the user's rename to `m_phys_base` and `m_size`, `check_range()`, and the parameter `flags`; the constructor follows.

**Symptom**
None: the tree did not build with the user's partial edit, which removed the old vDSO API in favour of a mapping held by the aspace. This entry completes it.

**Cause**
The objects:
- `m_lock`, the aspace's mutex. It is also the root region's lock, so `map_with_vmo()` takes it itself, and a caller cannot hold it across the mapping.
- `m_flags`, one word of bits: `Destroyed` and `Vdso` are written after construction.
- `m_vdso_mapping`, the vDSO code mapping, for whoever manages it.
- `m_vdso_code_base` and `m_vdso_code_size`, two atomics written once, read by every system call to check that it came from the vDSO.

The events of one registration:
1. `VmaDispatcher::map()` sees an exact, sealed RX mapping of the vDSO's code and calls `claim_vdso()`: under `m_lock`, it fails if `Vdso` is set, and otherwise sets it.
2. It maps the code without the lock.
3. On failure, `release_vdso()` clears `Vdso` under the lock, so a failed call does not keep the vDSO from ever being mapped.
4. On success, `set_vdso()` stores the Arc under the lock, then stores the base (relaxed) and the size (release).
   - If `destroy()` ran between steps 2 and 4, `set_vdso()` drops the mapping instead. A mapping holds an `Arc` to its aspace (`VmAreaOrMapping::m_aspace`), and only `destroy()` resets `m_vdso_mapping`. Storing it after `destroy()` would make an aspace and a mapping that keep each other alive forever.
   - The window is real: another process holding a handle to the region can map into it while its owner is being killed.
5. A system call's `vdso_contains()` loads the size (acquire), then the base. Seeing a size means seeing its base; seeing none refuses everything.

Why both copies: a mutex on the syscall path would put every system call of the process behind every map and fault on its address space. Zircon has the same split.

Why `m_flags` became guarded: `Destroyed` and `Vdso` are read-modify-writes of one word. If one were written under the lock and the other with an atomic or without a lock, two writers racing could each write back a word missing the other's bit. So every bit written after construction is written under `m_lock`, and the analysis now enforces it.

Why `vdso()` is out of line: returning an `Arc<VmMapping>` copies it, which retains it, and `VmMapping` is incomplete in the header.

**Change**

```cpp
// before: three atomics, one of them a bool
std::atomic<bool> m_vdso_claimed{false};
bool m_destroyed TA_GUARDED(m_lock) = false;

auto claim_vdso() -> bool {
    return !m_vdso_claimed.exchange(true, std::memory_order_acq_rel);
}

// after: the bits under the lock, the mapping beside them
Flags m_flags TA_GUARDED(m_lock);
ustl::Arc<VmMapping> m_vdso_mapping TA_GUARDED(m_lock);

auto claim_vdso() -> bool {
    Guard<sync::Mutex> guard{m_lock};
    if (!!(m_flags & Flags::Vdso)) {
        return false;
    }
    m_flags |= Flags::Vdso;
    return true;
}
```

`destroy()` moves `m_vdso_mapping` out under the lock and drops it after, as it does `m_last_fault`. The atomics are not cleared: no thread of a destroyed aspace asks.

`set_vdso()` refuses to keep a mapping that arrives after `destroy()`:

```cpp
if (!!(m_flags & Flags::Destroyed)) {
    return; // the parameter, the last Arc, dies after the guard
}
m_vdso_mapping = ustl::move(mapping);
```

Rejected:
- Checking every system call through `vdso()->contains()`, as the user's first edit did. It takes the aspace mutex on every call, and dereferences an empty Arc in a process with no vDSO.
- Keeping the claim as an atomic bool beside the flags: a bool where a bit would do.

**Reference**
- Zircon `VmAspace::vdso_code_mapping_`: guarded by the aspace lock, set in `VmAddressRegion::CreateSubVmarInternal()`, which refuses a second one with `ZX_ERR_ACCESS_DENIED`; reset in `VmAspace::Destroy()`.
- Zircon `ProcessDispatcher::vdso_code_address_`: cached once for the lock-free check in `VDso::ValidSyscallPC`.
- Zircon creates the mapping under the lock, so it needs no claim. Here the mapping takes the lock itself, hence the claim.

**Verification**
The shared tree does not build because of another session's in-flight `userboot.cpp`, so verification ran in a copy of HEAD with only this change's files laid on top.
- `ninja -k 0`: no error or warning.
- `boot_check.py`:
  - dbg PASS: 19 passed, 0 error lines;
  - rel PASS: 13 passed, 10 skipped.
- All 15 probes are killed in dbg, including `trap_outside_vdso` (the lock-free check).
- The shell's vDSO map checks pass: a second code mapping is `AccessDenied` (the claim), and a mapping into another process is `Ok`.
- `kshell_run`: `vmawx.test` passed 5 checks and `vmaseal.test` passed 13, both destroying an address space.
- Not exercised: `destroy()` landing between the mapping and `set_vdso()`. No test forces that window; the branch rests on the argument above.

**Deliberately not done**
- The user deferred the concurrency argument for the claim (which invariant, who races, the worst interleaving), on 2026-10-03; it is still to be written.
- Nothing reads `Active`, `Shared`, `OomIgnored`, `AreaMergeable` or `Aslr` after construction. If one is ever written later from outside `m_lock`, it needs a word of its own.

**How it was found**
None: completing the user's edit.

**The user's prediction**
None: the user chose option A (the mapping under the lock, plus a write-once cache) and deferred the argument.

**Three questions you should be able to answer**
- Why does `vdso_contains()` load the size before the base, and what goes wrong if `set_vdso()` stores the size first?
- Why must every bit of `m_flags` that is written after construction be written under the same lock?
- What would leak if `set_vdso()` stored the mapping after `destroy()`, and why does nothing else free it?

## Title: No reserved frame reaches the buddy: free ranges round inward, and the init area is un-reserved first (2026-10-03)

**Files**
- `kernel/mm/early_mem.cpp`:
  - `free_frames_phys_range()` rounds the start up, as it already rounded the end down. It returns the frames it freed, so the `Released N regions, M frames` line counts what was given.
  - In debug builds it asserts that no frame it frees is stamped Reserved.
  - `reclaim_init_area()` takes the init area's frames off the reserved books before freeing them.
- `kernel/mm/include/ours/mm/pm_frame.hpp` — `PageFrame::clear_reserved()`.
- `kernel/mm/include/ours/mm/pm_zone.hpp`, `kernel/mm/include/ours/mm/pm_node.hpp` — `sub_reserved_frames()`.
- `docs/repairs/261003/mm.md` (new) — the frame-flag overwrite this work found, not fixed here.

**Symptom**
Latent, found by research for the userboot-bootfs work, not by a failure. The boot image ends mid-page: on the rel image, `ours.obi` is at 0x2C3000 and 1210624 bytes long, so it ends 0x900 bytes into the page at 0x3EA000.

That whole page went to the buddy allocator at boot. `userboot_init()` still reads the image afterwards: it copies it for userboot, and parses its item table. Whoever got the page next could overwrite the tail of the `kernel` item under it. Nothing checks those bytes today: the container has no CRC flag, and the tail belongs to an item read only by phys. A CRC'd image would have failed at random.

The guard added to catch this then fired on a second case. At the end of boot, `reclaim_init_area()` frees the kernel's `.init*` frames, which are stamped Reserved:

```text
Assertion failed on !pfn_valid(pfn) || !(pfn_to_frame(pfn)->flags() & page::Flags::Reserved)
with message: frame 0x1fd77000 is reserved but inside a free range
```

**Cause**
The objects:
- bootmem's reservations. They are kept to the byte: the boot image's is exactly [base, base + size).
- The free view. It is everything between reservations, so a free range can begin mid-page, at a reservation's end.
- `free_frames_phys_range()`. It turns a free range into whole frames for the buddy.
- `stamp_reserved_frames()`. It runs before the hand-off and marks every frame a reservation touches Reserved. It rounds the end up, so the partial last page is stamped too.

The events, for the boot image:
1. phys reserves [0x2C3000, 0x3EA900) as BootPackage.
2. The free range after it starts at 0x3EA900.
3. `free_frames_phys_range()` took `phys_to_pfn(0x3EA900)`, which rounds down to 0x3EA, and freed frame 0x3EA000: a frame stamped Reserved, holding the image's last 0x900 bytes.

A partial frame is not free, so the start has to round up. Linux's `__free_memory_core()` does exactly that: `PFN_UP(start)`, `PFN_DOWN(end)`.

The init area is the other way in. Its frames are the kernel image's, stamped Reserved and counted in their node's and zone's `m_reserved_frames`. `reclaim_init_area()` freed them as they were, so the buddy held frames still marked Reserved, and both counts kept them forever. Linux frees such frames with `free_reserved_area()`: `free_reserved_page()` does `ClearPageReserved()` and adjusts the managed count before `__free_page()`.

**Change**

```cpp
// before
auto const start_pfn = phys_to_pfn(start);
// after
auto const start_pfn = phys_to_pfn(ustl::mm::align_up(start, PAGE_SIZE));
```

`reclaim_init_area()` now runs, for each Reserved frame of the range, before freeing it:
- `frame->clear_reserved()` and `frame->set_role(page::Role::None)`;
- `sub_reserved_frames(1)` on the frame's node and zone.

The guard, `DEBUG_ASSERT(!pfn_valid(pfn) || !(pfn_to_frame(pfn)->flags() & page::Flags::Reserved), ...)` under `KERNEL_DEBUG_LEVEL > 0`, makes "a frame in the buddy is not reserved" checked rather than assumed. It cannot see page-table frames, whose `Reserved` flag `mark_pinned()` overwrites (`docs/repairs/261003/mm.md`, item 1).

**Reference**
- Linux `__free_memory_core()` (`mm/memblock.c`): `PFN_UP` on the start, `PFN_DOWN` on the end.
- Linux `free_reserved_area()` and `free_reserved_page()` (`mm/page_alloc.c`, `include/linux/mm.h`).
- Zircon reserves the data ZBI rounded out to whole pages, and zeroes the slack (`MakePhysVmo()` in `zircon/kernel/phys/handoff-prep.cc`).

**Verification**
- `boot_check.py`: dbg PASS (20 passed), rel PASS (18 passed), with the guard live in dbg.
- `kshell_run.py pmnode.test`: ok, 20 checks.
- The guard fires on each bug when that bug is put back. Both checks ran on the dbg image:
  - only the old start rounding, in a copy of HEAD with this file: `frame 0x3ff000 is reserved but inside a free range`, the dbg image's tail page;
  - the new rounding without the init-area half: `frame 0x1fd77000 is reserved but inside a free range`, at the end of boot.
- The `Released 12 regions, ... frames` line reads the same before and after (126420 on dbg). The old count was the free range's byte length divided by a page, which happened to equal the new exact count. The extra frame it freed was never counted, so this line could not have shown the bug.

**Deliberately not done**
- Zircon's half: phys reserving the boot image rounded out to whole pages, with the slack zeroed. That matters when the image's pages are handed to user space, which is the next step, so it belongs there.
- The frame-flag overwrite (`docs/repairs/261003/mm.md`, item 1), which this work found.

**How it was found**
A research pass for the boot-image step read both sides of the reservation:
- `boot_package_input.hpp` reserves the exact byte size;
- `visit_free` starts the next free range at the reservation's end;
- `early_mem.cpp` rounded that start down.

The boot log's `ours.obi is at 0x2C3000, size 1210624 bytes` gave the page. The init-area case was found by the guard written for the first.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must a free range's start round up but its end round down?
- What would a CRC over the boot image have done with the old rounding, and why did nothing notice without one?
- Why does freeing the init area have to clear Reserved and the counts before the frames reach the buddy, and what in Linux does the same?

## Title: One unwire_frame() takes a frame off the reserved books, and the runtime free path checks it was (2026-10-03)

**Files**
- `kernel/mm/include/ours/mm/pmm.hpp`, `kernel/mm/pmm.cpp` — `unwire_frame()`, documented beside `free_frames()`. `free_frames()` now says what it requires.
- `kernel/mm/pm_node.cpp` — `PmNode::free_frames()` asserts, in debug builds, that the frame is not Reserved.
- `kernel/mm/early_mem.cpp` — `reclaim_init_area()` calls `unwire_frame()` instead of its inline sequence.
- `kernel/mm/include/ours/mm/pm_zone.hpp` — the comment on `sub_reserved_frames()` called the counter plain. It is `std::atomic`.

**Symptom**
None: step 2 of `.agents/tasks/boot-image-pages/PLAN.md`. The boot image's frames are about to be adopted into a VMO, which takes them off the reserved books the same way the init area's frames already were.

**Cause**
The previous fix put the "take a reserved frame off the books" sequence inline in `reclaim_init_area()`:
- clear Reserved;
- reset the role;
- subtract the node's and zone's counts.

A second user needs the same sequence. Two copies would drift, and one function is also the place to assert what a reserved frame looks like.

The guard against freeing a reserved frame covered only the boot-time path (`free_frames_phys_range()`). The runtime path, `free_frames()` through `PmNode::free_frames()`, `PmZone::free_frame()` and `FrameBuddy::insert_frame()`, looks at nothing about the frame. A VMO built from boot frames that skipped the un-wiring would hand them to the buddy silently, still flagged and still counted.

**Change**

```cpp
auto unwire_frame(PmFrame *const frame) -> void {
    DEBUG_ASSERT(!!(frame->flags() & page::Flags::Reserved), ...);
    DEBUG_ASSERT(frame->is_role(page::Role::None), ...);
    DEBUG_ASSERT(!frame->is_linked(), ...);
    frame->clear_reserved();
    // and one less reserved frame on its node and its zone
}
```

The role is left to the new owner. `reclaim_init_area()` frees the frames, and the buddy sets role Pmm; an adopting VMO will call `become<VmmBody>()`.

**Reference**
- Zircon `pmm_unwire_page()`.
- Linux `free_reserved_page()`: `ClearPageReserved()`, `init_page_count()`, `__free_page()`, `adjust_managed_page_count()`.

**Verification**
- `boot_check.py`: dbg PASS (20 passed), rel PASS (18 passed), with both guards armed in dbg.
- `kshell_run.py vmosnapshot.test pmnode.test usercopy.test`: all ok. They free many frames at run time through the newly guarded path.

**Deliberately not done**
- The adoption itself (step 4).

**How it was found**
None: a planned step.

**The user's prediction**
None given.

**Three questions you should be able to answer**
- What does `unwire_frame()` change about a frame, and what does it leave to the frame's next owner?
- Why does the runtime free path need its own Reserved check when the boot-time path has one?
- Why is a check-then-subtract on an atomic counter acceptable here?
