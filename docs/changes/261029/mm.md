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

## Title: a memory watchdog thread turns free memory into a pressure, and refuses the allocations that could wait once memory is out (2026-10-29)

**Files**
- `kernel/mm/include/ours/mm/memory_watchdog.hpp`, `kernel/mm/memory_watchdog.cpp`, `kernel/mm/BUILD.gn` — new: `MemoryWatchdog`, its thread, started at `InitLevel::Threading + 1`
- `kernel/include/ours/boot/options.hpp`, `kernel/top/boot_options.cpp` — `kernel.oom.outofmemory-mb`, `critical-mb`, `warning-mb`, `debounce-mb`, `hysteresis-seconds`
- `kernel/mm/include/ours/mm/pmm.hpp`, `kernel/mm/pmm.cpp` — `set_free_memory_signal()` takes what a delayed allocation gets (`DelayedAllocations::Wait` or `Refuse`); `refuse_delayed_allocations()` is gone into it
- `kernel/commands/memory_pressure_test.cpp` — the watchdog's checks; the checks of the gate run under `MemoryWatchdog::Hold`
- `docs/repairs/261029/mm.md` — item 1's cost is now paid from boot

**Symptom**
None: step 3 of `.agents/tasks/memory-pressure/PLAN.md`. Before it, nothing set the gate's level: steps 1 and 2 worked only when a test set it by hand.

**Cause**
Why each part has the shape it has:

- **Four pressures.** Normal, Warning, Critical, OutOfMemory, each beginning below a level of free memory. Zircon has a fifth, ImminentOutOfMemory, which tells user space to take diagnostics before it is too late; nothing here would read it, so it was left out (the user, 2026-10-29).
- **Debounce.** The bounds the watchdog arms are its pressure's range widened by the debounce on both sides. Free memory that moves to and fro across a level by a few frames, as every allocation and free makes it, then never wakes the watchdog; it wakes only once free memory has really gone past.
- **Hysteresis, for the declared pressure only.** A higher pressure is declared at once, a lower one only once the hysteresis has passed since the last change, so that a load that comes and goes is not logged as a change each time. What the PMM is armed with follows the free memory actually found: the gate opens as soon as memory is back, and only the log waits.
- **The delay level is the lower bound at Critical.** The level sits the debounce below the OutOfMemory level, which is exactly where Critical's bounds end. So the one allocation that takes free memory past it both shuts the gate and signals the watchdog, which then declares OutOfMemory and arms the PMM with Refuse: the waiters wake and are refused (D4 (a)), and the frames below the level are left to the kernel's own allocations. Nothing waits for a reclaim that cannot come.
- **Refuse as an argument, not a second call.** At OutOfMemory the watchdog arms the signal (to hear when memory is back) and refuses in one step. As two calls, `set_free_memory_signal()` would first have opened the gate in Wait with a level above its lower bound of 0, which its own assertion forbids, since a shut gate could then wait for a watcher who is not coming. With the mode an argument, the assertion stays and says exactly that: with Wait the level is not above the lower bound; with Refuse nobody waits.
- **A Hold for tests.** The gate's own checks set it by hand, which disarms the watchdog's event. `MemoryWatchdog::Hold` takes the lock the watchdog arms under, so it cannot arm meanwhile, and when the Hold goes it wakes the watchdog to arm again. `set_levels()` moves the levels to where free memory is, which reaches every pressure with a few hundred frames instead of eating 440 MiB (the user, 2026-10-29).

**Change**
The watchdog's turn, shortened:

```cpp
auto const found = pressure_for(levels, approx_free_frame_count());
auto declare = first || SKIP_HYSTERESIS || found > declared;
if (!declare && found < declared) {
    if (now - declared_at >= levels.hysteresis) {
        declare = true;
    } else {
        deadline = time::Deadline::at(declared_at + levels.hysteresis);
    }
}
// ... log and publish |found| if declared ...
auto delayed = DelayedAllocations::Wait;
if (found == MemoryPressure::OutOfMemory) {
    delayed = DelayedAllocations::Refuse;
}
auto const bounds = bounds_for(levels, found);
if (!set_free_memory_signal(bounds.lower, bounds.upper,
                            levels.out_of_memory - levels.debounce, &SIGNAL, delayed)) {
    continue;
}
// ... then, the lock let go:
auto _ = SIGNAL.wait(deadline);
```

The bounds, for levels `oom < critical < warning` and debounce `d`: Normal `[warning - d, ∞)`, Warning `[critical - d, warning + d]`, Critical `[oom - d, critical + d]`, OutOfMemory `[0, oom + d]`.

**Reference**
Zircon `MemoryWatchdog` (`zircon/kernel/object/memory_watchdog.cc`): `WorkerThread()`, `WaitForMemChange()` with `FreeMemBoundsForLevel()` and the delay level `mem_watermarks_[kOutOfMemory] - watermark_debounce_`, the hysteresis in `IsSignalDue()`, and `OnOom()`; the boot options `kernel.oom.*` and their defaults (`zircon/kernel/lib/boot-options/include/lib/boot-options/options.inc`, 50, 150, 300 and 1 MiB, 10 seconds). Differences: no ImminentOutOfMemory, no eviction, no events for user space; at OutOfMemory the waiters are refused where Zircon reboots or kills a job (`OomBehavior`); and the hysteresis holds back only the declared pressure, where Zircon's also holds back re-arming.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed. Both images log `mm: memory pressure Normal, 491 MiB free` at boot.
- `mempressure.test`: ok, 15 checks, the transitions in the log: Warning, Critical, Warning, OutOfMemory with its warning, Normal. `largepage.test`, `pagesource.test`, `pager.test`, `vmosnapshot.test`, `vmaundo.test`, `usercopy.test` ok.
- Controls: arming Wait at OutOfMemory panics on `set_free_memory_signal()`'s assertion ("a delay level 125800 above the lower bound 0"), the invariant the mode argument keeps; arming Refuse with a level of 0, which refuses nothing, fails the OutOfMemory check; arming no event, so that only `set_levels()` wakes the watchdog, fails the Critical check. Each restored, the test passes.
- clang-tidy: nothing in the files changed; clang-format clean.
- Not measured: the cost of the signal's check, now paid by every allocation and free from boot (`docs/repairs/261029/mm.md` item 1).

**Deliberately not done**
- Eviction, compression and compaction: nothing to reclaim yet. The watchdog is where they would be woken.
- Pressure events for user space, and killing a process at OutOfMemory (D4 (b)): later, with a component that acts on them.
- The thread runs at the default priority. Under pressure it has one thing to do per crossing, and the waiters it refuses are the ones it would compete with.

**How it was found**
None

**The user's prediction**
None (homework paused)

**Three questions you should be able to answer**
- Why is the delay level put exactly the debounce below the OutOfMemory level, and what happens in the one allocation that crosses it?
- What would go wrong without the debounce, with free memory sitting right at a level?
- Why does the hysteresis hold back the declared pressure but not the gate?

## Title: the memory watchdog is an object that owns its state, not a class of statics over file-scope globals (2026-10-29)

**Files**
- `kernel/mm/include/ours/mm/memory_watchdog.hpp`, `kernel/mm/memory_watchdog.cpp` — `MemoryWatchdog` holds its event, lock, levels and declared pressure as members, and its thread is `run()`; the questions about the levels are methods of `MemoryPressureLevels`
- `kernel/commands/memory_pressure_test.cpp` — reaches the watchdog through `MemoryWatchdog::get()`

**Symptom**
No change in behaviour. The user asked, 2026-10-29, why the watchdog was a class of static functions over a lock, an event, the levels and the declared pressure kept as globals in an anonymous namespace, with its thread and its two pure helpers as free functions.

**Cause**
It broke the rule of `modern-cpp-development` ("Model with objects ... Reach for a free function only when there is no state to guard"): the watchdog has state and an invariant over it (the levels and the declared pressure change only under the lock, and only the thread declares), and no object owned them. Two things looked like they forced free functions, and only one does: `Thread::spawn()` and `GKTL_INIT_HOOK` take plain function pointers. A function pointer is no reason to scatter the state: a lambda that captures nothing converts to one, takes the object as its `void *` argument, and calls the member. The rest copied the shape of the gate in `pmm.cpp`, which has the same fault (the next change).

Being an object pays back in checking: members can say `TA_GUARDED(m_lock)`, which `-Wthread-safety -Werror` checks on every access; the globals said nothing, so the analysis checked nothing. And `Hold` is now a scoped capability (`TA_SCOPED_CAPABILITY`, acquiring the watchdog's lock in its constructor and releasing it in its destructor), where it used to switch the analysis off.

**Change**
Before, the state:

```cpp
namespace {
    sync::Event SIGNAL{sync::Event::Options::AutoUnsignal};
    sync::Mutex LOCK;
    MemoryPressureLevels LEVELS{};
    bool SKIP_HYSTERESIS = false;
    std::atomic<MemoryPressure> PRESSURE{MemoryPressure::Normal};
    auto watchdog_routine(void *) -> i32 { /* declared, declared_at, first as locals */ }
}
```

After:

```cpp
class MemoryWatchdog {
    // ...
  private:
    auto run() -> i32;
    auto look() -> ustl::Option<time::Deadline>;

    sync::Event m_signal{sync::Event::Options::AutoUnsignal};
    mutable sync::Mutex m_lock;
    MemoryPressureLevels m_levels TA_GUARDED(m_lock){};
    bool m_skip_hysteresis TA_GUARDED(m_lock) = false;
    std::atomic<MemoryPressure> m_pressure{MemoryPressure::Normal};
    MemoryPressure m_declared TA_GUARDED(m_lock) = MemoryPressure::Normal;
    time::SteadyClock::TimePoint m_declared_at TA_GUARDED(m_lock){};
    bool m_declared_once TA_GUARDED(m_lock) = false;
};

auto *const thread = task::Thread::spawn(
    "memory-watchdog", [](void *const self) -> i32 { return static_cast<Self *>(self)->run(); },
    this, task::DEFAULT_PRIORITY);
```

`pressure_for(levels, free)` and `bounds_for(levels, pressure)` became `MemoryPressureLevels::pressure_at()` and `bounds_of()`, with `delay_level()` beside them, and `boot_levels()` became `MemoryPressureLevels::from_boot_options()`: they ask the levels, and hold nothing. The one instance is a function-local static in `get()`, as `SparseMemMap::get()` has it. The init hook stays a free function, for the same reason as the thread's entry, and only calls `MemoryWatchdog::get().start()`.

**Reference**
Zircon's `MemoryWatchdog` (`zircon/kernel/object/memory_watchdog.cc`) is a class whose members hold the same state (`mem_watermarks_`, `mem_event_idx_`, `hysteresis_seconds_`), with `WorkerThread()` a member.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed.
- `mempressure.test` ok, 15 checks, with the same transitions in the log; `largepage.test` and `pagesource.test` ok.
- The two files compile with `-Wthread-safety -Werror` (from the compile database) with no warning; clang-tidy reports nothing in them; clang-format clean.

**Deliberately not done**
- The gate in `pmm.cpp`: the next change.

**How it was found**
The user's review of step 3, 2026-10-29.

**The user's prediction**
None

**Three questions you should be able to answer**
- What did a plain function pointer actually force, and how does a lambda that captures nothing get from it back to the object?
- Which checks did the globals lose that the members get back?
- Why are `pressure_at()` and `bounds_of()` methods of the levels and not of the watchdog?

## Title: the PMM's gate for allocations that can wait is an object, FreeMemoryGate, behind the same free functions (2026-10-29)

**Files**
- `kernel/mm/pmm.cpp` — the gate's state, its free-memory signal and their lock are members of `FreeMemoryGate`, one instance in the file; `try_alloc_frames()`, `wait_for_frame_allocation()`, `set_free_memory_signal()` and every allocation and free entry ask it

**Symptom**
No change in behaviour. The same review as the change before: the gate of step 1 kept its state (the state, the delay level, the bounds, the watcher's event, the waiters' event, the watcher's lock) as globals in an anonymous namespace, with the functions that guard it beside them.

**Cause**
The same rule of `modern-cpp-development`, and a sharper case of it: the gate's invariant is spread over six variables that must change together (the bounds before the event that publishes them, the level and the state with them, all under the watcher's lock), and no object said so. Now one type holds all six and is the only code that touches them; the free functions keep `pmm.hpp`'s interface, which is free functions throughout, and forward to it.

The atomics stay: every allocation and free asks the gate something, and the allocation path takes no lock for it. Atomics as members cost the same as atomics at namespace scope.

**Change**
Before:

```cpp
constinit std::atomic<GateState> GATE{GateState::Open};
constinit std::atomic<usize> DELAY_LEVEL{0};
constinit std::atomic<usize> SIGNAL_LOWER{0};
constinit std::atomic<usize> SIGNAL_UPPER{0};
constinit std::atomic<sync::Event *> SIGNAL_EVENT{nullptr};
sync::Event MAY_ALLOCATE{sync::Event::Options::AutoUnsignal};
sync::SpinLock WATCHER_LOCK;
auto check_free_memory_signal() -> void;
auto admit_waitable() -> Status;
```

After:

```cpp
class FreeMemoryGate {
  public:
    auto admit() -> Status;
    auto check_signal() -> void;
    auto wait_for_opening(time::Deadline const &deadline, bool interruptible) -> Status;
    auto pass_on() -> void;
    auto set(usize lower, usize upper, usize delay_level, sync::Event *event,
             DelayedAllocations delayed) -> bool;

  private:
    enum class State : u8 { Open, Shut, Refusing };
    std::atomic<State> m_state{State::Open};
    std::atomic<usize> m_delay_level{0};
    std::atomic<usize> m_signal_lower{0};
    std::atomic<usize> m_signal_upper{0};
    std::atomic<sync::Event *> m_signal_event{nullptr};
    sync::Event m_may_allocate{sync::Event::Options::AutoUnsignal};
    sync::SpinLock m_watcher_lock;
};

FreeMemoryGate GATE;
```

The one instance is a global object, not a function-local static as the watchdog's: every allocation and free reaches it, and a function-local static would add a check of its guard to each. Global objects are built first in `start_kernel()` (`ccrt::init_global_static_objects()`), before the PMM exists. `assert_cannot_wait()` stays a free function: it holds no state.

**Reference**
Zircon keeps the same state as members of `PmmNode` (`should_wait_`, `should_wait_free_pages_level_`, `may_allocate_evt_`, `mem_signal_`, `mem_signal_lower_bound_`, `mem_signal_upper_bound_`, in `zircon/kernel/vm/include/vm/pmm_node.h`).

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed.
- `mempressure.test` ok, 15 checks; `largepage.test`, `pagesource.test`, `pager.test`, `vmosnapshot.test` ok.
- `pmm.cpp` recompiles with `-Wthread-safety -Werror` with no warning; clang-tidy reports nothing in it; clang-format clean.

**Deliberately not done**
None

**How it was found**
The user's review of step 3, 2026-10-29.

**The user's prediction**
None

**Three questions you should be able to answer**
- Which six pieces of state make up the gate, and which order of writes between them does `set()` keep?
- Why is this instance a global object while the watchdog's is a function-local static?
- Why do the public functions stay free functions while the state moves into a class?

## Title: PmZone walks every zone itself, and the walks over every node's own zones use it (2026-10-29)

**Files**
- `kernel/mm/include/ours/mm/pm_zone.hpp`, `kernel/mm/pm_zone.cpp` — `PmZone::for_each()`, over a list of every zone made, which the constructor appends to
- `kernel/mm/pmm.cpp` — `free_frame_count()` and `approx_free_frame_count()` use it
- `kernel/mm/pm_node_init.cpp` — `PmNode::setup_watermarks()`'s two walks use it

**Symptom**
No change in what is counted. The user asked, 2026-10-29, why `approx_free_frame_count()` walked the online nodes and each node's own zones by hand, rather than every zone directly.

**Cause**
The count was right: `ZoneList::for_each_local_zone()` walks the node's by-type array, its own zones only, so each zone was counted once. But "every zone in the machine" had no name, so each caller spelled it out as two loops, four times (the two counts and the two walks of `setup_watermarks()`), and each carried a comment warning that a node's fallback order lists the other nodes' zones too and would count them once per node. A rule every caller must remember belongs in the one place that walks.

Linux's `for_each_zone()` (`include/linux/mmzone.h:1788`) is the same two loops, online nodes then each node's `node_zones`, hidden in `next_zone()` (`mm/mmzone.c:30`). Here the zone itself keeps the list, as the user asked: a zone is made once per node and type while the PMM is brought up on the boot CPU, and never destroyed, so a list its constructor appends to is complete before anything walks it, and the walk needs neither a lock nor PmNode.

**Change**
Before, in each of the four:

```cpp
PmNode::for_each_nid(PmNode::State::Online, [&total](NnId const nid) {
    auto *const node = PmNode::node(nid);
    if (!node) {
        return;
    }
    node->zone_list()->for_each_local_zone(
        [&total](PmZone &zone) { total += zone.free_frames(); });
});
```

After:

```cpp
PmZone::for_each([&total](PmZone &zone) { total += zone.free_frames(); });
```

with, in `PmZone`:

```cpp
template <typename F>
    requires ustl::traits::Invocable<F, PmZone &>
static auto for_each(F &&f) -> void {
    for (auto *const zone: ZONES) {
        f(*zone);
    }
}

static inline ustl::collections::StaticVec<PmZone *, NUMA_MAX_NODE * NR_ZONES_PER_NODE> ZONES;
```

The walks that are about one node stay on its ZoneList: the memmap's per region, the fallback order a node builds from the others' zones, and a node's own dump.

**Reference**
Linux `for_each_zone()` and `next_zone()`, cited above. For the free count itself Linux walks nothing: `global_zone_page_state(NR_FREE_PAGES)` (`include/linux/vmstat.h:176`) reads one counter, `vm_zone_stat[]`; that is the direction of `docs/repairs/261029/mm.md` item 1, still to be measured.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed.
- The zones' watermarks at boot are the same as the boot before this change (DMA 19/23/27, DMA32 690/862/1034). DMA32 manages 2 frames fewer (122829 against 122831), because the kernel image grew by a page (420 to 421 pages) and the package carrying it by one (451 to 452 frames): the list of zones and the code crossed a page boundary. The walk changes no total.
- `pmnode.test`, `pmzone.test`, `mempressure.test`, `largepage.test`, `pagesource.test` ok.
- clang-tidy: nothing on the lines changed (one finding, `lists_` at `pm_zone.hpp:26`, is on a line this did not touch); clang-format clean.

**Deliberately not done**
- A running total of free frames, which would make `approx_free_frame_count()` one load: a performance change, `docs/repairs/261029/mm.md` item 1.
- The unmerged `feat/pmm-cpp-vocabulary` rewrites `pm_zone.hpp` and `pm_zone.cpp` too; merging it will meet this in both files.

**How it was found**
The user's review, 2026-10-29.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why did the old walks give the right count, and what mistake was each caller one step from?
- Why can the list of zones be read without a lock?
- Which walks over zones are about one node, and why do they stay on the node's ZoneList?

## Title: the platform hands the PMM one zone bound per zone type, by type, not a short list by position (2026-10-29)

**Files**
- `kernel/platform/pc/memory.cpp` — `platform_init_pmm_with_config()` fills an array of `NR_ZONES_PER_NODE` bounds, each at its type's index, 0 for a type the platform does not list
- `kernel/mm/pm_node_init.cpp` — `set_zone_bounds()` asserts one bound per type
- `kernel/mm/include/ours/mm/init.hpp` — `init_pmm()` says what its argument is; the comment it had described a parameter that does not exist
- `docs/repairs/261029/mm.md` — item 2 removed: this change

**Symptom**
Undefined behaviour on every boot, with no effect only by luck. `set_zone_bounds()` loops five times, once per zone type, over the bounds the platform passes, and x86 passed three (DMA, DMA32, Normal, `mem_cfg.hpp:23`), so entries 3 and 4 were read from past a `std::array` on the caller's stack (`std::span`'s `[]` checks nothing). They became the upper bounds of Movable and Dynamic, whose lower bound is Normal's end, `max_pfn`, so every node's range cut to nothing against them and no such zone was made.

And a latent mislabelling: with DMA switched off (`OURS_CONFIG_ZONE_DMA` 0), the old code reported `Zone[DMA]: managed 126280 frames` on the dev image: all its memory called DMA, the type that was switched off and that can only be the lowest 16 MiB.

**Cause**
One cause for both. The platform's list holds only the types it has, a switched-off type is filtered out of it (`DisabledZoneFilter`), and `platform_init_pmm_with_config()` copied what was left into an array by position. The kernel read that array by type. So the array was shorter than the types, and read past its end; and a type left out moved every later bound onto the type before it, which with DMA off put DMA32's bound on DMA and Normal's on DMA32.

**Change**
Before:

```cpp
std::array<mm::Pfn, sizeof...(LinearZoneMaxPfns)> linear_zone_pfns;
auto index = 0;
// ... for each listed type:
linear_zone_pfns[index++] = std::min(Pfn, mm::max_pfn());
```

After:

```cpp
std::array<mm::Pfn, mm::NR_ZONES_PER_NODE> max_zone_pfns{};
// ... for each listed type:
max_zone_pfns[static_cast<usize>(Type)] = std::min(Pfn, mm::max_pfn());
```

A 0 lays the type out empty where the types before it end, since `set_zone_bounds()` takes `max(bound, start)`. `set_zone_bounds()` now asserts it was given one bound for each type.

Rejected: only bounding the loop by the list's length, which was the plan as first written. It ends the read past the end but keeps the positions, so the shift with a type left out would stay.

**Reference**
None, this project's own code.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed. The zones in the dbg boot log the same as before (DMA 3451 frames, watermarks 19/23/27; DMA32 122829, 690/862/1034). The rel image's zones were not recorded before the change, so they are not compared.
- `pmnode.test`, `pmzone.test`, `mempressure.test`, `largepage.test` ok.
- Control: a list of three handed to `init_pmm()` again stops the boot on the new assertion ("3 zone bounds for 5 zone types").
- DMA switched off, both versions booted: the old code reports `Zone[DMA]: managed 126280 frames`, the new one `Zone[DMA32]: managed 126280 frames`. Restored after.
- clang-tidy: nothing in the files changed; clang-format clean.

**Deliberately not done**
- Repair item 3 (the per-type node states): step 2 of `.agents/tasks/zone-table` removes them (D3).

**How it was found**
Reading `set_zone_bounds()` to move it into the zone table (`.agents/tasks/zone-table`), and then the platform's caller and `std::span`'s index.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why did reading two values past the array have no visible effect, and what would have made it show?
- Why does a type left out of a list kept by position move every later bound, and onto which type?
- Why does a 0 bound give an empty zone rather than a zone from the bottom of memory?

## Title: the zones are cut into a table before any node exists, and each node takes its row (2026-10-29)

**Files**
- `kernel/mm/include/ours/mm/pm_zone.hpp`, `kernel/mm/pm_zone.cpp` — `PmZone::create_all()` cuts every node's memory into zones and fills `TABLE`, nodes by types; `PmZone::at(nid, type)` reads a cell; `for_each()` walks the table; the list the constructor appended to (`3c11c310`) is gone
- `kernel/mm/pm_node_init.cpp` — `bootstrap()` calls `create_all()` before `create_nodes()`; `PmNode::init()` makes no zone and takes the ones in its row; the zone bounds and their globals moved to `create_all()`
- `kernel/mm/include/ours/mm/pm_node.hpp` — the per-type states `State::Dma`, `Dma32`, `Normal` removed (D3)
- `docs/repairs/261029/mm.md` — item 3 removed: this change

**Symptom**
None: step 2 of `.agents/tasks/zone-table/PLAN.md`. Each node made its own zones in `PmNode::init()`, so "every zone" had no owner but a side list kept in step by the constructor, and each node recorded which types it had a second time, as per-type states that nothing read and whose numbers would have set `Memory` for a Movable zone and `Online` for a Dynamic one.

**Cause**
The rule that divides memory into zones is static, and what it divides, each node's range, is in the boot memory map before `init_pmm()` runs; a zone needs its node's number and nothing of its `PmNode`. So the zones can be cut first, into one place that is the only record of them, and a node takes what is there. The table is a fixed grid (D1): `NUMA_MAX_NODE` rows by `NR_ZONES_PER_NODE` columns of pointers, at most 160, null where a node has no memory of a type. Whether a node has memory of a type is then a cell, not a state (D3).

**Change**
`bootstrap()`, before:

```cpp
set_zone_bounds(max_zone_pfn);
// ...
create_nodes(nodes);
nodes.for_each([](NnId const nid) { PmNode::node(nid)->init(); });  // each cuts its zones
```

After:

```cpp
// ...
PmZone::create_all(max_zone_pfn, nodes);  // every zone, into PmZone::TABLE[nid][type]
create_nodes(nodes);
nodes.for_each([](NnId const nid) { PmNode::node(nid)->init(); });  // each takes its row
```

and in `PmNode::init()`, the loop that cut, allocated and constructed zones became:

```cpp
for (usize i = 0; i < NR_ZONES_PER_NODE; ++i) {
    auto const ztype = ZoneType(i);
    if (auto *const zone = PmZone::at(m_id, ztype)) {
        zone_list()->insert_local_zone(zone, ztype);
        total_present_frames += zone->present_frames();
    }
}
```

Each zone is still allocated from its own node's memory (`EarlyMem::allocate<PmZone>(1, nid, ...)`). The test that dropped a cut outside the node's range (`zone_start_pfn < node_start_pfn || zone_end_pfn > node_end_pfn`) is gone: a range clamped to a type's bounds that falls outside it comes out with its end at or below its start, which the remaining test already skips. `TABLE` is `constinit` and holds only pointers, so it needs no constructor.

The node's `ZoneList` still keeps its own zones by type, filled from its row; step 3 has it read the table instead.

**Reference**
None for the table: this project's own design (`.agents/tasks/zone-table/DESIGN.md`). Linux keeps each node's zones inside its node structure (`pg_data_t::node_zones`) and walks the nodes to reach them (`for_each_zone()`, `next_zone()`); here the zones come first and the node takes them.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed. The zones in the dbg boot log the same as before the step (diffed).
- A build with `mm_numa = true` (`out/zone-table.numa`), booted with `NUMA=1` as four nodes of 1 GiB, before and after: both images PASS, and the four nodes' fallback orders and the five zones (`Zone[...]: managed ... watermarks ...`) the same, line for line (diffed).
- `pmnode.test`, `pmzone.test`, `mempressure.test`, `largepage.test`, `pagesource.test` ok on the default build; the first four ok on the four-node build.
- Control: `create_all()` leaving the DMA32 cell empty stops the boot ("block [0x1000, 0x1400) is outside the span [0x0, 0x1000)": DMA32's frames, with no zone of their own, were handed to the DMA zone). Restored after.
- clang-tidy: nothing on the lines changed (the findings it reports in `pm_node.hpp`, `modernize-use-nodiscard` on lines 74, 102, 108, 175 and 180, are on lines this did not touch); clang-format clean.

**Deliberately not done**
- The node's `ZoneList` keeping a copy of its row, `PmZone::m_node` (D2), and the free path's lookup through the node: step 3.

**How it was found**
None

**The user's prediction**
None

**Three questions you should be able to answer**
- What does cutting a node's memory into zones need to know, and why does that let it come before the node exists?
- Why can the table be read with no lock, and what would a zone added later have to do to keep that true?
- Why did leaving one cell empty stop the boot rather than just lose that memory?

## Title: a node's ZoneList reads its own zones from the zone table, and a frame finds its zone there directly (2026-10-29)

**Files**
- `kernel/mm/include/ours/mm/pm_node.hpp` — `ZoneList` keeps the fallback order only: its by-type array and `insert_local_zone()` are gone, `local_zone()`, `local_zones()` and `for_each_local_zone()` read the node's row of the table, and `take_local_zones()` puts that row at the head of the fallback order
- `kernel/mm/include/ours/mm/pm_zone.hpp` — `PmZone::row(nid)`
- `kernel/mm/pm_node_init.cpp` — `PmNode::init()` fills each of its zones' `m_node` (D2) and takes its row
- `kernel/mm/pm_node.cpp`, `kernel/mm/pmm.cpp`, `kernel/mm/memmap_init.cpp` — a frame's zone is `PmZone::at(frame->nid(), frame->zone())`, not found through its node
- `kernel/mm/pm_zone.cpp` — the comment on `m_node` says who fills it

**Symptom**
None: step 3 of `.agents/tasks/zone-table/PLAN.md`. After step 2 the same zones were recorded twice, in the table and in each node's `ZoneList` by type; and `PmZone::m_node`, which a comment said the node filled in when it adopted the zone, was only ever null.

**Cause**
A copy is one fact with two owners, and the next change to one of them is the day they disagree. The table is the record of which node has which zone; the node's list now only orders them for its allocations, which is the one thing that is the node's own. The free path went through the node only to reach its copy: `PmNode::node(frame->nid())` then its list then the type; the frame's two numbers find the zone in the table with no node at all.

**Change**
Before, in `PmNode::free_frames()`:

```cpp
auto const node = PmNode::node(frame->nid());
auto const zone = node->m_zone_list.local_zone(frame->zone());
DEBUG_ASSERT(zone, "");
```

After:

```cpp
auto *const zone = PmZone::at(frame->nid(), frame->zone());
DEBUG_ASSERT(zone, "frame {:#x} is in no zone", frame_to_phys(frame));
```

`ZoneList`, before: `std::array<ZoneRef, NR_ZONES_PER_NODE> m_zones` beside the fallback order, filled by `insert_local_zone()`. After: `local_zone(type)` is `PmZone::at(m_nid, type)`, `local_zones()` is `PmZone::row(m_nid)`, and `PmNode::init()` ends with:

```cpp
for (auto *const zone: PmZone::row(m_id)) {
    if (zone) {
        zone->m_node = this;
        total_present_frames += zone->present_frames();
    }
}
m_zone_list.take_local_zones();
```

`local_zone()` and `local_zones()`, rewritten here, are `[[nodiscard]]` now.

**Reference**
None, this project's own design (`.agents/tasks/zone-table/DESIGN.md`).

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 148 passed. The zones in the dbg boot log the same as before the task (diffed against this build's own log; see How it was found).
- The `mm_numa = true` build, booted as four nodes: both images PASS; the four fallback orders and four of the five zones the same, line for line; node 3's Normal zone manages 62890 frames instead of 62892, because the kernel image grew from 427 to 428 pages and the package carrying it from 454 to 455 frames, both on that node; its watermarks unchanged.
- `pmnode.test`, `pmzone.test`, `mempressure.test`, `largepage.test`, `pagesource.test` ok on the default build; the first four ok on the four-node build.
- Control: `PmZone::at()` answering null for DMA32 stops the boot on the free path's assertion ("frame 0x1000000 is in no zone"). Restored after.
- clang-tidy: nothing on the lines changed (`modernize-use-nodiscard` on `fallback()`, `begin()` and `end()` in `pm_node.hpp` stays, on lines this did not touch); clang-format clean.

**Deliberately not done**
- `PmZone::m_node` has no reader yet; it is filled because a field kept for planned work is not deleted as cleanup (D2).

**How it was found**
One comparison first showed three extra zone lines in the boot log. The log's first line named the kernel of `.worktrees/feat-pmm-layering`: `boot_check.py` keeps its logs in a directory named after the build directory's last component, and that worktree's session boots an `out/bringup.x86` too, so its boot had overwritten this one's log. Booted again, with the first line checked, the zones were the same. The tool's collision is recorded in `docs/repairs/261029/ours-verify.md`.

**The user's prediction**
None

**Three questions you should be able to answer**
- After step 2, which fact had two owners, and which one is the node's own now?
- Why does the free path no longer need the node, and what did it cost to go through it?
- Why is `m_node` filled although nothing reads it?
