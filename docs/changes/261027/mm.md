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
