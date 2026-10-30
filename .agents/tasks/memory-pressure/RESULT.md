# memory-pressure: a user VMO's page waits at a gate in the PMM, and a watchdog thread refuses it once memory is out, keeping the last frames for the kernel

**Date**: 2026-10-29

## Delivered
A frame allocation that can wait (`Gaf::CanWait`) answers ShouldWait while free memory is short, and the fault, commit or copy that asked lets go of its locks, waits for a frame through an `AnonymousPageRequest`, and retries with it. The pages of the VMOs a process makes are allocated that way. A memory watchdog thread watches free memory through the PMM's free-memory signal, declares one of four pressures, and once it declares OutOfMemory refuses the allocations that could wait, so that the frames below that level stay the kernel's. The design is `docs/design/261029/memory-pressure.md`; the plan is `PLAN.md` here.

- `411df8c2` docs(task): the design, proposed, after the user chose Zircon's shape over the PMM as a PageProvider.
- `78a4dbd0` docs(task): D1 to D4 decided as recommended, the plan in three steps.
- `693260d0` feat(mm): step 1, the PMM's gate in `pmm.cpp` (`try_alloc_frames()`, `wait_for_frame_allocation()`, `set_free_memory_signal()`, `approx_free_frame_count()`) and `mempressure.test`. New repair: `docs/repairs/261029/mm.md` item 1.
- `ef49ea8f` feat(mm): step 2, `AnonymousPageRequest`, `MultiPageRequest::wait()` over both requests, `VmCowPages::alloc_pages()` through the request, and `GAF_USER_VMO` for `vmo_create` and a pager's VMO.
- `19a0eb01` feat(mm): step 3, `MemoryWatchdog` and the `kernel.oom.*` boot options; `set_free_memory_signal()` takes Wait or Refuse.
- This commit: this document, and the design moved to `docs/design/261029/memory-pressure.md`.
- Records: `docs/changes/261029/mm.md`, three entries.

## Verification
At the final state (`19a0eb01`):

- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed. Both images log `mm: memory pressure Normal, 491 MiB free` at boot.
- `mempressure.test` on the dbg image: ok, 15 checks. The gate (served with no level; ShouldWait below it while an allocation without CanWait is served; a waiter woken with a frame when it opens; waiters and allocations refused with OutOfMem; the signal on crossing either bound); a user VMO (a store through a mapping, a write and a commit each sleep with the gate shut and finish once it opens, with the bytes in place); the watchdog (Normal with the boot levels; Warning, Critical and back as levels and free memory move; at OutOfMemory a CanWait allocation refused and a plain one served; Normal again with the boot levels).
- `largepage.test`, `pagesource.test`, `pager.test`, `vmosnapshot.test`, `vmaundo.test`, `usercopy.test` pass.
- Controls, each restored after: the gate never shutting (3 checks fail); opening never waking a waiter (1); `alloc_pages()` never raising the request (3); user VMOs allocating past the gate (3); the watchdog arming Wait at OutOfMemory (the kernel panics on `set_free_memory_signal()`'s assertion that the delay level is not above the lower bound with Wait); the watchdog refusing with a level of 0 (1); the watchdog arming no event (1).
- clang-tidy: nothing on the lines changed; clang-format clean on them.
- Not measured: the cost of checking the signal after every allocation and free, paid from boot now (`docs/repairs/261029/mm.md` item 1). Not verifiable here: whether waiting helps under real pressure, since nothing frees memory yet except a process that exits.

## Deviations from the design
- **`approx_free_frame_count()` is public.** The levels must be set from the count the gate compares with, which includes the frames in the per-CPU caches; `free_frame_count()` does not, and takes every zone's lock.
- **`refuse_delayed_allocations()` became an argument.** At OutOfMemory the watchdog arms the signal and refuses in one step (`DelayedAllocations::Refuse`). As two calls, the first would have armed Wait with a level above its lower bound of 0, which the assertion forbids for a good reason: a shut gate nobody will open.
- **Four pressures.** ImminentOutOfMemory, which tells user space to take diagnostics, has no reader here and was left out (the user, 2026-10-29).
- **A preset, `GAF_USER_VMO`.** The design said VMOs made with a may-wait flag; `GAF_USER` itself could not carry it, since the kernel objects a process causes are allocated from the heap with it, and the heap cannot say ShouldWait.
- **A caller without a request is refused, not asserted on.** `take_pages_locked()` holds the node's lock across a range and cannot wait; Zircon asserts a request is always there for a VMO that can wait.
- **A 2 MiB block asks at the gate and falls back to one page**, which then waits; past the gate it would take 512 frames of the kernel's reserve.
- **The hysteresis holds back the declared pressure only**; the gate follows the free memory found.
- **Test seams**: `MemoryWatchdog::set_levels()` (the user's choice over a low-memory image) and `MemoryWatchdog::Hold`, which keeps the watchdog off the gate while the gate's own checks drive it.
- **A wait that never ends is reported every 5 seconds and goes on.** Zircon panics after 30 seconds; here the watchdog refuses the waiters at OutOfMemory.

## Found along the way
- **Zircon's PMM node is partly Rust now**: the free count and the signal's bounds are checked in `increment_free_count_locked()` and `decrement_free_count_locked()` (`zircon/kernel/vm/pmm_node.rs`), which the repair item first named wrongly as C++ functions; corrected before it was committed.
- **Global constructors run before any frame is allocated** (`ccrt::init_global_static_objects()` is the first thing `start_kernel()` does, `kernel/kernel/time/clock.cpp`). A comment of step 1 first said the opposite, as the reason for the gate's atomics; corrected before it was committed. The atomics stay, for a lock-free allocation path.
- **The populate path cannot wait**: `VmMapping::map()` with Commit holds the address space's lock. Its one caller is a kernel stack, and a user populate maps without Commit, so it never meets a VMO that can wait; Zircon makes the same assumption with an assertion (`zircon/kernel/vm/vm_mapping.cc:1032`).

## Not done
- **Reclaim**: eviction, compression and compaction. Nothing can be reclaimed until page aging runs (`PageQueues::start_runners()` is empty, `docs/repairs/261026/mm.md` item 9). The watchdog is where they would be woken.
- **The cost of the signal's check**: `docs/repairs/261029/mm.md` item 1, to be measured and put to the user.
- **Pressure events for user space, and killing a process at OutOfMemory** (D4 (b)): with a component that acts on them.
- **Levels for machines larger than the dev image's 512 MiB**: the defaults are Zircon's absolute ones, overridable by boot option; an open question of the design.
