# benches changes

## Title: page-walk-bench drives x86's CRTP page-table walks instead of the paging engine (2026-10-26)

**Files**
- `benches/kernel/lib/arch/page_walk_bench.cpp` — a `BenchTable` derived from `page_tables::X86PageTableImpl`, whose `table()` hook counts the tables a walk enters; the wrappers `table_map()`, `table_map_frames()`, `table_protect()`, `table_unmap()` in place of the engine's
- `benches/kernel/lib/arch/BUILD.gn` — depends on `//kernel/arch/x86/lib/page_tables`

**Symptom**
None. The kernel's x86 page table stopped using the paging engine (`.agents/tasks/x86-page-table-crtp`, steps 2 to 5), so a bench of the engine no longer measured the kernel's walks.

**Cause**
The bench is the instrument of `docs/perf/261025/page-walk.md`; its stage 3 measures the CRTP rewrite against stage 2, which needs the same cases run on the new code.

**Change**
The same cases, samples and wrappers, on a derived class instead of the engine:

```cpp
class BenchTable final: public page_tables::X86PageTableImpl<BenchTable> {
    ...
    auto table(PhysAddr const pa) const -> arch::PteVal * {
        *m_visits += 1;
        return reinterpret_cast<arch::PteVal *>(pa);
    }
    static constexpr auto splits_large_leaves() -> bool { return true; }
    ...
};
```

Visits are counted where the walk enters a table, as `CountingPhysToVirt` counted them for the engine. The table splits its large leaves, as the engine was given `CountedDeposits`, and its deposits are counted, not kept, as before. Stage 2's bench is built from `ba95bbba` when the two are compared.

**Reference**
None, this project's own tool.

**Verification**
- Builds; the visits match stage 2's in every case (4 a one-page call, the floor of tables for every range).
- Stage 3 of `docs/perf/261025/page-walk.md` was recorded with it.

**Deliberately not done**
- The engine's own bench: the engine now serves only phys, and nothing measures it.

**How it was found**
None.

**The user's prediction**
None: homework is paused (2026-10-21).

**Three questions you should be able to answer**
- Why is a visit counted in the `table()` hook, and why is that the same count the engine bench made?
- Why does the bench's table split its large leaves?
- What would the bench miss if `table()` were not inlined into the walks?
