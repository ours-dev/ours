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
