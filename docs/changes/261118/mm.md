# mm changes

## The gate's "outside the bounds" test is written once (2026-11-18)

**Files**
- `kernel/mm/pmm.cpp` — `FreeMemoryGate::outside(free, lower, upper)`, asked by `check_signal()` of the stored bounds and by `set()` of the bounds it is about to store

**Symptom**
From the review's minor list: the test that decides whether the watcher's event is signalled was written twice, once with `>=` and `<=` negated and once with `<` and `>`, so a change to one could miss the other.

**Cause**
One rule, two spellings. A static helper with the bounds as parameters serves both, since one site asks it of the stored bounds and the other of the new ones.

**Change**

```cpp
static auto outside(usize free, usize lower, usize upper) -> bool { return free < lower || free > upper; }
```

**Reference**
None: this kernel's own gate.

**Verification**
Recorded with the waiters' change, on the tree that holds both.

**Deliberately not done**
A bounds structure in the public `set_free_memory_signal()` (option B): it would break the rule that the public PMM interface takes primitive parameters (the user's, 2026-11-10).

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, minor item 35, decided by the user on 2026-11-17 (option A).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does `outside()` take the bounds as parameters rather than read the stored ones?
- What would a bounds structure in the public interface have cost?
- When is the watcher's event signalled, and by whom?
