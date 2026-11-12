# mm changes

## The allocation pipeline lives in mm::, and PmNode keeps one node's layer (2026-11-11)

**Files**
- `kernel/mm/pmm.cpp` — the pipeline: `observe_pressure()`, `apply_thread_policy()`, `resolve_home()`, `alloc_block()`, `free_block()` and `free_blocks()` in the unnamed namespace, and the bodies of the `mm::` allocation and release functions
- `kernel/mm/include/ours/mm/pmm.hpp` — `alloc_frames_bulk()` in two forms, `alloc_frames_fragment()` with a home node, a default order on `alloc_frames()` with a home node; the fragment contract's comment moves here
- `kernel/mm/pm_node.cpp` — the eight static operations and three static helpers go; the members `alloc_frames(request)` and `free_frames(frame, order)` replace `take_block()`
- `kernel/mm/include/ours/mm/pm_node.hpp` — the same, and `BlockFailure` becomes public
- `kernel/mm/include/ours/mm/pm_frame.hpp` — a comment that named `PmNode::free_frames()`
- `kernel/tests/pm_node_test.cpp`, `kernel/tests/memory_pressure_test.cpp` — 41 calls of the static operations go through `mm::`

**Symptom**
Latent, cannot be triggered today. No production code called the static `PmNode` operations, so nothing skipped admission; a caller that did would have.

**Cause**
One allocation had two entries with the same signature. `mm::alloc_frames(nid, flags, order, nodes)` asked the free-memory gate when the request carried `Gaf::CanWait`, and then called `PmNode::alloc_frames(nid, flags, order, nodes)`, a public static function that did everything else: resolve the home node, walk, observe pressure. Anyone holding the header could call the second and get a frame the gate had not admitted. Two tests did, 41 times, which is how the second entry came to look like an interface with users.

The same split made `PmNode` two things. An instance is one node: its zone order, its page queues, its CPU masks. The static half was every node at once: it picked which node a request starts from, looped over blocks for the list forms, rolled back, and observed pressure through `PmNode::observe_pressure()`, which was declared in `pm_node.hpp` and defined in `pmm.cpp` because the gate it calls is file-local there. A member function defined in another class's translation unit is a sign the function belongs to that other unit.

It was also what kept the node's layer from taking the seam's name. `PmNode` had a static `alloc_frames(Gaf, usize, NodeMask const &)`, and a member with the same parameter list cannot overload a static function, so the member stayed `take_block()`.

**Change**
The pipeline is the `mm::` free functions, and the node keeps the one thing only a node can do, the walk over its own zone order:

```cpp
// before: pmm.cpp
auto alloc_frames(NodeId nid, Gaf alloc_flags, usize order, NodeMask const &nodes)
    -> ktl::Result<PmFrame *> {
    if (!!(alloc_flags & Gaf::CanWait)) { /* GATE.admit() */ }
    return PmNode::alloc_frames(nid, alloc_flags & ~Gaf::CanWait, order, nodes);
}

// after: pmm.cpp
auto alloc_frames(NodeId const nid, Gaf alloc_flags, usize const order, NodeMask const &nodes)
    -> ktl::Result<PmFrame *> {
    if (!!(alloc_flags & Gaf::CanWait)) { /* GATE.admit() */ }
    alloc_flags = alloc_flags & ~Gaf::CanWait;

    auto const home = resolve_home(nid, alloc_flags);
    if (!home) { observe_pressure(); return ustl::err(home.unwrap_err()); }

    auto const block = alloc_block(**home, alloc_flags, order, nodes);
    if (!block) { /* observe_pressure(failure.depleted_zone); return its status */ }

    observe_pressure();
    return ustl::ok(*block);
}
```

`alloc_block()` is the old first half of `PmNode::take_block()`: the thread's allocation state, the order bound, and `resolve_request()`. The second half, the walk, is the member:

```cpp
// pm_node.hpp
[[nodiscard]]
auto alloc_frames(AllocRequest const &request) -> ustl::Result<PmFrame *, BlockFailure>;
auto free_frames(PmFrame &frame, usize order) -> void;
```

The member takes a resolved `AllocRequest` and not a `Gaf`. Only `resolve_request()` can build one, so the node's layer cannot be mistaken for an entry: there is nothing a caller outside the pipeline could pass it.

Release now goes seam, node, zone as well. `free_block()` asks the node table for the frame's node and calls its `free_frames()`, which finds the zone among its own. It used to look the zone up in the zone table directly and skip the node. The extra step costs one array load and puts every release in front of the node, which is where the redesign's pressure domain will look at it.

Rejected: keeping the node's two functions private and naming the five `mm::` functions as friends. It seals the layer, but five friend declarations with default arguments in two headers is more surface than the layer has, and `PmZone`'s tests need a way in too; both layers are sealed together in a later step.

**Reference**
Linux `__alloc_pages()` in `mm/page_alloc.c`: a free function that prepares the allocation context and calls `get_page_from_freelist()`, while `pg_data_t` holds a node's zones and no allocation entry. Zircon `pmm_alloc_page()` in `kernel/vm/pmm.cc` forwards to its single `PmmNode`; with several nodes the free function is where the home is chosen.

**Verification**
`verify` in the `feat/pmm-redesign` worktree. The counts are those of the commit before:

| | Before | After |
|---|---|---|
| dbg | 46 passed, 5 skipped, 0 failed | 46 passed, 5 skipped, 0 failed |
| rel | 44 passed, 8 skipped, 0 failed, 7/7 required | 44 passed, 8 skipped, 0 failed, 7/7 required |
| host tests | 183 passed | 183 passed |

Four nodes (`out/numa.x86`, `mm_numa=true`, `NUMA=1`): dbg 46 passed, rel 44 passed, host tests 183 passed.

`ktest PmZone.*:PmNode.*:PageReclaim.*:MemoryPressure.*:PageQueues.*:VmoContiguous.*:VmoPin.*` on dbg: 7 ran, 7 passed. `ktest PmZone.*:PmNode.*:MemoryPressure.*:PageReclaim.*` on the four-node machine: 4 ran, 4 passed; that is the run in which the explicit-home, excluded-home and other-node cases have a second node to work with. Not run: `PmNode` with `exhaust-remote`, which needs a memory-only node.

`format_cpp.py --check` is clean on the seven files. clang-tidy reports nothing in `pmm.cpp` or `pm_node.cpp`, and in `pm_node.hpp` only a `[[nodiscard]]` note on `ZoneList::fallback()`, which this change does not touch.

**Deliberately not done**
- The `mm::` signatures keep their trailing node mask. Nine test calls pass one, and removing the parameter (decision D4) is a change of the public interface with its own step.
- Zeroing and the locality counters stay in `PmNode::alloc_frames()`. They move to the seam with `prepare_alloc()`.
- `alloc_frames(NodeId, Gaf, PmFrame::List *, usize)` stays, as a wrapper over `alloc_frames_bulk()`, until its callers are moved.
- The node's and the zone's layers stay public; see Change.
- The two checks in `pm_node_test.cpp` that called the static form with allocation disabled are gone and not replaced: after this change they would repeat the two `mm::` calls beside them word for word.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does taking an `AllocRequest` protect the node's layer better than a comment saying "internal", when the function is still public?
- `PmNode::observe_pressure()` was declared in one file and defined in another. What did that say about where it belonged?
- A release now passes through the node. What does the node gain from seeing it, and what does it cost?

## One public signature for each allocation shape: flags, amount, home (2026-11-11)

**Files**
- `kernel/mm/include/ours/mm/pmm.hpp` — `alloc_frames(flags, order = 0, home)`, `alloc_frames_bulk(flags, n, home)`, `alloc_frames_fragment(flags, n, home)`, and `alloc_frames_n()`, `get_frames()`, `get_frames_n()` in the same shape; eleven declarations become six
- `kernel/mm/include/ours/mm/priv/pmm.hpp` — new: the same three functions with the node set as a parameter, in `mm::priv`
- `kernel/mm/pmm.cpp` — the bodies move into `mm::priv`; the public functions call them with every online node
- `kernel/mm/cpu_local.cpp`, `kernel/mm/object_cache.cpp` — the three production calls that name a home node
- `kernel/tests/pm_node_test.cpp`, `kernel/tests/memory_pressure_test.cpp`, `kernel/tests/page_reclaim_test.cpp`, `kernel/tests/vma_phys_test.cpp` — the new argument order; six calls that need a node set go through `mm::priv`

**Symptom**
None. Every caller is served as before.

**Cause**
The public allocation family had grown by doubling. Each shape existed once without a home node and once with the node as a new first parameter, and each of those took a trailing `NodeMask`. The list of single frames was spelled `alloc_frames(NodeId, Gaf, PmFrame::List *, usize)` and answered a `Status` where its siblings answered a `Result`; `alloc_frames(Gaf, PhysAddr *, usize)` existed to save its caller one call of `frame_to_phys()`. A reader had to learn eleven declarations to find three contracts.

The use did not justify the surface. Production code names a home node three times and passes a node set never; only tests passed one. And a node set is not something a caller should choose in the first place: which nodes a request may fall back to is a decision about the machine and the thread, which belongs to the allocator and to the thread's placement policy, while the one restriction a caller legitimately has, "this node and no other", already has a flag.

**Change**
One function for each shape, every parameter primitive, the home last and defaulted:

```cpp
// before
auto alloc_frames(Gaf, usize order = 0, NodeMask const & = node_online_mask());
auto alloc_frames(NodeId, Gaf, usize order = 0, NodeMask const & = node_online_mask());
auto alloc_frames(Gaf, PhysAddr *addr, usize order = 0);
auto alloc_frames(NodeId, Gaf, PmFrame::List *, usize, NodeMask const & = node_online_mask());

// after
auto alloc_frames(Gaf flags, usize order = 0, NodeId home = bootmem::ANY_NODE);
auto alloc_frames_bulk(Gaf flags, usize n, NodeId home = bootmem::ANY_NODE);
```

A home of `ANY_NODE` is not a new convention: an id outside the node table already meant "no node named, start on the local one" in `resolve_home()`. The default argument is what lets one function serve both forms.

The node set stays an input of the pipeline, in `mm::priv`:

```cpp
namespace ours::mm::priv {
auto alloc_frames(Gaf flags, usize order, NodeId home, NodeMask const &nodes)
    -> ktl::Result<PmFrame *>;
}
```

The public function is that with `node_online_mask()`. A test of an empty set, or of a set that leaves the home out, includes `ours/mm/priv/pmm.hpp` and calls the `priv` form; it still runs the whole pipeline (the gate, the thread's allocation state, rollback, the pressure observation), which calling `PmNode::alloc_frames()` with a hand-built request would not. Test calls that passed `NodeMask::of(nid)` together with the same home needed no set at all: `Gaf::ThisNode` says it.

Every old call fails to compile rather than changing meaning: a `NodeId` does not convert to a `Gaf`, and a `NodeMask` does not convert to a `NodeId`. The compiler found the two calls in `page_reclaim_test.cpp` that the search had missed.

**Reference**
Linux `alloc_pages_node(nid, gfp, order)` and `alloc_pages(gfp, order)` over `__alloc_pages(gfp, order, preferred_nid, nodemask)`: the node mask is a parameter of the core and of no convenience entry, and it is filled from the task's memory policy. Zircon `pmm_alloc_page(alloc_flags, page)` and `pmm_alloc_pages(count, alloc_flags, list)`: primitive parameters and nothing else.

**Verification**
`verify` in the `feat/pmm-redesign` worktree, counts unchanged from the commit before: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 183 passed. The same on four nodes (`out/numa.x86`, `NUMA=1`).

`ktest PmZone.*:PmNode.*:PageReclaim.*:MemoryPressure.*:PageQueues.*:VmoContiguous.*:VmoPin.*:VmaPhys.*` on dbg: 8 ran, 8 passed. `ktest PmZone.*:PmNode.*:MemoryPressure.*:PageReclaim.*` on four nodes: 4 ran, 4 passed. `format_cpp.py --check` is clean; clang-tidy reports nothing in `pmm.cpp`, `cpu_local.cpp`, `object_cache.cpp` or the two `pmm.hpp` headers.

**Deliberately not done**
- `order` and `home` are both integers, so `alloc_frames(flags, nid, order)` compiles and means something else. A strong `NodeId` would catch it; that is a change across the kernel and not this step's.
- `free_frames()` keeps its `order` parameter (decision F2).
- `alloc_frames_contiguous()` and the new `Gaf` bits are not added here.
- `pm_node_test.cpp` lost its case for the `PhysAddr *` output along with the overload it tested.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is a node set the allocator's and the thread's to decide, and not the caller's? What is the one node restriction a caller does own?
- Why do the tests call `mm::priv::alloc_frames()` and not `PmNode::alloc_frames()` with a request they build themselves?
- Why could no old call silently change meaning under the new signature, and which call can?

## Memory enters the buddy through adopt_frames(), and a free must match its block (2026-11-11)

**Files**
- `kernel/mm/include/ours/mm/pmm.hpp` — `adopt_frames(Pfn start, Pfn end)` declared; `free_frames()` documents that the order is the block's
- `kernel/mm/pmm.cpp` — `adopt_frames()` defined, with the loop that cuts a range into aligned blocks; a debug assertion in `free_frames()` that the order passed is the one the head frame records
- `kernel/mm/early_mem.cpp` — `free_frames_pfn_range()` goes; `free_frames_phys_range()` calls `adopt_frames()`
- `kernel/mm/cpu_local.cpp` — CPU 0's provisional unit is adopted, not freed

**Symptom**
Latent, cannot be triggered today. It would be the moment `free_frames()` stops taking an order and reads it from the head frame (decision F2): these two callers would then return order-0 blocks where they meant order 3 or 5, and all but the first frame of each range would be lost to the allocator.

**Cause**
`free_frames(frame, order)` was doing two jobs. One is returning a block: an allocation made it, the buddy wrote its order into the head frame when it split it off (`FrameBuddy::acquire_inner()`), and the caller hands the same block back. The other is taking in memory the buddy has never seen: ranges the boot allocator still called free at the handoff, the `.init*` area once boot is over, and the 32 pages CPU 0 ran its per-CPU data on before the real units existed. Those frames were never a block. Nothing wrote an order into them, so their head frames say 0, and the caller computes an order from the range's alignment and passes it in.

While the order is a parameter the two jobs look the same, which is how they came to share a function. They stop looking the same the moment the order is read from the frame, because only one of them has an order there to read.

The two callers were found by measurement, not by reading: a temporary record in `free_frames()` of every caller whose order differed from the head's, over a boot of both images and all 61 kernel tests on the default and the four-node machine. It named `free_frames_pfn_range()` (passed 3, head 0, during the handoff) and `CpuLocal::init()` (passed 5, head 0) and nothing else. The contiguous VMO, the suspect going in, was clean: it sets every frame's order to 0 by hand before it frees them one by one.

**Change**
Taking memory in gets its own entry, and the range says how much:

```cpp
// before: early_mem.cpp, and the same shape in cpu_local.cpp
free_frames(pfn_to_frame(start), order);   // order computed from alignment

// after
adopt_frames(start_pfn, end_pfn);
```

`adopt_frames()` is the loop `free_frames_pfn_range()` had, moved into the PMM: cut the range into the largest blocks its alignment allows and hand each to the frame's node. It observes pressure once at the end, where the old path did after every block.

Returning a block now checks the contract it always assumed:

```cpp
DEBUG_ASSERT(order == frame->order(),
             "frame {:#x} heads a block of order {} and is freed as order {}",
             frame_to_phys(frame), frame->order(), order);
```

Rejected: leaving both callers on `free_frames()` and having each write the order into the head frame first, as the contiguous VMO does. It is two lines instead of a function, but it keeps "taking in" disguised as "returning", and hot-added memory would be the third caller to learn the trick.

**Reference**
Linux `memblock_free_all()` reaching `__free_pages_core()`, which is not `__free_pages()`: boot memory enters the buddy through its own function, which sets up the pages before the first free. Zircon `PmmNode::AddFreePages()`, separate from `PmmNode::FreePage()`.

**Verification**
`verify` in the `feat/pmm-redesign` worktree, counts unchanged: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 183 passed. The same on four nodes (`NUMA=1`), and both images boot with `MEM=3072`, where RAM above 4 GiB gives the handoff more ranges to cut.

`ktest all` on dbg, default and four-node: 61 ran, 59 passed, 2 skipped, as before. The assertion is in the dbg image (its message is in the ELF) and not in rel, and it did not fire in any of these runs. `format_cpp.py --check` is clean; clang-tidy reports nothing in `pmm.cpp`, `early_mem.cpp` or `cpu_local.cpp`.

Not covered: paths these runs do not take (error branches, `exhaust-remote`), and a list freed through `free_frames(PmFrame::List *)`, which already reads each head's order and so has nothing to compare.

**Deliberately not done**
- `free_frames()` keeps its `order` parameter. With the assertion in, removing it is mechanical; it is the next step.
- `PageFrame::init()` still does not write the order field (`docs/repairs/261026/mm.md` item 5). The assertion holds because the memmap's storage is zeroed; the field should be written before the order becomes the only source.
- `PmZone::adopt()`, the zone's own entry of the design, is not built: `adopt_frames()` uses the zone's `free_frames()`, which still takes an order.

**How it was found**
See Cause. The record logged one line per distinct return address, with the address of `unwire_frame()` beside it as an anchor; `llvm-nm` gave the anchor's offset in the unstripped ELF and `llvm-objdump --start-address` named the function around each caller.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the head frame of adopted memory say order 0, and who writes the order of a block that was allocated?
- What would have happened to CPU 0's provisional unit if `free_frames()` had started reading the order from the frame before this change?
- Why find the mismatching callers with a record over a boot, and not by reading every caller of `free_frames()`? What does the record not cover?
