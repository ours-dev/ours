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
