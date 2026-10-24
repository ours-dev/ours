# mm changes

## A fault that widens a present entry reads it again under the object's lock (2026-10-24)

**Files**
- `kernel/mm/vm_mapping.cpp`, `kernel/mm/include/ours/mm/vm_mapping.hpp` — `widen_present_locked()` reads the entry again once it holds the object's lock, and loses its `existing_mmuf` parameter
- `kernel/commands/vmo_snapshot_test.cpp` — `check_widen_reads_entry_again()`
- `.agents/tasks/user-pager/PLAN.md` — step 4 split into four commits, the user's call

**Symptom**
Latent, cannot be triggered today. An access that needs a permission a present entry lacks (an instruction fetch through an entry without Execute) can map back a frame its object no longer has, once anything takes a frame out of a mapped object. `take_pages()` (`.agents/tasks/user-pager/` step 4) is the first thing that will.

**Cause**
The objects:

- The address space's lock, which a fault holds throughout.
- The object's lock: the VMO's, which is its page tree's.
- The page table entry, and the frame F it names.

The events, in the order that goes wrong:

1. The faulting thread, holding the address space's lock, reads the entry: present, frame F, without Execute (`arch_aspace.query()` in `VmMapping::page_fault()`). It has not taken the object's lock yet.
2. Another thread holds the object's lock and takes F out of the object. Taking a frame out unmaps it from every mapping first, so it unmaps this entry. It needs no address space lock to do that, only the object's.
3. The faulting thread gets the object's lock, and the non-write branch of `widen_present_locked()` maps F again with Execute, from what it read in step 1.

The entry now names a frame that is in another object, or free. The write branch never had the problem: it asks the object for the page (`require_owned_page()`) under the lock. The non-write branch used the frame and the Write bit it had read before the lock, as if the entry could not change while the address space's lock was held. That is true of faults, which all take that lock, but not of anything done under the object's lock alone: unmapping a frame that leaves, or a clone taking Write off the pages it is about to share (`remove_write_locked()`). The second could put back a Write the clone had just taken away, if an entry could have Write and lack Execute while its region had both, which W^X rules out today.

**Change**
Read the entry again once the object's lock is held. A frame leaves an object only under that lock and only after its entries are unmapped, and Write leaves an entry only under it too, so what the entry says under the lock is current. If it is gone or names another frame, the fault answers Ok without mapping anything, as it does for a fault some other CPU resolved, and the access retries into whatever is there now: a missing entry goes the demand path and maps the object's page.

```cpp
// before
} else {
    new_mmuf = (mmuf & ~MmuFlags::Writable) | (existing_mmuf & MmuFlags::Writable);
}
// after
} else {
    PhysAddr now = 0;
    MmuFlags now_mmuf{};
    if (Status::Ok != m_aspace->arch_aspace().query(va, &now, &now_mmuf) ||
        now != existing) {
        return {Status::Ok, 0};
    }
    new_mmuf = (mmuf & ~MmuFlags::Writable) | (now_mmuf & MmuFlags::Writable);
}
```

Rejected: asking the object for its page again, through the cursor, and mapping that. It answers the same question at a higher cost, and leaves open which Write a frame it did not read from the entry should keep. Rejected too: taking the object's lock before reading the entry, which is the order Zircon's fault path has (it looks the page up first and asks the page tables after); here it would move the lock ahead of the spurious-fault shortcut, which needs no object at all.

**Reference**
Zircon `VmMapping::PageFaultLocked()` (`kernel/vm/vm_mapping.cc`) takes the object's lock first and calls `PageFaultLockedObject()`, which looks the page up through the object's cursor before it touches the entry, so the frame it maps is always the object's current one. Ours keeps its order and reads the entry a second time instead.

**Verification**
- `vmosnapshot.test`, 223 checks pass, 5 of them new. The test holds the object's lock while another thread's instruction fetch, through an entry that lacks Execute, waits for it: the thread is parked, so it has read the entry. The test unmaps the entry and lets go. The fetch answers Ok and the entry stays unmapped; the fetch retried maps the object's page, executable.
- Control: with the second read switched off, "the fetch leaves the entry unmapped instead of mapping its frame back" fails. Unmapping an absent entry succeeds (`UnmapVisitor::on_absent()`), so the old branch mapped the frame it had read back.
- `ours-verify`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass. clang-tidy reports nothing on the lines changed.

**Deliberately not done**
The demand path, for an entry that is absent when the fault looks, already asks the object under its lock and maps what it gets, so it is unchanged. A physical object has no lock and no frame that leaves, so its branch is unchanged too.

**How it was found**
Reading the fault path while designing `take_pages()` (`.agents/tasks/user-pager/DESIGN.md`, "Found while designing"): the entry is read at `vm_mapping.cpp:645`, before the object's lock, and remapped from at `:769-776`.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
1. Which two locks does the fault hold, in which order, and which of them does an operation that takes a frame out of an object hold?
2. Why is reading the entry again under the object's lock enough, without asking the object for its page?
3. Why is answering Ok without mapping anything safe, and what happens on the retry?
