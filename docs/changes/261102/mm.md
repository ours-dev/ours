# mm changes

## Title: the watchdog drops its mutex for the OutOfMemory walk (2026-11-02)

**Files**
- `kernel/mm/memory_watchdog.cpp` — `look()` samples the distance, unlocks, walks, then locks again and reads the levels
- `.agents/skills/ours-kernel-design/references/mm.md` — the lock covers the levels and the declaration

**Symptom**
Latent for a running system: nothing else takes `MemoryWatchdog::m_lock`. A test's `Hold` or `set_levels()` blocks for the whole OutOfMemory walk, and that walk sleeps in the page owner's lock.

**Cause**
`m_lock` guards the levels, the hysteresis, and the declaration. `look()` held it from the first read of free memory through `set_free_memory_signal()`. The walk does not read those. Holding the mutex across it made `Hold` mean "the watchdog is not inside `look()`" rather than "the watchdog is not arming the gate." `set_levels()` waits to replace the levels for the same reason.

Keeping the mutex was the alternative. A production boot has no second taker, so the tests would simply wait. The mutex's range would stay larger than the data it guards, and the order recorded by lockdep would include the page owner's lock under it for the whole walk.

**Change**
The distance is copied out under the lock. The guard ends, `take()` and `reclaim()` run, and a new guard covers the declaration and the arming. Free memory and the levels are read again there: `set_levels()` may have replaced the levels during the walk. The count that was walked is the one sampled before the drop. This look does not walk a second time if the new levels still say OutOfMemory. The next `look()` does, because arming fails when free memory is already outside the new bounds.

```cpp
usize frames = 0;
{
    Guard guard{m_lock};
    auto const free = approx_free_frame_count();
    auto const found = m_levels.pressure_at(free);
    if (found == MemoryPressure::OutOfMemory) {
        auto const goal = m_levels.bounds_of(found).upper;
        if (goal > free) {
            frames = goal - free;
        }
    }
}
if (frames != 0) {
    auto _ = m_reclaimer.take();
    auto const freed = m_reclaimer.reclaim(frames);
}
Guard guard{m_lock};
auto free = approx_free_frame_count();
auto found = m_levels.pressure_at(free);
```

`Guard::unlock()` does not lock again, so the two ranges are two guards, not one guard unlocked and retaken.

**Reference**
None, this project's own design. The mutex guards the levels and the declaration. The walk does not.

**Verification**
- clang-format clean. clang-tidy `-p out/bringup.x86` on `memory_watchdog.cpp` reported nothing in that file.
- `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/reclaim-one`: build ok in 1.6s. dbg PASS, shell at 5.5s, 44 passed, 4 skipped, 0 failed. rel PASS, shell at 4.7s, 41 passed, 8 skipped, 0 failed, 7/7 required. host-tests PASS, 151 passed, 0 failed.
- `python3 scripts/kshell_run.py --out out/reclaim-one pagereclaim.test mempressure.test`: pagereclaim.test ok, 18 checks; mempressure.test ok, 15 checks. OutOfMemory still gives back 48 frames and then the Critical distance of 40. The pressure checks still move Warning, Critical, OutOfMemory, and back to Normal.

**Deliberately not done**
- A second walk in the same `look()` when the levels changed and free memory is still at OutOfMemory. The next look walks.
- `set_free_memory_signal()` failing still makes `run()` call `look()` again with no bound.
- Two asks the thread has not taken still keep the larger.

**How it was found**
Reading `look()` against `Hold` and `set_levels()`. The user chose to drop the mutex for the walk on 2026-11-02.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why is the distance sampled before the walk, and the pressure recomputed after it?
- Why does this `look()` not walk again when the new levels still say OutOfMemory?
- Why are there two `Guard`s instead of one `unlock()` and a second `lock()`?

## Title: a physical VMO maps uncached and refuses write (2026-11-02)

**Files**
- `kernel/object/vma_dispatcher.cpp` — `map()` sets `MmuFlags::Discache` on a physical VMO and returns InvArgs for `PermWrite`
- `kernel/object/include/ours/object/vma_dispatcher.hpp` — the InvArgs list names a writable mapping of a physical VMO

**Symptom**
Latent until a physical VMO is mapped into a user address space. The HPET's MMIO page is one: the kernel already maps it Discache. A cached user mapping of that same physical page would return stale cache lines instead of the counter. A writable user mapping could clear `ENABLE_CNF` and stop the counter, or rewrite a comparator the kernel is using as its clock event.

**Cause**
`VmaDispatcher::map()` built `mmuf` as `User` plus `Readable`/`Writable`/`Executable` from `VmOptions`, and never asked what kind of VMO it was mapping. A paged VMO is RAM: cache it, and a store is how the program writes it. A physical VMO is a window onto memory the object does not own. Device MMIO is the case that matters. Those registers are not RAM. A cached load hits a cache line filled at some earlier read and misses the device's current value; two mappings of the same physical page, one cached and one not, is the aliasing problem, and the cached one never snoops the device. A store is worse than stale. The HPET's configuration register and comparators sit in the same page as the main counter, so a store from ring 3 can disable the counter or reprogram a comparator. `vmo_read` and `vmo_write` already answer NotSupported on a physical VMO, so a mapping is the only way a program sees those bytes, and the mapping's flags are the only thing standing between the program and the device. `map()` had no reason to refuse a write or to drop the cache, because it treated every VMO as anonymous RAM.

The type is already on the `VmObject` (`Type::Physical`). `map()` just never asked.

**Change**
After the `Perm*` bits have been copied into `mmuf`, a physical VMO forces `Discache` and refuses `PermWrite`:

```cpp
auto mmuf = mm::MmuFlags::User;
if (!!(options & VmOptions::PermRead)) {
    mmuf |= mm::MmuFlags::Readable;
}
if (!!(options & VmOptions::PermWrite)) {
    mmuf |= mm::MmuFlags::Writable;
}
if (!!(options & VmOptions::PermExecute)) {
    mmuf |= mm::MmuFlags::Executable;
}
if (vmo.vmo()->type() == mm::VmObject::Type::Physical) {
    mmuf |= mm::MmuFlags::Discache;
    if (!!(options & VmOptions::PermWrite)) {
        return ktl::err(Status::InvArgs);
    }
}
```

Ruled out: checking this in `vma_map`. A kernel caller mapping a physical VMO through the dispatcher would skip it, and the dispatcher is the one entry point. Ruled out: a cache-policy word on the VMO that mappings inherit. A physical VMO has no such word today, and every physical VMO mapped through a VMA handle is MMIO, so the type is enough. Ruled out: also refusing `CanMapWrite`, so a later `vma_protect` could not raise the mapping. `vma_protect` has not landed, and this change refuses the mapping that would be writable now.

**Reference**
Zircon `VmAspace::MapObjectInternal()` (`zircon/kernel/vm/vm_aspace.cc`) takes the VMO's cache policy (`VmObject::GetMappingCachePolicyLocked()`) into `ARCH_MMU_FLAG_UNCACHED` / `ARCH_MMU_FLAG_UNCACHED_DEVICE`. A physical VMO there can still be mapped writable; this kernel refuses write because the physical VMOs a VMA handle maps are device MMIO, and a store can stop the HPET counter or rewrite a comparator.
Linux `ioremap()` (`arch/x86/mm/ioremap.c`) maps MMIO `pgprot_noncached`; a user mapping of the same pages goes through `remap_pfn_range()` with a noncached prot (`mmap_mem` in `drivers/char/mem.c`). Linux does not refuse write on that mapping either.

**Verification**
- `python3 scripts/format_cpp.py --check` was clean on `vma_dispatcher.cpp` and `vma_dispatcher.hpp`.
- clang-tidy `-p out/clock-mono.x86` on `vma_dispatcher.cpp` reported nothing on the new lines.
- `ninja -C out/clock-mono.x86 kernel_x86.dbg.1/obj/kernel/object/object.vma_dispatcher.cpp.o` compiled. QEMU boot is the parent's; not measured here.

**Deliberately not done**
- Refusing `CanMapWrite` on a physical VMO. `vma_protect` has not landed, so a ceiling that allows write cannot be raised yet.
- Refusing `PermExecute`. Device MMIO is not a place to fetch from, but no caller asks, and this change is the cache and the store.
- A cache-policy property on the VMO. The type is enough while every physical VMO mapped through a VMA handle is MMIO.
- The kernel's own `map_phys` of the HPET. That path already passes `Discache` at the call site.
- Booting QEMU. The parent does.

**How it was found**
Specified: a physical VMO of the HPET is being mapped into a user address space so a program can sample the counter. The kernel already maps that page Discache. `map()` did not.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why must a user mapping of a physical VMO be Discache even if the caller did not ask for it?
- Why is a writable mapping of a physical VMO InvArgs, rather than a store the device would see?
- Why is this check in `VmaDispatcher::map()` rather than in `vma_map` or in `mm::VmArea`?
