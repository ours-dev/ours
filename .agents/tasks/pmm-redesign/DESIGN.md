# pmm-redesign: design the PMM from its four kinds of user, keeping node, zone and frame

**Date**: 2026-11-10
**Status**: Proposed

## Problem

The user asked for a redesign of the PMM as a whole. A first pass the same day pruned what exists (one allocation entry, fewer overloads, dead declarations removed) and the user rejected it as shallow: it improved the present code and said nothing about what the PMM must support later. That pass also proposed deleting `pin_frame()` and `unpin_frame()`, which are planned interfaces for holding a frame against being swapped out. This document replaces it.

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

- `PmFrame`, `PmZone` and `PmNode` stay the principal objects. Each gets a defined face for movers and watchers beside the one it has for takers and returners, and a boundary that is not mixed.
- The public interface is symmetric and takes primitive parameters, in the style of Linux's `alloc_pages()` and Zircon's `pmm_alloc_page()`: a flags word, a count or an order, a node id. No compound structure is a parameter (the user's rule, 2026-11-10).
- What a caller wants beyond size and node is said in the one flags word, `Gaf`, and nowhere else.
- Every planned feature has one place to land: eviction and swap, 2 MiB pages, memory below 4 GiB, aligned contiguous allocation, compaction and hot removal, the per-CPU frame cache, free-frame poisoning.
- The design is decided object by object, and built one step at a time.

Non-goals:

- Building every feature above. This fixes where each lands and builds what today's callers need.
- A lock-free pin. F1 keeps pins under the owner's lock.
- `EarlyMem`, `MemMap` and `FrameBuddy`'s split and merge. They keep their shape.

## Design

### The public seam

Every parameter is primitive and there are at most three, four for a contiguous run: what (`Gaf`), how much, and optionally which node.

```cpp
auto alloc_frames(Gaf flags, usize order = 0, NodeId home = ANY_NODE) -> ktl::Result<PmFrame *>;
auto free_frames(PmFrame *frame) -> void;

auto alloc_frames_bulk(Gaf flags, usize n, NodeId home = ANY_NODE) -> ktl::Result<PmFrame::List>;
auto alloc_frames_fragment(Gaf flags, usize n, NodeId home = ANY_NODE)
    -> ktl::Result<PmFrame::List>;
auto alloc_frames_contiguous(Gaf flags, usize n, usize align_log2, NodeId home = ANY_NODE)
    -> ktl::Result<PmFrame::List>;
auto free_frames(PmFrame::List *frames) -> void;

auto pin_frame(PmFrame *frame) -> Status;
auto unpin_frame(PmFrame *frame) -> Status;

auto split_block(PmFrame *frame, usize order = 0) -> void;
```

- `alloc_frames()` is Linux's `alloc_pages_node(nid, gfp, order)` with the node last and defaulted, so the paired overloads with and without a home node become one function: a home of `ANY_NODE` means the calling CPU's node, as an id outside the node table already does today. Eleven declarations become four.
- No node mask is a public parameter (D4). Production code never passes one; "this node only" is `Gaf::ThisNode`, and a wider restriction comes from the calling thread's placement policy. The mask stays an input of the pipeline: the three functions exist in `mm::priv` (`ours/mm/priv/pmm.hpp`) with the node set as a last parameter, the public ones call them with every online node, and a test of a restriction the public interface cannot ask for calls the `priv` form.
- The list forms are Zircon's `pmm_alloc_pages(count, flags, list)` and `pmm_alloc_contiguous(count, flags, align_log2, pa, list)`, and `free_frames(PmFrame::List *)` is its `pmm_free(list)`. `alloc_frames_bulk()` is today's `alloc_frames(NodeId, Gaf, PmFrame::List *, usize)` under its own name.
- `free_frames()` takes no order, because the head of a block records it (F2).
- `get_frames<T>()`, `get_frames_n<T>()` and `alloc_frames_n()` stay as inline wrappers. `alloc_page_table()` and `free_page_table()`, `unwire_frame()`, `wait_for_frame_allocation()`, `set_free_memory_signal()` and the two counts do not change.

Inside the allocator the resolved `AllocRequest` stays: it is an internal value built once from these parameters, as Linux's `struct alloc_context` is, and no caller sees it.

### Gaf: six bits, each with one reader

`Gaf` is the one word for what a caller wants beyond size and node. It stays a set of bits, as `gfp_t` and Zircon's `PMM_ALLOC_FLAG_*` are, and it is kept to what a microkernel needs (the user's goal, restated 2026-11-10): the four bits it has, and two new ones.

| Bit | Meaning | Read by | Counterpart |
|---|---|---|---|
| `CanBlock` | the caller's context permits blocking. It selects the floor (Z3): a caller that may block is stopped at Low, one that may not can take a zone down to Min | the resolver | `__GFP_DIRECT_RECLAIM` |
| `CanWait` | on shortage the request is answered `ShouldWait` and the caller waits with its locks dropped; without it the answer is `OutOfMem` | the entry | Zircon `PMM_ALLOC_FLAG_CAN_WAIT` |
| `ThisNode` | the home node and no other. `Gaf` travels through layers that have no `nodes` parameter (`kmalloc()`), which is why it is a bit | the resolver | `__GFP_THISNODE` |
| `Zero` | the block is zeroed | `prepare_alloc()` | `__GFP_ZERO` |
| `Movable` (new) | the frame will belong to a VMO, an owner that can give it up when asked: it comes from the movable free lists | the resolver, then the zone as a `MigrateType` | `__GFP_MOVABLE` |
| `Dma32` (new) | the frame lies below 4 GiB | the resolver, as the request's highest zone | `__GFP_DMA32`, Zircon `PMM_ALLOC_FLAG_LO_MEM` |

| Preset | Bits | Asked by |
|---|---|---|
| `ALLOC_KERNEL` | `CanBlock` | object cache, stacks, kernel data; page tables add `Zero` |
| `ALLOC_USER` | `CanBlock`, `Movable`, `Zero` | pages a process can see |
| `ALLOC_USER_VMO` | `ALLOC_USER`, `CanWait` | pages of a VMO, which may be delayed under pressure |

`ALLOC_USER` gains `CanBlock` and `Movable`; it is `Zero` alone today, which under Z3 would let a user page take a zone lower than a kernel allocation may.

Why the rest of `gfp_t` gets no bit: each of those exists in Linux because drivers and filesystems allocate inside the kernel, and here they do not.

| Linux dimension | Why a monolithic kernel needs it | Here |
|---|---|---|
| `__GFP_IO`, `__GFP_FS` | an allocation can recurse into a filesystem or a driver while reclaiming | reclaim is above the PMM and writeback belongs to a user pager |
| `__GFP_HIGH`, `GFP_ATOMIC`, `__GFP_MEMALLOC` (deeper reserves) | drivers allocate in interrupt context, and reclaim itself allocates | interrupt paths in the kernel do not allocate; the one floor below Low is enough |
| `__GFP_RECLAIMABLE` | dentry and inode caches | no such cache; mobility is two-valued, a VMO's frame or not |
| `__GFP_DMA` | ISA devices | a device gets memory through a contiguous VMO, and only the 4 GiB line matters |
| `__GFP_COMP` | many callers take a high-order block and split it themselves | splitting happens inside the allocator (`alloc_frames_contiguous()`), so every block above order 0 handed out is kept whole and always has its tails stamped |

The `Dma` zone and `MigrateType::Reclaimable` stay as they are, without a flag, until something needs to ask for them.

### Symmetric pairs, by object

| Object | In | Out |
|---|---|---|
| the seam (`mm::`) | `alloc_frames(flags, order, home)` | `free_frames(frame)` |
| the seam | `alloc_frames_bulk()`, `_fragment()`, `_contiguous()` | `free_frames(list)` |
| the seam | `pin_frame(frame)` | `unpin_frame(frame)` |
| the two gates | `prepare_alloc(frame, request)` | `prepare_free(frame)` |
| `PmNode` | `alloc_frames(request)` | `free_frames(frame)` |
| `PmNode`, CPU set | `add_cpu(cpu)` | `remove_cpu(cpu)` |
| `PmZone`, stock | `alloc_frames(order, mobility)` | `free_frames(frame)` |
| `PmZone`, range | `take_range(start_pfn, n)` | `free_frames(frame)` per block |
| `PmZone`, membership | `adopt(start_pfn, end_pfn)` | `retire(start_pfn, end_pfn)`, for hot removal, later |
| `PmZone`, cache | `fill_cache()` | `drain_cache()` |
| `FrameBuddy` | `allocate(order)` | `release(frame, pfn)` |
| `PmFrame` | `pin()`, `become<Body>()`, `mark_reserved()` | `unpin()`, `reset()`, `clear_reserved()` |

### Boundaries

| Object | Owns | Answers | Never |
|---|---|---|---|
| `PmFrame` | one frame's identity (zone type, node, section), its block (order, tail offset), its pin count, its role body | what am I, whose am I, may I be moved | takes a lock, names a `PmZone` or a `PmNode`, links itself anywhere |
| `FrameBuddy` | the free lists of one span | give me a block of `2^k`; take this block back | locks, counts, decides policy |
| `PmZone` | the stock of one pfn span: lock, buddy, per-CPU cache, counters, watermarks | may this be taken from me; take; return | chooses between zones, zeroes, reads a `Gaf`, wakes a reclaimer |
| `PmNode` | one node: the order of zones to try, its CPU set, its page queues, its pressure (reclaim wake-up, waiters) | in what order does this request try which zones; am I short | holds the table of nodes, prepares a frame |
| the seam (`mm::` functions) | nothing but the node table, at file scope | the one entry: resolve the request, pick the home node, run the gates | touches a buddy, holds a zone lock |
| `CpuNodeMap` | which node each CPU is on | where is this CPU | allocates |

`Gaf` is read in two places only: the resolver, which turns it into the request's zone ceiling, mobility and floor, and `prepare_alloc()`, which reads `Zero`. A zone is handed an order, a mobility and a floor, never a `Gaf`.

### Built now, and slots for later

The interface an object has in this task is small. The rest is a place kept for a feature, written down so that the feature has one place to go, and not built until the feature is.

Only the seam is visible outside the PMM. The layers under it carry the same pair of names (E3): `mm::alloc_frames()` calls `PmNode::alloc_frames()`, which calls `PmZone::alloc_frames()`, and `mm::free_frames()` calls `PmNode::free_frames()` on the frame's node, which calls `PmZone::free_frames()`. Nothing else calls the node's or the zone's. The target is that they and `PmZone::admits()` are private members reached through `friend`, so that a caller outside cannot skip the seam's admission and gates. As built (2026-11-12) they are public: `PmZone`'s and `PmNode`'s own tests call them, and sealing both layers is a later step. Each layer does one thing under the one name: the seam resolves and prepares, the node chooses a zone and looks at its pressure, the zone moves the block under its lock.

| Object | Built in this task | A slot, built with its feature |
|---|---|---|
| the seam | the nine functions above, and `adopt_frames(start_pfn, end_pfn)` for memory the buddy has never held | `alloc_frames_range(phys, n)` |
| `PmFrame` | block order as a contract, tail stamps by the allocator, the pin count in the header | none |
| `PmZone` | `alloc_frames(order, mobility)`, `free_frames(frame)`, `admits(order, floor, highest_zone)` | `take_range()`, `adopt()` and `retire()`, `fill_cache()` and `drain_cache()` |
| `PmNode` | `alloc_frames(request)`, `free_frames(frame)`, `add_cpu()` and `remove_cpu()`, `wake_reclaim(zone)`, `reclaim_target()` | none |

### One allocation

```text
mm::alloc_frames(flags, order, home)         the only entry for one block (D1)
 resolve   -> AllocRequest                   home, policy, ceiling, mobility, floor
 admit     -> each candidate zone answers    watermark for the floor, low-zone reserve
 take      -> the zone's stock               per-CPU cache, then buddy
 prepare   -> prepare_alloc(frame, request)  order stamp, tail stamps, zeroing, role None, counters
 shortage  -> the home node's pressure       wake reclaim; ShouldWait or OutOfMem
```

The list forms run the same stages once per block inside one rollback scope. Release is the mirror: `mm::free_frames()` runs `prepare_free(frame)` (not pinned, not queued, not reserved; tail stamps cleared), and hands the block to `PmNode::free_frames()` on the frame's node, which returns it to the zone and looks at the node's pressure once.

`prepare_alloc()` and `prepare_free()` are the two gates every frame passes on its way out of and into stock. Before this task zeroing was in `PmNode::take_block()`, the role in the buddy, the release assertions in `PmNode::return_block()` and the tail stamps in the object cache. As built, zeroing, the locality counters and the release assertions are in `prepare_alloc()` and `prepare_free()` in `pmm.cpp`; the tail stamps are still the object cache's. The counterparts are Linux's `post_alloc_hook()` and `free_pages_prepare()` in `mm/page_alloc.c`.

### PmFrame

**A block is a first-class thing (F2).** The head frame of an allocated block records its order; `FrameBuddy::acquire_inner()` already writes it, and the design makes it a contract. Consequences:

- `free_frames()` takes no order. A caller cannot free a block at the wrong order.
- The allocator stamps the tail frames of every block above order 0 it hands out (`compound_offset`) in `prepare_alloc()` and clears them in `prepare_free()`. The object cache stops doing it.
- `split_block(PmFrame *, usize to_order = 0)` turns one block into independent blocks of a lower order, each a head with its own order. A VMO splitting a 2 MiB block uses it, and so does `alloc_frames_contiguous()`, which takes a block, splits it and returns the surplus itself.

**Holding a frame (F1).** A pin says: this frame's physical location and its content must stay. It is a property of the frame whatever its role, so the count moves from `page::VmmBody::pinned_count` to the header. It takes a byte of the half word the compound offset had, which never needs more than sixteen bits. An earlier version of this document put it in the reference word and called that word unused; the object cache takes a reference on a slab's frame when it makes the slab and drops it when it destroys it, so the word stays as it is. The limit stays 255 pins, which `vmo_pin_test.cpp` and `bti_test.cpp` check. `pin_frame()` and `unpin_frame()` require the caller to hold the lock of the frame's owner.

The rule that makes it safe is one sentence: a pin and anything that moves, replaces or reclaims the frame both run under the lock of the frame's owner, and the mover looks at the pin under that lock. For a VMO page the owner's lock is the lock of the tree its `VmCowPages` belongs to, which is where `pin_range_locked()` and `reclaim_page()` already run. The worst interleaving is a pin arriving while a reclaimer holds the page as a candidate: the reclaimer found it under the queue lock, but acts only after it has taken the owner's lock and looked again, so either the pin came first and the reclaimer backs off, or the reclaim came first and the pin finds no page. Zircon's `VmCowPages::PinRangeLocked()` and its eviction path have the same shape.

What a mover does, whatever it is: find the frame (from a page queue, or from a pfn when clearing a range), learn the owner from the role, take the owner's lock, refuse if the frame is pinned, and ask the owner to give the frame up. `Vmm` frames have an owner that can; `Slab`, `Mmu` and unowned frames cannot be moved. `prepare_free()` asserts that no frame of any role is freed pinned.

Filing a pinned page on the pinned page queue stays with the VMO: `pin_frame()` counts, and the VMO moves the page between queues on the first pin and the last unpin, as it does now.

### PmZone

Decided (Z1 to Z3). A zone is the stock of one pfn span and the judge of whether something may be taken from it. The stock has three layers: the per-CPU cache, taken without the zone lock (a performance item, built later); the buddy, keyed by order and mobility; and the reserve, which is the three watermarks plus a share held back from requests that could have used a higher zone.

- `alloc_frames(order, mobility)` and `free_frames(frame)` move one block under the zone lock.
- `admits(order, floor, wanted_zone)` is the one question a zone answers about itself: would taking `2^order` frames leave it at or above `floor`, plus its reserve against a request whose `wanted_zone` is above it. The candidate walk asks; the zone never chooses.
- `take_range(start_pfn, n)` succeeds only when the whole range is free. A named physical range uses it now, and a mover clearing a range later.
- `adopt(start_pfn, end_pfn)` is the one door frames enter stock by: the boot handoff, the init area, and hot add. It counts the frames as managed and observes no pressure. `retire()` is its inverse, for hot removal.
- The walk goes from the highest zone a request may use downwards (Z2), so that Dma is the last resort and not the first. Built 2026-11-14 as the order of the fallback list, node first and zone second (each node's zones from the highest type down, then the next node's); the low-zone reserve is left to Z3, where the admission check that would read it is built. Z3 is built as revised with the user on 2026-11-14: two passes, Low for every request and then Min (three quarters of Min for a caller that cannot block), and each low zone keeping 1/256 of the memory above it from requests that could use a higher zone.
- The watermarks each mean one thing (Z3): High is where reclaim stops; Low is where it is woken, and the floor of a request with `CanBlock`; Min is the floor of a request without it. The counterpart is Linux's `zone_watermark_ok()` with `lowmem_reserve[]`.

### PmNode

Decided (N1 to N3). A node is two domains. As a placement domain it owns the order in which a request tries zones and the set of CPUs working on it. As a pressure domain it owns its page queues and the record of which of its zones are short. Zircon's `PmmNode` is the counterpart of the second: it holds the free-memory signal, the should-wait state, the page queues and the evictor for its one node. Here those are spread over a gate in `pmm.cpp`, a two-dimensional array inside `MemoryReclaimer` and the node.

```cpp
class PmNode {
    // Placement domain.
    auto alloc_frames(AllocRequest const &request) -> ustl::Result<PmFrame *, BlockFailure>;
    auto free_frames(PmFrame &frame) -> void;
    auto add_cpu(CpuNum cpu) -> void;
    auto remove_cpu(CpuNum cpu) -> void;

    // Pressure domain.
    auto page_queues() -> PageQueues &;
    auto wake_reclaim(PmZone const &zone) -> void;
    auto reclaim_target() -> usize;
};
```

- `alloc_frames()` walks the node's candidates for the request, asks each zone `admits()` and takes from the first that does. `free_frames()` returns a block to the zone of this node it came from, and wakes one waiter when that lifts the zone back above Low.
- `wake_reclaim(zone)` is called by the entry when a walk found a zone below its floor: it marks the node as wanting reclaim and wakes the reclaimer. The counterpart is Linux's `wakeup_kswapd()`.
- `reclaim_target()` is what the reclaimer asks each node: how many frames its zones are short of High, and zero for a node nobody marked. The mark is the node's (N2); the thread and the order of the walk (zero pages, then pager pages) stay with `MemoryReclaimer`, above the PMM. As built (2026-11-14, N2 option (b), the user's choice): the node only records, as `mark_short(zone) -> bool` (true on the first mark since the last take) and `take_reclaim_target()`, and `MemoryReclaimer::request(zone)` marks and wakes its own thread, so `PmNode` does not depend on the reclaimer; `wake_reclaim()` and `reclaim_target()` above are the draft's names.

One request that waits, on a node that is short:

```text
alloc_frames(ALLOC_USER_VMO)   every candidate zone is below Low: wake_reclaim(), answer ShouldWait
caller                         drops its locks, wait_for_frame_allocation(flags, deadline)
reclaimer                      asks each node reclaim_target(), walks that node's page queues
free_frames()                  a zone rises above Low: its node wakes one waiter
waiter                         retries; on success wakes the next waiter, on ShouldWait sleeps again
```

As built (2026-11-14, N1 revised with the user): the machine-wide gate stays, and a waiting request is also put off at a zone's Low, answering `ShouldWait` without the second pass; a release that brings a zone back to admitting a frame at Low (the count's own increment crossing Low plus the zone's reserve) wakes one waiter on the gate's event. The rest of this paragraph is the first draft, set aside because it removed the gate. A request is put off by one threshold only, the zone's Low floor (N1). The gate's delay level and its Shut state go, because they are a second, machine-wide threshold for the same decision, and a request confined to one node or to memory below 4 GiB can be short while the machine-wide count is healthy. What stays machine-wide is what is machine-wide: the watchdog's signal over total free memory, from which it reports pressure to user space; its switch at OutOfMemory that makes every waiter and every later `CanWait` request answer `OutOfMem` (Zircon's `pmm_stop_returning_should_wait()`); and the waiters, which are one event and not one per node, since a request may fall back to another node and a relief anywhere is a reason to retry.

As built (2026-11-14): the watchdog's levels are raised so that the gate's delay level sits above the zones' floors summed (D1(a)), which makes the gate end every ordinary wait, and a confined waiting request is not put off at Low at all (D2(b)); the stuck mark below was not built. A wait has to end when nothing can end the shortage: a node whose reclaim pass gave nothing back is marked stuck, and a retry there answers `OutOfMem` and not `ShouldWait`.

The node table and the state masks are file-scope data behind free functions, as `pmm-layering` D2(a) decided. As built (2026-11-12) that is `ours/mm/node_table.hpp`: `find_node(nid)`, `node_mask(NodeState)`, `set_node_state()`, `for_each_node()` and `for_each_nid()`, in the flat `node_*` style `pmm.hpp` already used (the user's choice of the two spellings offered). CPU membership (publish, claim, move) belongs to `CpuNodeMap`.

### Where each planned feature lands

| Feature | Lands in |
|---|---|
| Eviction and swap | the frame's pin, the node's pressure domain |
| 2 MiB pages | the zone's lists by order and mobility, the tail stamps, `split_block()` |
| Memory below 4 GiB | `Gaf::Dma32`, read by the resolver and applied by the candidate walk |
| Aligned contiguous allocation | `alloc_frames_contiguous()`, `split_block()`, later `PmZone::take_range()` |
| Compaction, hot removal | `PmZone::take_range()` and `retire()`, the frame's pin, the owner asked by role |
| Per-CPU frame cache | the first layer of the zone's stock |
| Free-frame poisoning, owner tracking | `prepare_alloc()` and `prepare_free()` |
| NUMA placement policy | the resolver |

## Alternatives considered

- **Prune the present design** (the first pass). Rejected by the user: it leaves movers and watchers where they were bolted on.
- **One `FrameRequest` structure in, a list of blocks out**, with a `Placement` aggregate and `Gaf` as a class enum. Proposed on 2026-11-10 and rejected by the user the same day: the interface is to take primitive parameters as Linux's and Zircon's do, and a compound structure as a parameter is to be avoided. `Placement` (D2(b)) went with it.
- **A lock-free pin** (F1(b)): one atomic word in the header that a pin raises and a mover freezes, as Linux's `folio_ref_freeze()` does. Rejected: every reclaim pays a compare-and-swap, and nothing pins without already holding the owner's lock.
- **Keep the order with the caller** (F2(a)). Rejected: the block logic stays spread over the VMO and the object cache, and large pages add a third copy.
- **Static `PmNode` operations as the pipeline.** Rejected by D1.
- **A `Pmm` singleton**, as Zircon has. Rejected on 2026-10-28 (`pmm-layering` D2) and not reopened.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Where the pipeline across nodes lives | (a) static `PmNode::alloc_frames()` and `free_frames()`, with `mm::` adding admission; (b) the `mm::` free functions are the pipeline and `PmNode` keeps the member `alloc_frames(request)` | (b). It reverses the choice of 2026-11-04. Production never calls the static layer; it is a second entry that skips admission | None (homework paused) | **(b)**, 2026-11-10 |
| D2. How a caller says where | (a) paired overloads with and without a home, and a trailing `NodeMask`; (b) one `Placement{home, nodes}` aggregate; (c) one function with trailing primitive parameters, `home = ANY_NODE` and `nodes` | (b) at first | None | (b), 2026-11-10; reversed to **(c)** the same day by the user's rule against compound parameters |
| D3. Scope | (a) structure only, behaviour unchanged; (b) the behaviour debts too | (a) | None | (a), 2026-11-10; withdrawn the same day, when the user reopened the scope |
| D4. Whether a node mask is a public parameter | (a) yes, a trailing `nodes` on every allocation function, as today; (b) no: `Gaf::ThisNode` for one node, the thread's policy for the rest, the mask internal | (b). Production never passes one, and it is the fourth parameter of every signature | None | **(b)**, 2026-11-10, Claude's judgement as N1; can be reopened |
| F1. How a pin is made safe against movers | (a) the count is in the frame header, whatever the role, and is read and written under the owner's lock, which every mover also takes; (b) an atomic word that a pin raises and a mover freezes, with no owner lock | (a). Reclaim already adopts the owner, takes its lock and looks again; every mover follows the same path. The cost is that `pin_frame()` requires the owner's lock | None | **(a)**, 2026-11-10 |
| F2. Whether a block is a first-class thing | (a) the caller remembers the order and the object cache stamps tails; (b) the allocator stamps order and tails, `free_frames()` takes no order, `split_block()` is added | (b). Large pages and contiguous VMOs both take a block, split it and return the parts | None | **(b)**, 2026-11-10 |
| E1. The style of the public interface | (a) one `FrameRequest` structure and a list of blocks each way; (b) primitive parameters as in Linux and Zircon, one function per result kind | (a) at first | None | **(b)**, 2026-11-10, the user's rule |
| E2. What `Gaf` carries | (a) four bits, `Zero`, `CanWait`, `Movable` and `Dma32`, with `CanBlock` and `ThisNode` removed; (b) eleven bits covering every `gfp_t` dimension; (c) today's four bits kept, plus `Movable` and `Dma32` | (a). A microkernel has no in-kernel driver or filesystem, which is what the other `gfp_t` bits serve | None | **(c)**, 2026-11-10: the user keeps `CanBlock` and `ThisNode` |
| E3. What the layers under the seam are called | (a) `take_block()` and `return_block()`, the names of 2026-11-04; (b) `alloc_frames()` and `free_frames()` on `PmNode` and on `PmZone`, the seam's own pair | (b). The layers are private, so access says which is the entry and the name need not | None | **(b)**, 2026-11-10, the user's names. `PmNode::free_frames()` is added so that the node has the pair too |
| Z1. How far mobility grouping goes | (a) the request and `alloc_frames()` carry `MigrateType` now and the buddy keeps one list per order until large pages need more; (b) lists per order and type, a type per pageblock, and stealing whole blocks, now; (c) none | (a). It is a ceiling on large-page availability, so the user's to decide; (a) fixes the interface and leaves the mechanism until there is fragmentation to measure | None | **(a)**, 2026-11-10 |
| Z2. The order of the zone walk | (a) from the highest usable zone down, with low zones protected by a reserve; (b) from the lowest up, as today | (a) | None | **(a)**, 2026-11-10 |
| Z3. What each watermark means | (a) High stops reclaim, Low wakes it and is the floor with `CanBlock`, Min is the floor without it; (b) one floor | (a). It is what the `Watermark` comments and `Gaf::CanBlock` say they are for, and it gives the kept bit its reader: a caller that may block is stopped first, and one that may not keeps the band between Low and Min | None | **(a)**, 2026-11-10 |
| N1. What puts a request off, and what wakes it | (a) one threshold, the zone's Low floor; a release that lifts a zone above Low wakes a waiter; the gate keeps the watchdog's signal and the OutOfMemory refusal, and loses its delay level and Shut state; (b) the zone floor and the machine-wide delay level both, as today | (a). It revises "admission and waits use the global gate" of 2026-11-04: Z3 already makes the zone decide, and two thresholds for one decision can disagree | None | **(a)**, 2026-11-10. The user left N1 to N3 to Claude's judgement; any of them can be reopened |
| N2. Where the record of a short zone lives | (a) in its node, set by `wake_reclaim()` and read through `reclaim_target()`; (b) in `MemoryReclaimer`'s array indexed by node and zone type, as today | (a). It is state about the node's zones, and the reclaimer needs only a number from each node | None | **(a)**, 2026-11-10, as N1 |
| N3. How many reclaim threads | (a) one, serving each marked node in turn, as today; (b) one per node | (a) for now. One per node is a scaling choice and so the user's; nothing in the interface above depends on it | None | **(a)**, 2026-11-10, as N1 |

## Testing and verification

Each step is checked with `verify` on both images and the host tests. A step that only restructures must leave the test counts where they were; a step that changes behaviour brings its own test. For the frame: a block freed with no order returns the zone's free count and free-block profile to where they started; a block split to order 0 and freed frame by frame coalesces back to one block; a pinned page is refused by reclaim and reclaimed after the unpin; freeing a pinned frame asserts in a debug build.

What cannot be verified is that the design extends well. The check of that is whether each feature in the table above lands where the table says when it is built.

## Open questions

- `docs/repairs/261030/mm-pin.md` item 4: a read-only pin of a page-source page still needs a per-page bit to stay reclaimable after the unpin.
- Whether `alloc_frames_fragment()` keeps a production caller; today only tests call it and `alloc_frames_bulk()`.
