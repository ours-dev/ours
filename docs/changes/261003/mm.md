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
