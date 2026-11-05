# mm changes

## Title: unpinning a page of a page source keeps it written, so the reclaimer cannot evict what a device wrote (2026-11-03)

**Files**
- `kernel/mm/vm_cow_pages.cpp` — `unpin_range_locked()` files the page with `Fill::Written`; `file_page_locked()` sends a `Written` page of a node with a page source to the anonymous queue
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp` — the `Fill::Written` enumerator, and the comments of `file_page_locked()` and `unpin_range_locked()`
- `kernel/commands/page_reclaim_test.cpp` — `check_unpin_keeps_the_write()`, three new checks in `pagereclaim.test`
- `docs/repairs/261030/mm-pin.md` — item 3 no longer says the page is reclaimable after the unpin; item 4 records the cost of this change
- `.agents/skills/kernel-design/references/mm.md` — "a pin counts as a write" next to the reclaim decisions

**Symptom**
A page of a pager-backed VMO that a device wrote while it was pinned, or that a program wrote before the pin, is evicted under memory pressure after the unpin. The next read fetches the page source's old content, so the device's data and the program's stores are gone. The path from user space is `pager_create_vmo`, supply page 0, `vma_map` it read-write, `bti_pin` it with write permission, write, `pmt_unpin`, then memory pressure; it needs a BTI, so it is a driver's path, not an unprivileged program's. `pagereclaim.test` now reproduces it without the syscalls, by calling `pin_range()` and `unpin_range()` directly.

**Cause**
**The "this page was written" fact lived in one place, the queue the page is on, and a second fact took that place.** A page of a node with a page source is on the Reclaimable queue while it is clean, and `file_dirty_locked()` moves it to the Anonymous queue when it is written. There is no per-page dirty bit. `pin_range_locked()` takes every page through `require_owned_page()` because a device may write it, and that call marks the page written (Reclaimable to Anonymous). The pin then calls `set_pinned()`, which moves the page to the Pinned queue, and the Anonymous mark is gone: from then on the page's queue says only "pinned".

At the last unpin, `file_page_locked(page, index, Fill::Content)` asked the question it asks for a page that has just arrived: how does a page of this node normally come in? For a node with a page source the answer is "as the source's content", so the page went to Reclaimable. That was wrong for a page that had been handed to a writer.

Two things then make the loss silent. First, the page table entry stays writable: the write fault that marked the page written returned `.writable = true`, and nothing takes Write away at the unpin, so a later store does not fault and the node is never told again. Second, `reclaim_page()` checks only the queue kind under the owner's lock; it does not look at a hardware dirty bit, so it evicts the page as clean. The invariant stated at `Cursor::writable_here()` ("Reclaimable means clean, and mapped read-only everywhere") is broken by the unpin.

The general lesson: a fact kept only as a side effect of another fact (here, written as a side effect of the queue) is lost the moment the other fact changes (here, the page leaves for the pinned queue). The pinned queue is a state that does not remember where the page came from.

**Change**
The unpin files the page as written, not as the source's content. `Fill` gets a third value for it.

```cpp
// before: vm_cow_pages.cpp, unpin_range_locked()
file_page_locked(page, index, Fill::Content);

// before: file_page_locked()
} else if (m_page_source) {
    queues.set_reclaimable(page, this, index);
}

// after
file_page_locked(page, index, Fill::Written);

} else if (m_page_source && Fill::Written != fill) {
    queues.set_reclaimable(page, this, index);
}
```

For a pinned node (`Flags::Pinned`) the first branch still files the page as pinned, and for a node without a page source `Written` falls through to the anonymous queue as `Content` did, so only a page of a node with a page source behaves differently.

Rejected: refusing to pin a VMO that has a page source. It would close the bug in one line, but `docs/repairs/261030/mm-pin.md` item 3 says `pin_range()` is meant to work on such a VMO. Also rejected for now: a per-page clean/dirty bit, and having `pin_range()` carry the permission so a read-only pin keeps the page clean. Both are larger changes to the page and to the pin interface; the cost of not doing them is recorded as item 4 of the same repair file.

**Reference**
Zircon `VmCowPages::MoveToNotPinnedLocked()` (`kernel/vm/vm_cow_pages.cc:4761`): for a user-pager VMO it asks `is_page_clean(page)` and sends a clean page back to the reclaim queue, any other to the dirty queue, so the page itself remembers what it was. This kernel has no such bit, so the unpin cannot tell, and the safe answer is "written". `PinRangeLocked()` (`:4022`) does not change a page's dirty state, which is the part this kernel does differently: its pin has already marked the page written.

**Verification**
- Built and booted in a clean worktree (`.worktrees/verify-h1-pin`, HEAD `49899f75` plus these three C++ files), because the main checkout's build was broken by another session's uncommitted edit of `system/public/ours/types.hpp` (`PhysAddr` and `VirtAddr` as `enum class`), which I did not touch.
- `python3 .agents/skills/verify/scripts/boot_check.py`: build ok in 13.2 s; dbg PASS, shell finished at 5.6 s, 46 passed, 5 skipped, 0 failed; rel PASS, shell finished at 4.9 s, 44 passed, 8 skipped, 0 failed, 7/7 required; host-tests PASS, 161 passed, 0 failed.
- `python3 scripts/kshell_run.py --out out/bringup.x86 pagereclaim.test vmopin.test`: `pagereclaim.test: ok -- 21 checks` (18 before, 3 new), `vmopin.test: ok -- 28 checks`.
- Fail-first: with the unpin line put back to `Fill::Content` and everything else in place, `pagereclaim.test: FAILED -- 2 of 21 checks`, the two checks "a page unpinned is written, not reclaimable" and "reclaim(100) evicts the page never pinned, and keeps what the device wrote".
- `scripts/format_cpp.py --check` is clean on the three files. `clang-tidy -p out/bringup.x86` on `vm_cow_pages.cpp` and `page_reclaim_test.cpp` reports two warnings in `vm_cow_pages.hpp` (line 496, `modernize-use-override`; line 858, `modernize-use-nodiscard`), neither on a line this change touched.
- Not verified: the user-space path (`pager_create_vmo` to `bti_pin`) end to end, since no program drives it yet; the test calls `VmObjectPaged::pin_range()` and `unpin_range()` directly. The `MEM=3072`, SMP and NUMA boots were not run, as nothing here depends on them.

**Deliberately not done**
- A read-only pin still makes the page written for good, because `pin_range()` cannot tell the two kinds of pin apart. That loses reclaimability, not data, and is item 4 of `docs/repairs/261030/mm-pin.md`.
- No change to the page table entry at unpin. A writable entry on a written (anonymous) page is correct; what was wrong was the queue.
- The case in item 3 of the same file (a pin that has to wait for a late supply) stays untested.

**How it was found**
By the independent review in `.agents/tasks/review-last-three-days/REVIEW.md` (H1), which traced `pin_range_locked()`, `file_dirty_locked()` and `reclaim_page()` by reading. Three separate checkers re-derived it from the code, each from a different angle (the code path, the entry point and its preconditions, and the design records). None ran it; this entry's test is the first run.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does a pin already count as a write in this kernel, and why does that make the unpin the place where the mark has to be kept?
- Why must a Reclaimable page be mapped read-only, and what did the unpin leave behind that broke this?
- Zircon asks the page whether it is clean at the unpin and this kernel does not: what does this kernel do instead, and what does it cost?

## Title: the page-fault spin-lock assertion reads the CPU's count with interrupts off, once (2026-11-05)

**Files**
- `kernel/mm/vm_aspace.cpp` — `VmAspace::page_fault_internal()` reads `SpinLock::num_held_cpu()` once inside an `arch::AutoInterruptDisabler` block, and the assertion tests and prints that value
- `docs/repairs/261103/kernel-task-sched.md` — item 29 deleted (numbers kept)

**Symptom**
Latent until `Scheduler::Current::steal()` (c03894ea), reachable since. A soft fault (`Thread::Current::soft_fault()`, from a user copy such as the one in `vm_object_paged.cpp`) arrives with interrupts and preemption on. If the thread is preempted inside the count read, taken by an idle CPU and resumed there, the read returns the first CPU's count. That is non-zero whenever the first CPU is inside a spin lock or an interrupt handler, and the `ASSERT` stops the kernel with "resolved with N spin lock(s) held on this CPU" in a release image too (it is compiled in on purpose). Found by a reviewer's reading of the code; not reproduced.

**Cause**
**A per-CPU value is a fact about one CPU at one instant, and the code read it in two steps with nothing keeping the thread on that CPU between them.** `SpinLock::num_held_cpu()` is `SP_SPINLOCK_NUM_HELD.get()`, which is `CpuLocal::with_current(slot, ...)`: first `current_cpu()` (one load through `%gs`), then `access(slot, cpu)` (the slot's address for *that* CPU number). The result is "CPU A's count, loaded by a thread that is now on CPU B".

Before work stealing this was safe without anyone having to think about it: a preempted thread always resumed on the CPU it left, so both steps named the same CPU. `steal()` removed that guarantee without changing a line of this function, so every unprotected per-CPU read made from preemptible context became a latent bug at once. The hardware-fault entry is not affected: the exception gate clears IF, so `x86_handle_page_fault()` and `may_sleep_resolving()` read the count with interrupts off.

The message had a second copy of the problem. `ASSERT` evaluates its message arguments only after the condition failed (`if (!(condition)) { panic_assertion(#condition, fmt, args...); }`), so the old second `num_held_cpu()` in the message was a *separate* read, made later, that could print a different number from the one that failed the test, or on a different CPU.

**Change**
```cpp
// before
ASSERT(sync::SpinLock::num_held_cpu() == 0,
       "page fault at {:#x} resolved with {} spin lock(s) held on this CPU", va,
       sync::SpinLock::num_held_cpu());
ASSERT(!arch::irqs_disabled(), "page fault at {:#x} resolved with interrupts disabled", va);

// after
usize num_spin_locks = 0;
{
    arch::AutoInterruptDisabler irq;
    num_spin_locks = sync::SpinLock::num_held_cpu();
}
ASSERT(num_spin_locks == 0, "page fault at {:#x} resolved with {} spin lock(s) held on this CPU",
       va, num_spin_locks);
ASSERT(!arch::irqs_disabled(), "page fault at {:#x} resolved with interrupts disabled", va);
```
The disabler is in a block of its own because the next assertion asks whether the caller had interrupts off, and it has to see them as the caller left them. A thread with interrupts off cannot be preempted (preemption arrives on the tick or an IPI), so the two steps of the read now happen on one CPU. The disabler saves and restores the flag, so the hardware path, which already has interrupts off, is unchanged.

Rejected: making `Counter::get()` itself safe. Every other `get()` caller either holds interrupts off or tolerates a stale value (a statistic); a disabler in the counter would add a flags save and restore to each of those for the sake of this one assertion. The check is the caller's to place, because only the caller knows it is asking about "this CPU".

**Reference**
None, this project's own design. The same remedy is used by `TimerQueue::activate_timer()` and by `check_local_fallback()` in `pm_node_test.cpp`.

**Verification**
- `prebuilt/ninja/ninja -C out/mm-percpu.x86` and `out/mm-percpu-numa.x86` (`mm_numa=true`) build.
- `python3 scripts/kshell_run.py --out out/mm-percpu.x86 vmosnapshot.test usercopy.test pagequeues.test pmnode.test`: all four `ok`, exit 0. `usercopy.test` and `vmosnapshot.test` take the soft-fault path.
- `python3 .agents/skills/verify/scripts/boot_check.py --out out/mm-percpu.x86`: dbg PASS (46 passed, 5 skipped, 0 failed), rel PASS (44 passed, 8 skipped, 0 failed, 7/7 required), host-tests PASS (173 passed, 0 failed).
- `clang-tidy -p out/mm-percpu.x86 kernel/mm/vm_aspace.cpp`: nothing reported on the changed lines.
- Not reproduced: the race itself. It needs a preempt, a steal and a lock-holding first CPU inside a window of a few instructions; the change is justified by the reading, and a stress that would show it is not written.

**Deliberately not done**
- `Counter::get()` and `CpuLocal::with_current()` are not changed (see Change).
- The remaining unprotected `num_held_cpu()` readers were looked at, not changed: `irq_test.cpp` reads it inside its handler (interrupts off), the lockup report reads it from the NMI path, and `fault.cpp` reads it with the exception's IF clear.

**How it was found**
A two-round independent review of c03894ea listed the per-CPU reads made from preemptible context; this one was confirmed here by reading `Counter::get()`, `CpuLocal::with_current()` and `ASSERT`.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why was `num_held_cpu()` safe from a preemptible soft fault before `steal()` and not after, with no line of it changed?
- Why must the disabler's block end before `ASSERT(!arch::irqs_disabled())`?
- Why does the old message's second `num_held_cpu()` not necessarily print the number that failed the test?

## Title: pmnode.test reads the CPU and its node with interrupts off (2026-11-05)

**Files**
- `kernel/commands/pm_node_test.cpp` — `run_topology_checks()` reads `current_cpu()` and `mm::current_node()` in one `arch::AutoInterruptDisabler` block
- `docs/repairs/261103/kernel-task-sched.md` — item 32 deleted (numbers kept)

**Symptom**
On a multi-node machine the case "current_node() is the placement table's answer for the calling CPU" could fail with no bug in the kernel. The kshell thread is not pinned; if an idle CPU on another node took it between the two reads, `node_of(cpu)` was the first CPU's node and `nid` the second's. Invisible on one node (every CPU maps to node 0). The review's reading; on `NUMA=1` the test passed in 3 of 3 runs after `steal()` was added.

**Cause**
The test assumed the thread stays on its CPU between two statements. The check is a comparison of two facts about the same CPU, so both have to be read at one instant. `check_local_fallback()` in the same file already did this (disabler, then `current_node()`), which is the pattern the topology check missed.

**Change**
```cpp
// before
auto const cpu = mm::CpuLocal::current_cpu();
auto const nid = mm::current_node();

// after
auto cpu = CpuNum{};
auto nid = NodeId{};
{
    arch::AutoInterruptDisabler irq;
    cpu = mm::CpuLocal::current_cpu();
    nid = mm::current_node();
}
```
The other `mm::current_node()` calls in the file (`check_static_allocation`, `check_batch_errors`, `check_node_queries`, `check_remote_exhaustion`, `get_frames_n` and the disabled-allocation checks) were read one by one and left alone: each takes one node number and passes it on explicitly, or asks for the implicit home without comparing the result with a CPU's node, so a move between statements changes which node is exercised but not whether the check is right.

Rejected: pinning the kshell thread with `set_cpu_affinity_hard()`. It would change the context every check runs in, and `check_remote_exhaustion` is documented to run from an idle guest.

**Reference**
None, this project's own test. Pattern: `check_local_fallback()` in the same file.

**Verification**
- `NUMA=1 python3 scripts/kshell_run.py --out out/mm-percpu-numa.x86 pmnode.test` (`mm_numa=true`): `pmnode.test: ok -- 77 checks`.
- The same command 20 times in a row (a fresh boot each): 20 of 20 `ok -- 77 checks`, no FAIL line. This is item 32's acceptance; it does not show the fix, because the failure was not seen before it either.
- On `out/mm-percpu.x86` (one node): `pmnode.test: ok -- 38 checks`.

**Deliberately not done**
- The failure was not reproduced before the change; the window is a few instructions wide.

**How it was found**
The same review as the previous entry; the reviewer noted the test's unpinned read and `check_local_fallback()`'s disabler next to it.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why can a one-node machine never show this failure?
- What two facts does the case compare, and why must they be read in one disabled region?
- Why were the other `current_node()` reads in the file not changed?
