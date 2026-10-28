# mm changes

## PmNode::alloc_frames: report a failed allocation as an error instead of ok(nullptr) (2026-10-27)

**Files**
- `kernel/mm/pm_node.cpp` — `alloc_frames()` returns `ustl::err(Status::OutOfMem)` when both paths come back null, ahead of the postamble

**Symptom**
Out of memory became a write through a null pointer rather than a refusal. The two loops that consume the `Result` directly, `alloc_frames_fragment()` and `alloc_frames_bulk()`, do `list.push_back(*frame.unwrap())`, and `push_back` on an intrusive list writes two hook pointers into the object — so the first kernel allocation that could not be satisfied stored to address 0x10 and 0x18. Latent in the sense that nothing in a passing boot exhausts memory, but on the path every frame allocation takes.

**Cause**
`ktl::Result`'s `operator bool()` is `is_ok()`. It answers "did this call succeed", not "is the pointer inside it usable". That is the right contract for a result type, and it is precisely why wrapping a null in `ustl::ok()` is a lie: it tells every caller the call succeeded while handing them nothing.

The null came from two places that are themselves fine. `get_frame_from_zone_list()` and `alloc_frames_slowpath()` both answer with a plain `PmFrame *`, null on failure, which is the natural shape for internal helpers. `alloc_frames()` is the boundary where that convention has to be translated into the `Result` the public interface promises, and it skipped the translation — it passed the pointer to the postamble and then straight into `ustl::ok()` without ever testing it.

What makes this worth writing up is that both callers were correct. `if (auto frame = this->alloc_frames(...)) { list.push_back(*frame.unwrap()); }` is exactly how a `Result<PmFrame *>` is meant to be used. There was no misuse to find at the call sites; the type said "succeeded" and they believed it. A convention boundary that does not convert is worse than no convention, because it moves the bug to a place where the code reads correctly.

There was a second, quieter consequence. The free-function wrapper in `kernel/mm/pmm.cpp:153` already handled the error path and logs the reason:

```cpp
auto alloc_frames(Gaf gaf, usize order, NodeMask const &fallback) -> PmFrame * {
    auto result = PmNode::Current::alloc_frames(gaf, order, fallback);
    if (!result) {
        log::trace("Failed to allocate frame with reason: {}", to_string(result.unwrap_err()));
        return nullptr;
    }
    return *result;
}
```

Because the failure arrived as `ok(nullptr)`, that branch was never taken: the wrapper fell through to `return *result` and handed its callers a null with no log line at all. So every allocation failure reaching the wrapper was silent, and the one diagnostic written for it could not fire.

**Change**
Before:

```cpp
auto frame = get_frame_from_zone_list(gaf, order, control, context);
if (!frame) {
    frame = alloc_frames_slowpath(gaf, order, context);
}

alloc_frames_postamble(frame, gaf, order, context);
return ustl::ok(frame);
```

After:

```cpp
auto frame = get_frame_from_zone_list(gaf, order, control, context);
if (!frame) {
    frame = alloc_frames_slowpath(gaf, order, context);
}
if (!frame) {
    return ustl::err(Status::OutOfMem);
}

alloc_frames_postamble(frame, gaf, order, context);
return ustl::ok(frame);
```

The test sits ahead of `alloc_frames_postamble()` deliberately. That function counts `PM_NODE_LOCAL_HIT` or `PM_NODE_LOCAL_MISS` by comparing `Topology::current_node()` with `m_id` — it never looks at `frame` at all. A failure is neither a local hit nor a remote miss, so running the postamble on it would turn both counters from "allocations, by locality" into "attempts, by locality", which is a different statistic wearing the same name.

Rejected: having `get_frame_from_zone_list()` and `alloc_frames_slowpath()` return `Result` themselves. They are internal, they have exactly one caller each, and a null is the cheaper signal there; the conversion belongs once, at the public boundary, not twice inside it.

Rejected: `Status::OutOfMem` versus a more specific status per path. The slow path cannot yet distinguish "no memory" from "reclaim did not help" because both reclaim stubs return 0 (see item 2 of `docs/repairs/261026/mm.md`), so a finer status would be a guess today. It can be split when reclaim is real.

**Reference**
Linux `__alloc_frozen_pages_noprof()` in `mm/page_alloc.c` returns a bare `struct page *` and every caller tests it, so there is no wrapping layer to get wrong — the mistake here is specific to carrying a pointer inside a result type. The nearest thing to the rule being restored is Rust's convention that a fallible constructor returns `Result<T, E>` with `T` non-nullable, which is what `ktl::Result<PmFrame *>` was imitating without enforcing.

**Verification**
`python3 scripts/format_cpp.py --check kernel/mm/pm_node.cpp` clean. `prebuilt/clang/llvm-23/bin/clang-tidy -p out/bringup.x86 kernel/mm/pm_node.cpp` reports nothing on the changed lines. `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/lane.memmap` passes both images and the host tests; see the run below the commit.

Not measured: the failure path itself. Nothing in the tree drives the allocator to exhaustion yet, so this change is verified by reading plus a no-regression boot, not by observing a refusal. The test that would observe it is listed under Acceptance in the repair item and is still owed.

**Deliberately not done**
`alloc_frames_postamble()` still ignores all four of its parameters, and `gaf_to_allocation_control()` still ignores its `Gaf`. Both are filed separately. Not touched here because this change is about one thing — the boundary between a null pointer and an error — and folding in the counters or the option mapping would make the commit unreviewable against that claim.

**How it was found**
A subagent scan of `kernel/mm/` for "framework written, one step never landed" flagged the return. Confirmed by reading `ktl::Result::operator bool()` to establish it is `is_ok()`, then the two consuming loops in the same file to see that both dereference, then `kernel/mm/pmm.cpp:153` to find that the wrapper's error branch was unreachable as a result. The third reading is the one that turned this from "a null could be dereferenced" into "every allocation failure is already silent today".

**The user's prediction**
None.

**Three questions you should be able to answer**
1. `ktl::Result::operator bool()` is `is_ok()`. Why is that the right definition, and what would break if it also tested the pointer inside?
2. The callers were written correctly against the type and still faulted. What does that say about where to put a check: at the boundary that creates the value, or at the places that consume it?
3. The test went before `alloc_frames_postamble()` rather than after. What would the two locality counters have come to mean if it had gone after?

## Reclaim leaves the frame allocation path: `alloc_frames_slowpath()` and the two reclaim stubs are gone (2026-10-27)

**Files**
- `kernel/mm/pm_node.cpp` — `alloc_frames_direct_reclaim()`, `alloc_frames_direct_compact()` and `alloc_frames_slowpath()` deleted; `alloc_frames()` makes one attempt and refuses what it cannot serve
- `kernel/mm/include/ours/mm/pm_node.hpp` — the three declarations removed
- `kernel/mm/include/ours/mm/gaf.hpp` — `Gaf::DirectReclaim` no longer promises that the allocator waits; `Gaf::ThisNode` no longer claims a retry
- `kernel/mm/cpu_local.cpp` — `GAF_GROW`'s reason for leaving `DirectReclaim` out restated, since the old reason was "the allocator refuses instead of retrying" and now it refuses either way
- `kernel/lib/heap/src/vmalloc.cpp` — the churn self-test's note that its null branch is unreachable put in the past tense

**Symptom**
The first kernel-side frame allocation that could not be satisfied hung the machine: no panic, no log line, and `ours-verify` reporting only `no "--- end ---" within Ns`. Every `if (!p)` in the kernel was unreachable in exactly the case it was written for — the allocator's failure path could only be reached by an illegal request (an order past `MAX_FRAME_ORDER`, a thread with allocation disabled), never by actually running out of memory.

**Cause**
Three layers, and only the first is the kind of bug a careful reading of the function finds.

The loop had no exit. `GAF_KERNEL` is `Gaf::DirectReclaim` and nothing else, so `can_direct_reclaim` was true for every kernel allocation; `alloc_frames_direct_reclaim()` and `alloc_frames_direct_compact()` were both `return 0;`; and the only `break` other than success was `if (!can_direct_reclaim)`. So `while (1)` was literally endless for the common case. `GAF_USER` does not carry the bit, so a user allocation broke out and met the `ok(nullptr)` of the previous entry in this file instead.

The retry could not have worked even in principle, which is the part that decided the fix. `gaf_to_allocation_control()` (`kernel/mm/pm_node.cpp:26`) is a stub whose whole body is `return AllocationOptions();` — an empty set, for every `Gaf`. So `control` was identical in `alloc_frames()` and in the slowpath, and the slowpath's `get_frame_from_zone_list(gaf, order, control, context)` asked the same zones the same question with byte-identical arguments to the call `alloc_frames()` had just made one line earlier. This was not Linux's "relax the watermark and try again"; it was a repeat. Bounding the loop with a retry counter would therefore have been tuning a number whose only correct value was 1.

The stubs were written somewhere they cannot be filled in. Reclaim means taking frames back from whatever is holding them: pages owned by `VmCowPages`, the ranking `PageQueues` keeps, the object cache's slabs. All three sit **above** `PmNode` — it is the bottom of mm, the thing they allocate from. For `PmNode::alloc_frames_direct_reclaim()` to do its job it would have to call up into them, which inverts the dependency the whole subsystem is layered on. So these were not two functions that nobody had got round to writing; they were two functions in a place where the work cannot be done. That is why the fix deletes them instead of bounding the loop around them: a bounded loop would have preserved the inversion and left a reclaim hook in the one layer that must not have one.

The free-standing consequence worth keeping in mind: `Gaf::DirectReclaim`'s documented contract was "The allocation may wait ... The allocator does the waiting, not its caller." A promise an implementation cannot keep is worse than no promise, because callers are written against it. `kernel/lib/heap/src/vmalloc.cpp` had already noticed and worked around it in a comment, and `docs/changes/261005/prof.md` records a 1 GiB-per-CPU limit that exists only because "the allocation will answer NoMemory past what the machine has" turned out to be false.

**Change**
Before:

```cpp
auto PmNode::alloc_frames_direct_reclaim(Gaf gaf, usize order, AllocationOptions options,
                                         MemoryAllocationContext const &context) -> PmFrame * {
    return 0;
}

auto PmNode::alloc_frames_direct_compact(Gaf gaf, usize order, AllocationOptions options,
                                         MemoryAllocationContext const &context) -> PmFrame * {
    return 0;
}

auto PmNode::alloc_frames_slowpath(Gaf gaf, usize order, MemoryAllocationContext const &context)
    -> PmFrame * {
    auto const control = gaf_to_allocation_control(gaf);
    auto const can_direct_reclaim = !!(gaf & Gaf::DirectReclaim);

    PmFrame *frame;
    while (1) {
        frame = get_frame_from_zone_list(gaf, order, control, context);
        if (frame) {
            break;
        }

        if (!can_direct_reclaim) {
            break;
        }

        frame = alloc_frames_direct_reclaim(gaf, order, control, context);
        if (frame) {
            break;
        }

        frame = alloc_frames_direct_compact(gaf, order, control, context);
        if (frame) {
            break;
        }
    }

    return frame;
}
```

and, in `alloc_frames()`:

```cpp
    // First attempt to allocate.
    auto frame = get_frame_from_zone_list(gaf, order, control, context);
    if (!frame) {
        frame = alloc_frames_slowpath(gaf, order, context);
    }
```

After: all three functions are gone, and `alloc_frames()` reads

```cpp
    auto *const frame = get_frame_from_zone_list(gaf, order, control, context);
    if (!frame) {
        return ustl::err(Status::OutOfMem);
    }
```

with the comment above it recording why there is no reclaim here and where a relaxed retry goes when one exists. `frame` became `auto *const` because it is now assigned exactly once.

Rejected: a retry counter in the loop (the shape Linux has). Its only defensible value today is 1, because nothing inside the loop can change the next iteration's outcome, so the constant would have been a knob that looks meaningful and is not. It comes back with the first real reclaimer, when the loop has something that makes progress to count the absence of.

Rejected: keeping `alloc_frames_slowpath()` as an empty seat for the future watermark-relaxing retry. With the loop and the stubs removed its entire body would have been a duplicate of the call `alloc_frames()` had just made. The seat is cheap to re-create at the point where `gaf_to_allocation_control()` starts mapping `Gaf` to real options, and at that point the second attempt will have arguments of its own and so a reason to be a separate function.

Rejected: a registration list (`FrameShrinker` and friends) so that `PmNode` could keep calling reclaim without naming a concrete type, which is how Linux avoids the inversion. It keeps the `Gaf::DirectReclaim` contract intact, but it puts a second polymorphic dispatch under every allocation failure and departs from Zircon where there is no need to.

Rejected: the user's first name for the reclaim layer, `PageProvider`. `mm::PageProvider` already exists — `kernel/mm/include/ours/mm/page_request.hpp:38`, implemented by `object::PagerProxy` — and means very nearly the opposite: the party that *supplies* pages into a sourced VMO on demand. `Evictor` or `Reclaimer` instead, when it is built.

**Reference**
Zircon, and this is the decision rather than an analogy: `kernel/vm/pmm_node.cc` does not mention eviction or reclaim anywhere on the allocation path. The only thing it does about memory pressure is publish a level, `PmmNode::SetFreeMemorySignal()`. Reclaim is `Evictor` (`kernel/vm/include/vm/evictor.h`, "Implements page evictor logic to free pages belonging to a PmmNode under memory pressure"), which the `PmmNode` owns but does not call: it is driven from outside the allocation path by its own `Evictor::EvictionThreadLoop()` and by the memory-pressure scanner, `kernel/vm/scanner.cc` calling `EvictAsynchronous()` and `EvictFromExternalTarget()`, and it reaches down into `VmCowPages::Reclaim*`. Every dependency points down.

Linux is the other answer and was not taken: `__alloc_pages_slowpath()` in `mm/page_alloc.c` calls `__alloc_pages_direct_reclaim()` → `try_to_free_pages()`, so its allocator does call up — but only through registered interfaces (`struct shrinker`, `address_space_operations`), never naming a concrete page holder, and its loop exits on `should_reclaim_retry()` plus a `no_progress_loops` counter. Both halves were missing here: no registration, and no counter.

**Verification**
`python3 scripts/format_cpp.py --check` clean on all five files. `prebuilt/clang/llvm-23/bin/clang-tidy -p out/bringup.x86` on `pm_node.cpp`, `cpu_local.cpp` and `vmalloc.cpp` reports no diagnostic on any changed line; the `modernize-use-nodiscard` warnings it does report are on accessors in `pm_node.hpp` and `x86_pagings.hpp` that this change did not touch. `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/lane.memmap` passes both images and the host tests; the run is below the commit.

Not measured: the failure path itself, again. Nothing in the tree drives the allocator to exhaustion, so "an allocation that cannot be served is now refused instead of hanging" is verified by reading plus a no-regression boot, not by observing a refusal. The test that would observe it is the one listed under Acceptance in both repair items this change closes, and it is still owed.

**Deliberately not done**
The reclaimer. This change only takes reclaim off the allocation path; nothing reclaims yet, and an exhausted system now fails allocations rather than freeing anything. That is strictly better than hanging and is not the finished mechanism.

The free-frame-level signal on `PmNode` that a reclaimer will watch (Zircon's `SetFreeMemorySignal()`). Next step, not this one.

`gaf_to_allocation_control()` is still a stub returning an empty set, so `AllocationOptions::NoBlock` is still never set and the watermark gate in `PmZone::alloc_frames()` still turns away every caller of a zone that is running short. Filed separately; it is also what has to land before a relaxed retry makes sense.

`Gaf::DirectReclaim` keeps its enumerator although nothing now reads it — `gaf_to_allocation_control()` ignores its argument, so the bit is documentation until that mapping is written. Deleting it would be removing scaffolding for planned work.

`.agents/tasks/flag-consolidation/` plans to strip `AllocationOptions` and its `options` parameter from five signatures; two of those signatures just disappeared, which shortens that task. Its documents were left alone — that work is another session's to land, and editing its plan from here would be writing in someone else's workspace.

`docs/design/260923/userspace-interface.md` still carries a row listing this as 待修 in a point-in-time survey table. Left as it is: that document is a dated snapshot, not a live tracker.

**How it was found**
It did not need finding — it was item 2 of `docs/repairs/261026/mm.md`, and item 3 of `docs/repairs/260908/heap.md` before that, where it had been reproduced for real in 2026-09: restore the `~VmCowPages() = default` leak, run the vmalloc churn self-test through a 512 MiB guest, and at ~1975 rounds about 494 MiB had leaked with no error, no null and no panic — the kernel was still spinning when QEMU was killed.

What was new this round was the evidence that it could not be bounded, which came from two readings. A census of the frame allocator's callers (`vm_cow_pages.cpp` 10 sites, `early_mem.cpp` 7, `cpu_local.cpp` 6, `object_cache.cpp` 5, the x86 page table and its deposits 5, `vmalloc.cpp` 3) showed the Vm layer is the largest single consumer but only about a third, and that the heap and the page tables cannot be reclaimed through anything VMO-shaped — so a reclaimer reachable only from the Vm side would leave those with no reclaim at all. Then reading `gaf_to_allocation_control()` showed the retry's arguments were identical to the first attempt's, which is what turned "bound the loop" into "the loop has nothing to do". Zircon was then read to confirm the alternative actually exists there: `grep -n "Evictor\|evict\|reclaim" kernel/vm/pmm_node.cc` returns exactly one line, and it is `SetFreeMemorySignal`.

**The user's prediction**
None — homework is paused. The shape was the user's decision, taken in two steps: reclaim becomes its own activity above the page queues driven by a free-memory signal, rather than a step inside a failed allocation (chosen over the registration-list alternative); and then the slowpath function deleted outright rather than kept as an empty seat.

**Three questions you should be able to answer**
1. Filling in the two stubs where they stood would have required `PmNode` to call `VmCowPages`, `PageQueues` and `MemCache`. Why is that the wrong direction, and what does Zircon's `PmmNode` do instead of calling its own `Evictor`?
2. `gaf_to_allocation_control()` returns an empty option set for every `Gaf`. Explain how that single fact turns the slowpath's retry from "try harder" into "ask the same question again", and why it made a retry counter the wrong fix.
3. The allocator's documented contract said it would wait rather than fail. Two places in the tree had already been written around the fact that it does not. What does that cost pattern tell you about leaving a promise in a comment that the code cannot keep?
