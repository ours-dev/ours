# mm changes

## Title: the PMM gets a gate for allocations that can wait, and a free-memory signal for whoever watches memory (2026-10-29)

**Files**
- `kernel/mm/include/ours/mm/gaf.hpp` — `Gaf::CanWait`
- `kernel/mm/include/ours/mm/pmm.hpp` — `try_alloc_frames()`, `wait_for_frame_allocation()`, `set_free_memory_signal()`, `refuse_delayed_allocations()`, `approx_free_frame_count()`
- `kernel/mm/pmm.cpp` — the gate, the signal, and a check of the signal after every allocation and free; the entries that answer a bare pointer or a Status assert they are not given CanWait
- `kernel/commands/memory_pressure_test.cpp`, `kernel/commands/BUILD.gn` — `mempressure.test`, new
- `docs/repairs/261029/mm.md` — new: the cost of the signal's check on every allocation and free

**Symptom**
None: a new feature, step 1 of `.agents/tasks/memory-pressure/PLAN.md`. Before it, a frame allocation that found no frame failed on the spot, whoever asked; nothing could tell a caller to wait, and nothing told anyone that memory was getting short.

**Cause**
Why each part has the shape it has, since that is the part worth learning:

- **A gate that stays shut.** A CanWait allocation that finds free memory below the delay level shuts the gate, and every later CanWait allocation answers ShouldWait, however many frames come back, until the watcher calls `set_free_memory_signal()` again. If the gate reopened by itself as soon as free memory was back above the level, the waiters would wake, take the few frames a free gave back, push free memory under the level again and shut it again: the gate would flap at the rate of frees, and the watcher, the one thing that can make memory (reclaim) or decide that none will come (D4), might never get to act. Sticky means the gate opens only after the watcher has seen the shortage.
- **The level not above the lower bound.** The watcher's event fires when free memory falls below its lower bound. With the delay level at or below that bound, free memory has crossed the bound before the gate can shut, so a shut gate means the watcher has been told. With the level above the bound the gate could shut while the watcher sleeps, and the waiters would wait for someone who is not coming. `set_free_memory_signal()` asserts it.
- **Waiters wake one at a time.** `MAY_ALLOCATE` is an auto-unsignal event: each signal wakes one waiter, which takes its frame and signals again for the next. A manual-reset event would wake every waiter on an opening, and all of them would race for the same frames, most to find the gate shut again. A waiter that finds the gate shut again does not pass the signal on: the next opening does. A refusal is passed on, so every waiter hears it.
- **CanWait is not DirectReclaim.** DirectReclaim says the caller may block inside the call; CanWait says the call returns at once with ShouldWait and the caller waits later, where it holds no lock (D2). Only `try_alloc_frames()` takes it, because only a Result can say ShouldWait: an entry answering a null pointer would have to serve the allocation past the gate or report OutOfMem, and either is wrong without the caller knowing. Those entries assert it.
- **Above PmNode.** Every allocation and free of a frame goes through `pmm.cpp`, so the gate and the signal see all of them there without touching `PmNode`, whose allocation path the unmerged `feat/pmm-cpp-vocabulary` rewrites, and the node stays without policy about who waits (D3). The price is that there is no running free count at this layer, only the zones' own counters, summed on each check: `docs/repairs/261029/mm.md` item 1.
- **Atomics on the hot side.** What every allocation and free reads (the gate's state, the level, the bounds, the event pointer) is atomics, each loaded on its own, so the allocation path takes no new lock. The bounds and the event are two loads, not one snapshot, so a CPU may compare with bounds newer than the event it loaded; that costs at most one extra signal, which only makes the watcher look again. The event that wakes waiters and the lock ordering the watcher's calls are touched only by waiters and by the watcher.
- **Signals outside the lock.** `set_free_memory_signal()` signals `MAY_ALLOCATE` after letting go of `WATCHER_LOCK`: a signal wakes a thread and takes the scheduler's lock, which should not nest inside a PMM spinlock.

**Change**
The gate's decision, in `admit_waitable()`:

```cpp
auto state = GATE.load(std::memory_order_acquire);
while (true) {
    if (state == GateState::Shut) {
        return Status::ShouldWait;
    }
    auto const level = DELAY_LEVEL.load(std::memory_order_relaxed);
    if (level == 0 || approx_free_frame_count() >= level) {
        return Status::Ok;
    }
    if (state == GateState::Refusing) {
        return Status::OutOfMem;
    }
    if (GATE.compare_exchange_weak(state, GateState::Shut, std::memory_order_acq_rel,
                                   std::memory_order_acquire)) {
        return Status::ShouldWait;
    }
}
```

The signal, after every allocation and free, one load when nothing is armed:

```cpp
if (SIGNAL_EVENT.load(std::memory_order_acquire) == nullptr) {
    return;
}
auto const free = approx_free_frame_count();
if (free >= SIGNAL_LOWER.load(std::memory_order_relaxed) &&
    free <= SIGNAL_UPPER.load(std::memory_order_relaxed)) {
    return;
}
if (auto *const event = SIGNAL_EVENT.exchange(nullptr, std::memory_order_acq_rel)) {
    event->signal();
}
```

`approx_free_frame_count()` is public because the levels must be set from the count they are compared with: it includes the frames in the per-CPU caches, which `free_frame_count()` does not, so bounds set from the exact count can be off by as much as those caches hold.

Rejected: the PMM as a PageProvider (the user's first shape, DESIGN.md); the gate inside `PmNode` (D3 (a)); a waiting `alloc_frames()` that sleeps inside the call (Linux direct reclaim), for the reason `6794ed44` removed reclaim from the allocation path.

**Reference**
Zircon: the delayed allocations of `PmmNode` (`ShouldWaitState`, `should_wait_free_pages_level_` and `may_allocate_evt_` in `zircon/kernel/vm/include/vm/pmm_node.h:359-379`), `pmm_set_free_memory_signal()` and its rule that the delay level is not above the lower bound (`zircon/kernel/vm/include/vm/pmm.h`), `PmmNode::WaitForSinglePageAllocation()` (`zircon/kernel/vm/pmm_node.cc:339`), and the free count checked on every change in `increment_free_count_locked()` and `decrement_free_count_locked()` (`zircon/kernel/vm/pmm_node.rs`). `PMM_ALLOC_FLAG_CAN_WAIT` is `Gaf::CanWait`. Differences: Zircon keeps the gate inside its one PMM node under its lock; here it is one layer up, on atomics.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed.
- `mempressure.test`: ok, 6 checks. `largepage.test` and `pagesource.test` still ok.
- Controls: `admit_waitable()` always answering Ok fails 3 checks (the ShouldWait, the waiter, the refusal); `set_free_memory_signal()` not signalling `MAY_ALLOCATE` fails the waiter check (it gives up after 5 seconds). Each restored, the test passes.
- clang-tidy: nothing in the files changed; clang-format clean.
- Not measured: the cost of the signal's check (repair item above).

**Deliberately not done**
- Nothing passes CanWait yet: the VMO side is step 2, the watcher step 3, and until then the gate is open with no level and no event, so every allocation behaves as before.
- The bounds are checked both ways on every call. Zircon checks the lower bound only when the count goes down and the upper only when it goes up; with the zones' counters summed either way, splitting the check saves a compare, not the sum.

**How it was found**
None

**The user's prediction**
None (homework paused)

**Three questions you should be able to answer**
- Why does the gate stay shut when free memory comes back above the delay level, and what would go wrong if it reopened by itself?
- Why must the delay level not be above the watcher's lower bound?
- A waiter wakes, tries, and finds the gate shut again. Why does it not signal `MAY_ALLOCATE` for the next waiter, while one that was refused does?

## Title: a user VMO's page waits at the gate through an anonymous page request (2026-10-29)

**Files**
- `kernel/mm/include/ours/mm/page_request.hpp`, `kernel/mm/page_request.cpp` — `AnonymousPageRequest`, until now an empty placeholder, and `MultiPageRequest::wait()` over both of its requests
- `kernel/mm/include/ours/mm/vm_cow_pages.hpp`, `kernel/mm/vm_cow_pages.cpp` — `alloc_pages()` takes the request's frame or raises the request; `cow_page()` takes a request; a 2 MiB block goes through the gate without waiting
- `kernel/mm/include/ours/mm/gaf.hpp` — the preset `GAF_USER_VMO`
- `kernel/object/vmo_dispatcher.cpp`, `kernel/object/pager_dispatcher.cpp` — the VMOs a process makes, and a pager's, are made with it
- `kernel/commands/memory_pressure_test.cpp` — a store, a write and a commit of a user VMO, each with the gate shut

**Symptom**
None: step 2 of `.agents/tasks/memory-pressure/PLAN.md`. Before it, the gate of step 1 had no caller: nothing passed `Gaf::CanWait`.

**Cause**
Why each part has the shape it has:

- **The wait keeps the frame.** `AnonymousPageRequest::allocate()` does not just wait for the gate to open and let the retried fault ask the PMM again: it takes a frame itself (`wait_for_frame_allocation()`) and keeps it, and `alloc_pages()` on the retry takes that frame first. Asking again could meet a gate shut again by someone else, and a waiter that loses that race every time never gets a page. Holding the frame across the retry makes each wait progress.
- **The request survives the retry.** The frame lives in the request, so the request must outlive the pass that raised it: the three loops that wait (the fault in `VmAspace::soft_fault()`, `commit_range()`, `copy_page_by_page()`) already keep their `MultiPageRequest` outside the loop, for the page source's sake. A retry that finds the page there already, put there by another thread meanwhile, does not take the frame; the request's destructor gives it back.
- **Only one of the two is raised.** A call that answers ShouldWait has raised exactly the request it needed and returned at once, so `MultiPageRequest::wait()` waits on the anonymous one when it is active and on the read request otherwise.
- **No request means refused, not served.** Two callers allocate while holding locks they cannot let go of: `take_pages_locked()`, through `cow_page()`, holds the node's lock across a whole range. They pass no request, and a ShouldWait becomes OutOfMem, as for a refused waiter. Serving them past the gate instead would hand them the frames below the delay level, the ones the gate keeps for the kernel. Zircon asserts instead that a VMO allowed to wait is always given a request (`AllocateCopyPage()`, `zircon/kernel/vm/vm_cow_pages.cc:824`).
- **A block goes through the gate.** `fill_block_locked()` allocates 512 frames at once. Past the gate it would take them from the kernel's reserve while single pages wait; so it asks with the node's flags, and when turned away returns nothing, and the hole gets one page, which waits.
- **A preset, not GAF_USER.** `GAF_USER` also allocates the kernel objects a process causes (`new (mm::GAF_USER)` for every dispatcher), which come from the heap, whose entries cannot say ShouldWait. `GAF_USER_VMO` is for VMO pages only. A clone inherits its parent's flags, so a snapshot of a user VMO waits too; the VMOs a process gets pinned or wired (`create_paged()`, `create_from_wired_pages()`) do not.
- **A mapping that commits never meets it.** `VmMapping::map()` with `MapOptions::Commit` holds the address space's lock and cannot wait; its only caller is a kernel stack, never made with `CanWait`, and a user populate maps without commit (`vm_area.cpp`). The same assumption Zircon makes, with an assertion, at `zircon/kernel/vm/vm_mapping.cc:1032`.

**Change**
`alloc_pages()`, before:

```cpp
auto frame = alloc_frames(m_gaf | Gaf::Zero, order);
if (!frame) {
    return Status::OutOfMem;
}
```

After:

```cpp
auto const gaf = m_gaf | Gaf::Zero;
PmFrame *frame = nullptr;
if (request && request->has_frame()) {
    frame = request->take_frame();
} else {
    auto result = try_alloc_frames(gaf, order);
    if (!result) {
        auto const status = result.unwrap_err();
        if (Status::ShouldWait != status) {
            return status;
        }
        if (!request) {
            return Status::OutOfMem;
        }
        request->make_active(gaf);
        return Status::ShouldWait;
    }
    frame = *result;
}
```

`MultiPageRequest::wait()`, before `return m_read.wait(suspendable);`, after:

```cpp
if (m_anonymous.is_active()) {
    return m_anonymous.allocate(suspendable);
}
return m_read.wait(suspendable);
```

**Reference**
Zircon `AnonymousPageRequest::Allocate()` (`zircon/kernel/vm/anonymous_page_request.cc`), which waits in `PmmNode::WaitForSinglePageAllocation()` and keeps the page for `take_page()`; `VmCowPages::AllocateCopyPage()` taking `request->take_page()` first (`zircon/kernel/vm/vm_cow_pages.cc:821`); user VMOs made with `ALLOC_FLAG_CAN_WAIT` (`zircon/kernel/lib/syscalls/vmo.rs:54`). Differences: Zircon panics after 30 seconds of waiting, here the wait reports every 5 seconds and goes on, since the watcher of step 3 refuses the waiters when nothing will come back; and a caller without a request is refused rather than asserted on.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed.
- `mempressure.test`: ok, 10 checks. `largepage.test`, `pagesource.test`, `pager.test`, `vmosnapshot.test`, `vmaundo.test`, `usercopy.test` ok.
- Controls: `alloc_pages()` never raising the request (every ShouldWait refused) fails the 3 VMO checks; user VMOs allocating past the gate (`CanWait` masked off) fails the same 3, since nothing sleeps. Each restored, the test passes.
- clang-tidy: nothing on the lines changed (one finding, `modernize-use-override` on `~VmCowPages()`, is on a line this did not touch); clang-format clean.

**Deliberately not done**
- A wait that never ends is reported, not ended: it ends when the gate opens or the watcher refuses (step 3). Until step 3 nothing shuts the gate, so no wait happens outside the test.
- A wait for content from a page source and a wait for a frame are never needed by the same call, so `MultiPageRequest` does not try to raise both.

**How it was found**
None

**The user's prediction**
None (homework paused)

**Three questions you should be able to answer**
- Why does the wait take a frame and keep it, instead of only waiting for the gate to open and letting the retried fault allocate?
- Why is a caller with no request refused, rather than served past the gate?
- Why can `GAF_USER` itself not carry `CanWait`?
