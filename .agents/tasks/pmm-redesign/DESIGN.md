# pmm-redesign: one allocation pipeline, and one job for each PMM class

**Date**: 2026-11-10
**Status**: Accepted

## Problem

The user asked for a redesign of the PMM as a whole: which classes exist and what each one's interface is. Earlier tasks (`pmm-layering`, `pmm-responsibilities`, `pmm-static`, `pmm-transfer`, `pmm-naming`) each fixed one seam. The overall shape still has four faults, measured on `a1d37028`:

- **The allocation entry exists twice with one signature.** `mm::alloc_frames()` calls the static `PmNode::alloc_frames()`, which calls the member `PmNode::take_block()`. The two outer layers differ only in the `Gaf::CanWait` admission and the test fault injection. No production code calls the static layer directly. Two tests do, 41 times (`kernel/tests/pm_node_test.cpp` 37, `kernel/tests/memory_pressure_test.cpp` 4), and they reach an entry that skips admission.
- **`PmNode` is still four things.** It is one node's state, the registry of all nodes (`NODES`, `STATES`, `NUM_NODES`), the allocator across nodes (the static allocation and release operations plus `observe_pressure()`), and the keeper of CPU membership (`publish_cpu()`, `publish_cpus()`, `claim_possible_cpus()`, `set_cpu_node()`). Outside the PMM's own files, 15 uses name `PmNode`, and none of them allocates: they want the registry, CPU membership or bring-up.
- **`pmm.hpp` declares eleven allocation functions for three contracts.** A block of `2^order` frames, a list of single frames, and an exact page count as blocks. Each exists once without a home node and once with one, each with a trailing `NodeMask`. In production an explicit home is passed three times (`cpu_local.cpp` twice, `object_cache.cpp` once) and a `NodeMask` never. The list form is spelled `alloc_frames(NodeId, Gaf, PmFrame::List *, usize)` and answers a `Status`, unlike its siblings.
- **Declarations and paths with nothing behind them.** `pin_frame()`, `unpin_frame()`, `cpu_to_node()` and `PmZone::find_frame()` are declared and never defined. The `BelowHighWmark` test in `PmZone::take_block()` has no writer, so it never fires. `apply_thread_policy()` takes the thread's resource lock and then does nothing in every branch. `FreeMemoryGate` is in an anonymous namespace in `pmm.cpp`, which is why `PmNode::observe_pressure()` is defined in `pmm.cpp` and not with the rest of `PmNode`.

## Goals and non-goals

Goals:

- One allocation pipeline with one entry, so admission cannot be walked around.
- One job for each class, so a placement question has one answer.
- A public allocation family with one function for each contract.
- Every step leaves behaviour unchanged and is committed on its own.

Non-goals, each recorded where it already lives:

- Watermark admission in the zone walk (`pmm-layering` D1(c), decided and not built).
- The order of the zone walk, which spends Dma first (`docs/repairs/261028/mm.md` item 1).
- Memory below 4 GiB and aligned contiguous allocation (`docs/repairs/261031/mm.md` items 1 and 2).
- The per-CPU frame cache and the gate's sum over every zone (`docs/repairs/261029/mm.md` item 3). Both are performance items and go through the user one at a time.
- `PageFrame`'s role-specific accessors, `EarlyMem`, `MemMap`, `FrameBuddy` and the locking. None of them moves.

## Design

### One allocation

```text
mm::alloc_frames(flags, order, placement)         pmm.cpp, the only entry
 1 FreeMemoryGate::admit()                        CanWait only; ShouldWait and OutOfMem leave here
 2 resolve_request(flags, order, placement, cpu)  pure function, answers an AllocRequest
 3 node(home)->take_block(request)                ZoneCandidates filters, PmZone::take_block(order) takes
 4 zeroing, locality counters, gate.observe()     all outside the zone lock
```

The list and fragment forms run steps 2 to 4 once per block inside one rollback scope and observe pressure once at the end, as `pmm-transfer` decided. The release path is `mm::free_frames()`, which finds the zone from the frame (`PmZone::at(frame.nid(), frame.zone())`) and observes pressure once.

A request that does not carry `Gaf::CanWait` skips step 1, as it does today, so a test that needs a frame while the gate is shut asks without the flag. The fault injection of `MM_TEST_ALLOC_FAILURE` stays at the entry, for requests that name no home.

The counterpart is Linux's `__alloc_pages()` in `mm/page_alloc.c`: a free function that runs `prepare_alloc_pages()` and then `get_page_from_freelist()`, while `pg_data_t` holds a node's zones and no allocation entry. Zircon's `pmm_alloc_page()` in `kernel/vm/pmm.cc` forwards to its single `PmmNode`; with several nodes there is no single node to forward to, so the free function is where the home is resolved.

### Classes

| Class | Its one job | Public interface | Change |
|---|---|---|---|
| `PmFrame` | one frame's descriptor | as today | none |
| `FrameBuddy` | split and merge, no lock | `allocate` `release` `free_count` `empty` | none |
| `PmZone` | the stock of one pfn span: lock, buddy, counters, watermarks | `take_block` `return_block` `num_free_frames` `count_free_frames` `free_blocks` `contains` `watermark` `watermark_ok`, the span and accounting getters; static `at` `row` `for_each` `create_all` | loses the dead gate, `Flags`, `find_frame` and `m_node`; the per-CPU cache scaffold stays |
| `ZoneList` | one node's fallback order, built at boot | `candidates(request)` `local_zone` `for_each_local_zone` `connect` | none |
| `ZoneCandidates` | the zones one request may use | a range | none |
| `AllocRequest` | one resolved, immutable request | its accessors; `resolve_request()` takes a `Placement` and absorbs `resolve_home()` and the thread policy | one resolver |
| `PmNode` | one node: its `ZoneList`, `PageQueues` and CPU masks | `take_block(request)` `zone_list` `page_queues` `work_cpus` `hard_affinity` `soft_affinity` `add_cpu` `remove_cpu` `nid` | the static allocation and release operations, the registry and CPU membership leave |
| node table | which nodes exist and in what state | free functions: `node(nid)` `for_each_node(state, f)` `for_each_nid(state, f)` the state masks `set_node_state` `num_nodes` | new home for `PmNode::NODES` and `STATES`, at file scope as `pmm-layering` D2(a) decided |
| `CpuNodeMap` | which node each CPU is on | as today, plus `publish` `publish_all` `claim_possible` `move(cpu, nid)` | takes `publish_cpu(s)`, `claim_possible_cpus` and `set_cpu_node` from `PmNode` |
| `FreeMemoryGate` | admission of requests that can wait, and the free-memory signal | `admit` `observe` `wait_for_opening` `pass_on` `set` | moves to `priv/free_memory_gate.hpp`; `PmNode::observe_pressure()` goes |

`CpuNodeMap::move()` is three objects wide (two nodes' CPU masks, each under its own node lock, and the table), which is why `PmNode` gains `add_cpu()` and `remove_cpu()`: each takes the node lock for its own mask, and the order between the three stays written in one function.

### Public interface

```cpp
/// Where a request may be served from. The default is the calling CPU's node
/// first and every online node after it.
struct Placement {
    NodeId home = ANY_NODE;
    NodeMask nodes = node_online_mask();
};

auto alloc_frames(Gaf, usize order = 0, Placement = {}) -> ktl::Result<PmFrame *>;
auto alloc_frames_bulk(Gaf, usize n, Placement = {}) -> ktl::Result<PmFrame::List>;
auto alloc_frames_fragment(Gaf, usize n, Placement = {}) -> ktl::Result<PmFrame::List>;
auto wait_for_frame_allocation(Gaf, time::Deadline const &, bool interruptible)
    -> ktl::Result<PmFrame *>;

template <typename T = u8>
auto get_frames(Gaf, usize order = 0, Placement = {}) -> T *;

auto free_frames(PmFrame *, usize order = 0) -> void;
auto free_frames(PmFrame::List *) -> void;
auto free_frames(VirtAddr, usize order = 0) -> void;
auto unwire_frame(PmFrame *) -> void;
```

`alloc_frames_n()` and `get_frames_n()` stay as inline helpers over `get_order()`. `alloc_page_table()`, `free_page_table()`, `set_free_memory_signal()`, `approx_free_frame_count()` and `free_frame_count()` do not change.

Removed: `alloc_frames(Gaf, PhysAddr *, usize)`, which only tests call and `frame_to_phys()` replaces; `pin_frame()`, `unpin_frame()` and `cpu_to_node()`, which have no definition. The node queries (`current_node()`, `node_work_cpus()`, the affinity getters, the masks and the counts) move to the node table's header, which `pmm.hpp` includes until its users include it themselves.

`Gaf::ThisNode` keeps its contract: the resolver intersects `Placement::nodes` with the home node.

### Locking

Nothing changes. The zone lock covers the buddy and the free-frame count together, the node lock covers `m_work_cpus`, the gate's allocation side reads atomics and takes no lock, and an `AllocRequest` is a stack value one thread owns.

## Alternatives considered

- **Keep the static `PmNode` operations as the implementation** (the choice of 2026-11-04). Rejected by D1: it keeps two entries with one signature, and it keeps `PmNode` both one node and all nodes.
- **A `Pmm` singleton owning the node table, the gate and the CPU map**, as Zircon's `Pmm` does. Rejected on 2026-10-28 (`pmm-layering` D2) and not reopened: it is an object whose only job is to hold things.
- **Keep the paired overloads.** Rejected by D2: three production calls name a home and none names a mask, and every new constraint would double the family again.
- **Do the behaviour debts in the same pass.** Rejected by D3: a step that changes what an allocation returns cannot be checked by an unchanged boot.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Where the pipeline across nodes lives | (a) static `PmNode::alloc_frames()` and `free_frames()` are the implementation and `mm::` adds admission; (b) the `mm::` free functions are the pipeline and `PmNode` keeps the member `take_block(request)` | (b). It reverses the choice of 2026-11-04. Production never calls the static layer; it is a second entry that skips admission | None (homework paused) | **(b)**, 2026-11-10 |
| D2. How a caller says where | (a) paired overloads with and without a home, and a trailing `NodeMask`; (b) one `Placement{home, nodes}` aggregate defaulting to `{}` | (b). A later constraint such as an address ceiling is one more field | None | **(b)**, 2026-11-10, with the name `Placement` |
| D3. Scope of this pass | (a) structure only, behaviour unchanged; (b) the behaviour debts too | (a). Functional debts follow as their own tasks; performance items go through the user one at a time | None | **(a)**, 2026-11-10 |

## Testing and verification

Every step is checked with `verify` on both images and the host tests, and the test counts must equal those of the step before. The tests that call the static `PmNode` operations move to `mm::` with a `Placement`, and their cases must pass unchanged: local-first placement, an explicit home, `ThisNode`, an excluded home, rollback of a failed list or fragment, and the pressure protocol.

What cannot be verified is that the layering is better. The check of that is whether watermark admission, the zone order and an address ceiling each land in one place afterwards.

## Open questions

- The node table's header name and whether `PmNode::State` keeps its name once the masks are no longer `PmNode`'s.
- Whether `PmNode::contains()` and `PmNode::dump()` keep a caller once the tests go through `mm::`.
