# mm outstanding repairs

## 1. Every frame allocation and free sums every zone's free counter while a free-memory signal is armed

**Severity**
Low: performance only, and only once something arms the signal (the memory watchdog of `.agents/tasks/memory-pressure`, step 3). Not measured.

**Symptom**
`check_free_memory_signal()` in `kernel/mm/pmm.cpp` runs after every allocation and free of a frame. With an event armed it calls `approx_free_frame_count()`, which walks every online node and loads each zone's `m_free_frames`: about three atomic loads a node, on cache lines that every CPU's allocations and frees write. A CanWait allocation pays the same sum once more in the gate (`admit_waitable()`).

**Cause**
The gate and the signal live above `PmNode` (decision D3 of `.agents/tasks/memory-pressure/DESIGN.md`), where there is no running total of free frames, only the zones' own counters. Zircon's `PmmNode` keeps one free count under the lock its allocation path already holds, so its check costs a compare.

**Change**
Measure first, in `benches/`: the time of an order-0 allocation and free with the signal armed and not, on one CPU and on all of them at once. If the sum shows, the candidates are a free count kept beside the zones' (one more shared write a call), per-CPU deltas folded in when they pass a threshold (Linux `vm_stat` and its per-CPU `vm_stat_diff`), or checking only every Nth call; each to be put to the user with its numbers.

**Reference**
Zircon `increment_free_count_locked()` and `decrement_free_count_locked()` (`zircon/kernel/vm/pmm_node.rs`), which keep one free count under the node's lock and compare it with the upper bound on a free and the lower bound on an allocation, then `signal_free_memory_change_locked()`; Linux `zone_page_state()` and the per-CPU `vm_stat_diff` folding.

**Acceptance**
A report in `docs/perf/` with the samples of each stage, and the choice the user made.
