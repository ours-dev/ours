# mm outstanding repairs

## 1. Every allocation that names no node takes the thread's resource spin lock to read its placement policy

**Severity**
Low, performance, unmeasured: no wrong result, a cost on the hottest allocation path.

**Symptom**
Every frame allocation that names no node (`mm::alloc_frames()` with `home = ANY_NODE`, which is nearly every caller) runs `thread_policy()` in `kernel/mm/pmm.cpp` once per call: it saves and disables interrupts, takes the calling thread's `m_resource_lock` (`sync::SpinLock`, `IrqSave`), copies the `ustl::Arc<AllocPolicy>` out of `AllocState`, and releases the lock. Today the Arc is always null, since nothing can set a policy, so the copy is a null check; the lock and the interrupt save and restore are paid for nothing.

**Cause**
The rule is that a thread's policy may be replaced only under this lock (`Thread::m_resource_lock`), so reading the Arc under it is what makes the copy safe against a replacement. No replacement exists yet: `AllocState` has no setter, and who may set a policy, the thread itself or another thread too, is not designed. It is a spin lock with interrupts off because one of its readers, this path, is reached by the page fault path with interrupts already off. Nothing on the read side needs more than an atomic load of one pointer and a retain, and the common case, no policy at all, needs only the load.

**Change**
Measure first, as CLAUDE.md asks of performance work: a `benches/` benchmark of `alloc_frames()` plus `free_frames()` of order 0 from one CPU, run with the lock as it is and with the read removed (a control, not a fix), giving the share of each allocation the read costs. Candidates once the cost is known, each the user's to choose: (a) an atomic pointer to the policy, replaced with a release store and read with an acquire load, with the old policy's release deferred until no reader can hold it (RCU, or a per-thread epoch); (b) a per-thread flag word bit "has a policy" read without the lock, taking the lock only when it is set, since a thread with no policy is the common case; (c) only the thread itself may replace its policy, as in Linux, so its own allocations read it with no lock and the lock guards only readers on other threads; (d) leave it, if the measurement says the cost is noise next to the zone lock. (c) is decided by the setter's design, so it belongs with that decision. Ruled out only if other threads may replace a policy: reading the Arc without the lock, which then races with a replacement that drops the last reference.

**Reference**
Linux reads `current->mempolicy` without a lock on the allocation path (`get_task_policy()` in `mm/mempolicy.c`): the task's own policy is replaced only by the task itself (`do_set_mempolicy()` takes `task_lock()` against readers on other tasks), so its own allocations need no lock to read it.

**Acceptance**
A report in `docs/perf/<YYMMDD>/` with the baseline and each chosen stage's samples, and, if a candidate is taken, its change record.
