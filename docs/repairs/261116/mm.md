# mm outstanding repairs

## 1. Every allocation that names no node takes the thread's resource spin lock to read its placement policy

**Severity**
Low, performance, unmeasured: no wrong result, a cost on the hottest allocation path.

**Symptom**
Every frame allocation that names no node (`mm::alloc_frames()` with `home = ANY_NODE`, which is nearly every caller) runs `thread_policy()` in `kernel/mm/pmm.cpp` once per call: it saves and disables interrupts, takes the calling thread's `m_resource_lock` (`sync::SpinLock`, `IrqSave`), copies the `ustl::Arc<AllocPolicy>` out of `AllocState`, and releases the lock. For a thread that has set no policy, which is every thread but a test's today, the copy is a null check, and the lock and the interrupt save and restore are paid for nothing.

**Cause**
The rule is that a thread's policy may be replaced only under this lock (`Thread::m_resource_lock`), so reading the Arc under it is what makes the copy safe against a replacement. The replacement is `mm::set_thread_policy()`, and only the calling thread can make it (decided 2026-11-16): a thread's own allocations can therefore never race a replacement, and the lock protects only readers on other threads, of which there are none. It is a spin lock with interrupts off because one of its readers, this path, is reached by the page fault path with interrupts already off. Nothing on the read side needs more than an atomic load of one pointer and a retain, and the common case, no policy at all, needs only the load.

**Change**
Measure first, as CLAUDE.md asks of performance work: a `benches/` benchmark of `alloc_frames()` plus `free_frames()` of order 0 from one CPU, run with the lock as it is and with the read removed (a control, not a fix), giving the share of each allocation the read costs. Candidates once the cost is known, each the user's to choose: (a) an atomic pointer to the policy, replaced with a release store and read with an acquire load, with the old policy's release deferred until no reader can hold it (RCU, or a per-thread epoch); (b) a per-thread flag word bit "has a policy" read without the lock, taking the lock only when it is set, since a thread with no policy is the common case; (c) read the Arc with no lock on the thread's own allocations, which the self-only setter makes safe, keeping the lock for readers on other threads; one caveat, an allocation from interrupt context taken in the middle of `set_thread_policy()`'s swap, is closed by the swap running with interrupts off, as it does; (d) leave it, if the measurement says the cost is noise next to the zone lock. Ruled out: letting another thread replace a policy, which would make (c) race with a replacement that drops the last reference.

**Reference**
Linux reads `current->mempolicy` without a lock on the allocation path (`get_task_policy()` in `mm/mempolicy.c`): the task's own policy is replaced only by the task itself (`do_set_mempolicy()` takes `task_lock()` against readers on other tasks), so its own allocations need no lock to read it.

**Acceptance**
A report in `docs/perf/<YYMMDD>/` with the baseline and each chosen stage's samples, and, if a candidate is taken, its change record.

## 2. A contiguous run gives its surplus back one frame and one pressure observation at a time

**Severity**
Low, performance, unmeasured: no wrong result.

**Symptom**
`alloc_frames_contiguous()` in `kernel/mm/pmm.cpp` takes a block of `2^order` frames for a run of `n`, splits it to single frames and frees the `2^order - n` it does not need with one `free_frames()` call each. Each call takes the zone lock once and observes the pressure once (`observe_pressure()`, which re-sums every zone's free count when the watcher's signal is armed). A run of 17 frames gives back 15 frames that way, one of 257 gives back 255.

**Cause**
The surplus is freed through the per-frame public entry, which is right for a block handed out, while the frames being given back are one contiguous range the allocator still owns as a unit. The observation rule is "once per entry call", so a loop over the entry multiplies it.

**Change**
Measure first, as CLAUDE.md asks of performance work: a `benches/` benchmark of `alloc_frames_contiguous()` plus freeing the run, with `n` just above a power of two (17, 257) and just below (15, 255), with the signal armed and not, giving cycles per call and zone-lock acquisitions per call. Candidates once the cost is known, each the user's to choose: (a) give the surplus back as one `PmFrame::List` through `free_frames(PmFrame::List *)`, one observation; (b) give it back as the largest aligned blocks it splits into, through the zone's adoption (`PmZone::adopt()` carves a range into aligned blocks already), fewer locks and fewer buddy merges; (c) leave it, if the measurement says the cost is noise.

**Reference**
Linux `alloc_pages_exact()` in `mm/page_alloc.c` frees the tail with `make_alloc_exact()`, one `free_page()` per page, the same shape as today; `free_pages_exact()` frees a run page by page too.

**Acceptance**
A report in `docs/perf/<YYMMDD>/` with the baseline and each chosen stage's samples, and, if a candidate is taken, its change record.
