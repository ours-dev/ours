# pmm-redesign: design the PMM from its four kinds of user, keeping node, zone and frame

**Date**: 2026-11-10
**Status**: Proposed

## Problem

The user asked for a redesign of the PMM as a whole. A first pass the same day pruned what exists (one allocation entry, fewer overloads, dead declarations removed) and the user rejected it as shallow: it improved the present code and said nothing about what the PMM must support later. That pass also proposed deleting `pin_frame()` and `unpin_frame()`, which are planned interfaces for holding a frame against being swapped out. This document replaces it. D1 and D2 of that pass stand and are kept below.

The PMM has four kinds of user, and its design models two of them:

| User | What it needs | Where that lives today |
|---|---|---|
| Taker (VMO, object cache, page tables, DMA) | frames of a shape, in a place, with an urgency | four `Gaf` bits and an order |
| Returner | to give frames back | `free_frames(frame, order)`, the order remembered by the caller |
| Mover (eviction and swap, compaction, clearing a range for a contiguous request, hot removal) | to take or replace a frame that is in use, or be told no | reclaim only, and only for pages of a VMO |
| Watcher (pressure, the reclaimer, waiters) | to know where memory is short and to wait until it is not | one machine-wide gate over one machine-wide free count |

The evidence that movers and watchers were bolted on: the pin count is in the VMM role body, so only VMO code can pin and only VMO code knows; `MigrateType` and the `Movable` and `Dynamic` zone types have no user; a block's tail frames are stamped by the object cache (`object_cache.cpp:101`) and not by the allocator that made the block; `create_contiguous()` builds a contiguous run out of a rounded-up block inside the VMO (`docs/repairs/261031/mm.md` item 1); the gate compares a machine-wide count while a request may be confined to one node or one zone.

## Goals and non-goals

Goals:

- `PmFrame`, `PmZone` and `PmNode` stay the principal objects. Each gets a defined face for movers and watchers beside the one it has for takers and returners.
- Every planned feature has one place to land: eviction and swap, 2 MiB pages, memory below 4 GiB, aligned contiguous allocation, compaction and hot removal, the per-CPU frame cache, free-frame poisoning.
- One allocation pipeline with one entry (D1), and one way for a caller to say where (D2).
- The design is decided object by object, and built one step at a time.

Non-goals:

- Building every feature above. This fixes where each lands and builds what today's callers need.
- A lock-free pin. F1 keeps pins under the owner's lock.
- `EarlyMem`, `MemMap` and `FrameBuddy`'s split and merge. They keep their shape.

## Design

### The public seam: one pair, one request

The user's constraints (2026-11-10): the interface is symmetric, the outside sees one method, flag parameters converge, and each object has a boundary that is not mixed.

```cpp
struct FrameRequest {
    Gaf flags = ALLOC_KERNEL;                   // who is asking, plus Zero and CanWait
    FrameShape shape = FrameShape::block(0);    // what
    Placement where = {};                       // where
};

auto alloc_frames(FrameRequest const &) -> ktl::Result<PmFrame::List>;
auto free_frames(PmFrame::List &) -> void;

auto pin_frame(PmFrame *) -> Status;
auto unpin_frame(PmFrame *) -> Status;
```

The currency in both directions is a list of blocks, each head carrying its own order (F2). One block is a list of one. `free_frames()` takes what `alloc_frames()` gave, or any part of it, and needs nothing else.

- `FrameShape` is built by name: `block(order)`, `pages(n)` for `n` independent frames, `run(n, align_log2)` for `n` adjacent independent frames, `exact(n)` for exactly `n` pages as one block per set bit, and later `range(phys, n)`.
- `Placement` is `{home, nodes, strict, below}`. `strict` replaces `Gaf::ThisNode`, which is about place and has no production caller. `below` is an address ceiling and resolves to the highest zone type allowed.
- `Gaf` stops being a set of independent bits. It names a class and carries two modifiers:

| Class | Asked by | Mobility | Floor | Content |
|---|---|---|---|---|
| `Kernel` | object cache, stacks, kernel data | `Unmovable` | Low | as asked |
| `PageTable` | page tables, callers that cannot block | `Unmovable` | Min | zeroed |
| `User` | pages of a VMO | `Movable` | Low | zeroed, always |
| `Reclaimer` | the reclaimer's own allocations | `Unmovable` | none | as asked |

  The modifiers are `Zero` and `CanWait`. Mobility and the floor are derived by the resolver and no caller spells them, so a new dimension adds a column here and no flag. `CanBlock` goes: the class says it. Callers keep writing `ALLOC_KERNEL`, `ALLOC_USER` and `ALLOC_USER_VMO`, which is all production code writes today (74, 13 and 2 uses).

The single-block callers, about 90, use an inline wrapper in the header that builds the request and takes the one block out of the list; `get_frames<T>()` is such a wrapper already. A wrapper is spelling over the one entry and never a second path.

### Symmetric pairs, by object

| Object | In | Out |
|---|---|---|
| the seam (`mm::`) | `alloc_frames(request)` | `free_frames(list)` |
| the seam | `pin_frame(frame)` | `unpin_frame(frame)` |
| the two gates | `prepare_alloc(frame, request)` | `prepare_free(frame)` |
| `PmNode` | `take_blocks(request, list)` | none: a frame names its own zone, so release does not pass through a node |
| `PmNode`, CPU set | `add_cpu(cpu)` | `remove_cpu(cpu)` |
| `PmZone`, stock | `take_block(order, mobility)` | `return_block(frame)` |
| `PmZone`, range | `take_range(start_pfn, n)` | `return_block(frame)` per block |
| `PmZone`, membership | `adopt(start_pfn, end_pfn)` | `retire(start_pfn, end_pfn)`, for hot removal, later |
| `PmZone`, cache | `fill_cache()` | `drain_cache()` |
| `FrameBuddy` | `allocate(order)` | `release(frame, pfn)` |
| `PmFrame` | `pin()`, `become<Body>()`, `mark_reserved()` | `unpin()`, `reset()`, `clear_reserved()` |

### Boundaries

| Object | Owns | Answers | Never |
|---|---|---|---|
| `PmFrame` | one frame's identity (zone type, node, section), its block (order, tail offset), its pin count, its role body | what am I, whose am I, may I be moved | takes a lock, names a `PmZone` or a `PmNode`, links itself anywhere |
| `FrameBuddy` | the free lists of one span | give me a block of `2^k`; take this block back | locks, counts, decides policy |
| `PmZone` | the stock of one pfn span: lock, buddy, per-CPU cache, counters, watermarks | may this be taken from me; take; return | chooses between zones, zeroes, reads a placement, wakes a reclaimer |
| `PmNode` | one node: the order of zones to try, its CPU set, its page queues, its pressure (reclaim wake-up, waiters) | in what order does this request try which zones; am I short | holds the table of nodes, serves a release, prepares a frame |
| the seam (`mm::` functions) | nothing but the node table, at file scope | the one entry: resolve the request, pick the home node, run the gates | touches a buddy, holds a zone lock |
| `CpuNodeMap` | which node each CPU is on | where is this CPU | allocates |

### One allocation

```text
mm::alloc_frames(request)                   the only entry (D1)
 resolve   -> AllocRequest                  home, policy, ceiling, mobility, floor
 admit     -> each candidate zone answers   watermark for the floor, low-zone reserve
 take      -> the zone's stock              per-CPU cache, then buddy
 prepare   -> prepare_alloc(frame, request) order and tail stamps, zeroing, role None, counters
 shortage  -> the home node's pressure      wake reclaim; ShouldWait or OutOfMem
```

Release is the mirror: `mm::free_frames(list)` runs `prepare_free(frame)` on each block (not pinned, not queued, not reserved; tail stamps cleared), returns the block to its zone, and lets the node observe pressure once.

`prepare_alloc()` and `prepare_free()` are the two gates every frame passes on its way out of and into stock. Zeroing is in `PmNode::take_block()` today, the role in the buddy, the assertions in `PmNode::return_block()` and the tail stamps in the object cache. The counterparts are Linux's `post_alloc_hook()` and `free_pages_prepare()` in `mm/page_alloc.c`.

### PmFrame

**A block is a first-class thing (F2).** The head frame of an allocated block records its order; `FrameBuddy::acquire_inner()` already writes it, and the design makes it a contract. Consequences:

- `free_frames()` takes no order. A caller cannot free a block at the wrong order.
- The allocator stamps the tail frames of a block (`compound_offset`) in `prepare_alloc()` and clears them in `prepare_free()`. The object cache stops doing it.
- `split_block(PmFrame *, usize to_order = 0)` turns one block into independent blocks of a lower order, each a head with its own order. A VMO splitting a 2 MiB block and the contiguous shape both use it.
- The contiguous shape is the allocator's: `FrameShape::run(n, align_log2)` answers `n` independent adjacent frames, the allocator having taken a block, split it and returned the surplus itself.

**Holding a frame (F1).** A pin says: this frame's physical location and its content must stay. It is a property of the frame whatever its role, so the count moves from `page::VmmBody::pinned_count` to the header, into the reference word that has one writer and no reader today.

```cpp
/// The caller holds the lock of the frame's owner.
auto pin_frame(PmFrame *frame) -> Status;
auto unpin_frame(PmFrame *frame) -> Status;
```

The rule that makes it safe is one sentence: a pin and anything that moves, replaces or reclaims the frame both run under the lock of the frame's owner, and the mover looks at the pin under that lock. For a VMO page the owner's lock is the lock of the tree its `VmCowPages` belongs to, which is where `pin_range_locked()` and `reclaim_page()` already run. The worst interleaving is a pin arriving while a reclaimer holds the page as a candidate: the reclaimer found it under the queue lock, but acts only after it has taken the owner's lock and looked again, so either the pin came first and the reclaimer backs off, or the reclaim came first and the pin finds no page. Zircon's `VmCowPages::PinRangeLocked()` and its eviction path have the same shape.

What a mover does, whatever it is: find the frame (from a page queue, or from a pfn when clearing a range), learn the owner from the role, take the owner's lock, refuse if the frame is pinned, and ask the owner to give the frame up. `Vmm` frames have an owner that can; `Slab`, `Mmu` and unowned frames cannot be moved. `prepare_free()` asserts that no frame of any role is freed pinned.

Filing a pinned page on the pinned page queue stays with the VMO: `pin_frame()` counts, and the VMO moves the page between queues on the first pin and the last unpin, as it does now.

### PmZone

Proposed; Z1 to Z3 are open. A zone is the stock of one pfn span and the judge of whether something may be taken from it. The stock has three layers: the per-CPU cache, taken without the zone lock (a performance item, built later); the buddy, keyed by order and mobility; and the reserve, which is the three watermarks plus a share held back from requests that could have used a higher zone.

- `take_block(order, mobility)` and `return_block(frame)` move one block under the zone lock.
- `admits(order, floor, wanted_zone)` is the one question a zone answers about itself: would taking `2^order` frames leave it at or above `floor`, plus its reserve against a request whose `wanted_zone` is above it. The candidate walk asks; the zone never chooses.
- `take_range(start_pfn, n)` succeeds only when the whole range is free. A named physical range uses it now, and a mover clearing a range later.
- `adopt(start_pfn, end_pfn)` is the one door frames enter stock by: the boot handoff, the init area, and hot add. It counts the frames as managed and observes no pressure. `retire()` is its inverse, for hot removal.
- The walk goes from the highest zone a request may use downwards (Z2), so that Dma is the last resort and not the first (`docs/repairs/261028/mm.md` item 1).
- The watermarks each mean one thing (Z3): High is where reclaim stops, Low is where it is woken and the floor of `Kernel` and `User`, Min is the floor of `PageTable`, and `Reclaimer` has no floor. The counterpart is Linux's `zone_watermark_ok()` with `lowmem_reserve[]`.

### PmNode

To be decided after the zone (N1). The shape under discussion: a placement domain (zone order, CPU set, policy) and a pressure domain (page queues, this node's reclaim wake-up and its waiters), with the machine-wide gate left as the watchdog's summary. The node table moves to file scope and CPU membership to `CpuNodeMap`, as `pmm-layering` D2(a) decided.

### Where each planned feature lands

| Feature | Lands in |
|---|---|
| Eviction and swap | the frame's pin, the node's pressure domain |
| 2 MiB pages | the zone's lists by order and mobility, the frame's block and `split_block()` |
| Memory below 4 GiB | the request's address ceiling, read by the candidate walk |
| Aligned contiguous allocation | the request's shape, `PmZone::take_range()`, `split_block()` |
| Compaction, hot removal | `PmZone::take_range()`, the frame's pin, the owner asked by role |
| Per-CPU frame cache | the first layer of the zone's stock |
| Free-frame poisoning, owner tracking | `prepare_alloc()` and `prepare_free()` |
| NUMA placement policy | the resolver |

## Alternatives considered

- **Prune the present design** (the first pass). Rejected by the user: it leaves movers and watchers where they were bolted on.
- **A lock-free pin** (F1(b)): one atomic word in the header that a pin raises and a mover freezes, as Linux's `folio_ref_freeze()` does. Rejected: every reclaim pays a compare-and-swap, and nothing pins without already holding the owner's lock.
- **Keep the order with the caller** (F2(a)). Rejected: the block logic stays spread over the VMO and the object cache, and large pages add a third copy.
- **Static `PmNode` operations as the pipeline**, and **paired overloads** for a home node. Rejected by D1 and D2.
- **A `Pmm` singleton**, as Zircon has. Rejected on 2026-10-28 (`pmm-layering` D2) and not reopened.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Where the pipeline across nodes lives | (a) static `PmNode::alloc_frames()` and `free_frames()`, with `mm::` adding admission; (b) the `mm::` free functions are the pipeline and `PmNode` keeps the member `take_block(request)` | (b). It reverses the choice of 2026-11-04. Production never calls the static layer; it is a second entry that skips admission | None (homework paused) | **(b)**, 2026-11-10 |
| D2. How a caller says where | (a) paired overloads with and without a home, and a trailing `NodeMask`; (b) one `Placement{home, nodes}` aggregate defaulting to `{}` | (b). A later constraint such as an address ceiling is one more field | None | **(b)**, 2026-11-10, with the name `Placement` |
| D3. Scope | (a) structure only, behaviour unchanged; (b) the behaviour debts too | (a) | None | (a), 2026-11-10; withdrawn the same day, when the user reopened the scope |
| F1. How a pin is made safe against movers | (a) the count is in the frame header, whatever the role, and is read and written under the owner's lock, which every mover also takes; (b) an atomic word that a pin raises and a mover freezes, with no owner lock | (a). Reclaim already adopts the owner, takes its lock and looks again; every mover follows the same path. The cost is that `pin_frame()` requires the owner's lock | None | **(a)**, 2026-11-10 |
| F2. Whether a block is a first-class thing | (a) the caller remembers the order and the object cache stamps tails; (b) the allocator stamps order and tails, `free_frames()` takes no order, `split_block()` is added | (b). Large pages and contiguous VMOs both take a block, split it and return the parts | None | **(b)**, 2026-11-10 |
| E1. The currency of the one entry | (a) one signature each way: `alloc_frames(FrameRequest)` answers a list of blocks and `free_frames()` takes one, with inline wrappers for the single-block callers; (b) one name with two signatures, a frame pointer for one block and a list for several | (a). Every shape takes one path, rollback and the pressure observation are written once, and a batch can be taken under one hold of the zone lock. The cost is a wrapper at about 90 single-block callers | None | |
| E2. What `Gaf` is | (a) a class (`Kernel`, `PageTable`, `User`, `Reclaimer`) plus `Zero` and `CanWait`, with mobility and floor derived, `ThisNode` moved to `Placement::strict` and `CanBlock` gone; (b) independent bits, one more per new dimension | (a). Production writes three presets and nothing else; (b) would add bits for mobility, the floor and the reclaimer | None | |
| Z1. How far mobility grouping goes | (a) the request and `take_block()` carry `MigrateType` now and the buddy keeps one list per order until large pages need more; (b) lists per order and type, a type per pageblock, and stealing whole blocks, now; (c) none | (a). It is a ceiling on large-page availability, so the user's to decide; (a) fixes the interface and leaves the mechanism until there is fragmentation to measure | None | |
| Z2. The order of the zone walk | (a) from the highest usable zone down, with low zones protected by a reserve; (b) from the lowest up, as today | (a) | None | |
| Z3. What each watermark means | (a) High stops reclaim, Low wakes it and is the floor of `Kernel` and `User`, Min is the floor of `PageTable`, `Reclaimer` has none; (b) one floor | (a). It is what the `Watermark` comments and `Gaf::CanBlock` intended and nothing reads | None | |

## Testing and verification

Each step is checked with `verify` on both images and the host tests. A step that only restructures must leave the test counts where they were; a step that changes behaviour brings its own test. For the frame: a block freed with no order returns the zone's free count and free-block profile to where they started; a block split to order 0 and freed frame by frame coalesces back to one block; a pinned page is refused by reclaim and reclaimed after the unpin; freeing a pinned frame asserts in a debug build.

What cannot be verified is that the design extends well. The check of that is whether each feature in the table above lands where the table says when it is built.

## Open questions

- `docs/repairs/261030/mm-pin.md` item 4: a read-only pin of a page-source page still needs a per-page bit to stay reclaimable after the unpin.
- Whether every block of order above 0 has its tails stamped, or only a block that is kept whole (the object cache, a large page): stamping 511 tails of a block about to be split is wasted work.
- The zone (Z1 to Z3) and the node (N1) rounds.
