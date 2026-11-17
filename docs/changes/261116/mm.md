# mm changes

## The draining tests share a ZoneTrial, and the layout tests read the zone's free-block profile (2026-11-16)

**Files**
- `kernel/mm/include/ours/mm/pm_zone.hpp`, `kernel/mm/pm_zone.cpp` — `free_blocks(order)` gives way to `free_block_profile()`, which returns a `PmZone::FreeBlockProfile` (free blocks at each order and free frames) read in one hold of the zone lock; `with_block()` gives the profile one more free block
- `kernel/tests/pm_node_test.cpp` — a test-local RAII `ZoneTrial` (zone, `MemoryReclaimer::Pause`, held frames) with `drain_to()`, `keep()` and `hand_over()`, used by the five draining tests; `AdmissionHoldsAtLowThenAtMin` uses `remote_single_zone_node()` instead of its own copy; `ABlockCarriesItsLayoutAndCoalescesBack` splits to order 0 and frees frame by frame; `AdoptedFramesReturnToTheirZone` compares whole profiles; new `AContiguousRunGivesItsSurplusBack`
- `kernel/tests/pm_zone_test.cpp` — `FreedBlocksCoalesceAcrossSections` compares profiles instead of its own per-order counts

**Symptom**
Latent, in the tests. Five tests each picked a zone, paused the reclaimer, kept a list of what they took and pushed every allocation onto it with an `if (r) { held.push_back(**r); }`, about ten times, then freed the list by hand; an early return between a drain and that free would have left a remote zone drained for every test after it. `AdmissionHoldsAtLowThenAtMin` had its own copy of the node pick. And the design's test plan for the frame layout was half covered: no test split a block to order 0 and freed it frame by frame through the seam, and nothing checked that `alloc_frames_contiguous()` gives its surplus frame back. `SplitBlockMakesBlocksThatAreFreedAlone` said itself that its frees would not catch a part returned at a wrong order.

**Cause**
The protocol "hold the zone still, take frames, give them all back" was an invariant every test kept by hand, and C++ keeps such an invariant for you only when an object owns it: a destructor runs on every way out of a scope, a hand-written free at the bottom of a test runs only on the way the author thought of. The frames must also go back while reclaim is still paused, so that the zone is whole before the reclaimer may look at it; `~ZoneTrial()` gives them back in its body, and C++ runs a destructor's body before it destroys any member, the Pause among them.

The layout plan was half covered because the only thing that can see a coalescing mistake is the zone's free-block profile, and each test that wanted one built it by hand, one `free_blocks(order)` call, and so one lock hold, per order. A block in mid-merge on another CPU between two of those holds is counted at two orders or at none, so the profile was never one moment's. A free that hands the buddy a part at a wrong order (but still aligned) passes every assertion; only the profile shows it, as frames counted twice.

**Change**
The protocol is an object:

```cpp
class ZoneTrial {
  public:
    explicit ZoneTrial(PmZone &zone);
    ~ZoneTrial();                                  // mm::free_frames(&m_held)
    auto drain_to(usize target) -> bool;           // through the zone, which has no floor
    auto keep(ktl::Result<PmFrame *> const &) -> void;
    auto hand_over() -> PmFrame *;                 // one held frame back to the test
  private:
    PmZone &m_zone;
    mm::MemoryReclaimer::Pause const m_no_reclaim; // destroyed after the body has freed
    PmFrame::List m_held;
};

// before, about ten times over five tests
auto below_low = mm::alloc_frames(blocking, 0, nid);
if (below_low) { held.push_back(**below_low); }
// after
auto const below_low = mm::alloc_frames(blocking, 0, nid);
trial.keep(below_low);
```

It is not a `KTEST_F` fixture: a fixture is one static instance for every test that names it, which would hold the Pause for good.

The profile is a value the zone gives in one lock hold:

```cpp
// before: one lock hold per order, in each test
for (usize order = 0; order <= MAX_FRAME_ORDER; ++order) { blocks[order] = zone.free_blocks(order); }
// after
auto const before = zone.free_block_profile();
...
EXPECT_TRUE(zone.free_block_profile() == before, ...);
EXPECT_TRUE(zone.free_block_profile() == without.with_block(0).with_block(1).with_block(2), ...);
```

`ABlockCarriesItsLayoutAndCoalescesBack` now splits an order-3 block into blocks of two, splits the lower two of those into single frames, frees the upper two by their heads and the four single frames one by one, two top down and two bottom up, so the merge steps both ways, and compares profiles. `AContiguousRunGivesItsSurplusBack` takes three frames aligned to four pages from a quiet remote node, checks that the zone lost three frames and not four, frees the run and checks the profile is back.

The type is on `PmZone` rather than in the tests because only the zone can read every order in one hold of its lock, and because both test files need it: `free_blocks(order)` had no caller outside the tests, so the diagnostic surface is no larger than it was.

**Reference**
None for the trial: test structure. The profile is the reading Linux's `/proc/buddyinfo` gives: `frag_show_print()` in `mm/vmstat.c` prints each order's `free_area[order].nr_free` for one zone, called by `walk_zones_in_node()` with `zone->lock` held across all of them.

**Verification**
`verify` in the `feat/pmm-redesign` worktree on the default and the four-node machine: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed. `ktest all`: default 95 ran, 84 passed, 11 skipped (the remote-node tests skip on one node); four nodes 95 ran, 92 passed, 3 skipped, one test more than before. Failure-injection build: 95 ran, 85 passed, 0 failed, with the three known `Failed to map` lines. clang-tidy reports nothing new in the three files. Controls, on the four-node machine: with `alloc_frames_contiguous()` keeping its surplus frame, `ContiguousFramesAreAdjacentAlignedAndFreedAlone` still passes and `AContiguousRunGivesItsSurplusBack` fails both its checks; with one single frame left unfreed, `ABlockCarriesItsLayoutAndCoalescesBack` fails on the profile.

A narrow pre-commit review (three reviewers, each finding checked by a second agent) confirmed that every test keeps its old behaviour, that the free orders and the exact profile equalities hold for the buddy as written, and that the frame counter `free_block_profile()` reads moves only under the zone lock on every path that runs today (the per-CPU cache branches that would not are dead while `MAX_PCPU_CACHE_ORDER` is 0). It found that the first draft credited `ZoneTrial`'s member order with freeing before the Pause ends, which the destructor body does whatever the order (comment and this record fixed), and that `pm_zone_test.cpp`'s failure log compared each order with the profile before rather than the one expected, hiding the missing max-order block and the frame count (now compared with the expected profile, frames included). Moving `WaitingRequestsStopAtLowAndWakeOnRelief` to the trial showed a leak of its own: the frame of the request that cannot wait was never freed if it came from another node. The trial holds it now, and the review confirmed the leak closed.

**Deliberately not done**
`check_exhausted_node()` keeps its own held list: it drains a whole node through `mm::` and then each of its zones, not one zone, and runs only with the `exhaust-remote` option. `SplitBlockMakesBlocksThatAreFreedAlone` stays on the calling CPU's node, so that the split is checked on a machine with one node too; the profile check of its frees is the remote-node test's.

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, M11 and M12, from the test reviewer.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must `ZoneTrial` free its frames before its Pause ends, and what rule of C++ makes its destructor do that whatever order its members are in?
- Why can a free at a wrong order that is still aligned pass every assertion, and what in the free-block profile shows it?
- Why are the per-order counts read in one hold of the zone lock rather than one hold per order?

## The placement policy answers a home and a node set, and only the resolver asks it (2026-11-16)

**Files**
- `kernel/mm/include/ours/mm/priv/alloc_policy.hpp` — `AllocPolicy::Placement` (home node, node set) and `place(local, allowed)`; Default and Local leave both as they are, the other kinds are asserted unbuilt; `m_kind` defaults to `Default` instead of being indeterminate
- `kernel/mm/include/ours/mm/priv/alloc_request.hpp` — `resolve_request()` takes an optional policy and asks it unless the flags carry `Gaf::ThisNode`
- `kernel/mm/pmm.cpp` — `apply_thread_policy(Gaf &)` is gone; `resolve_home(nid)` returns a `Home` (node, and the thread's policy when no node is named), read by `thread_policy()`; `alloc_block()` starts the walk at the request's home node
- `kernel/mm/tests/alloc_request_smoke.cpp` — a default policy leaves the placement, and `ThisNode` keeps the policy out
- `kernel/include/ours/task/thread.hpp` — the resource lock's comment names `thread_policy()`
- `docs/repairs/261116/mm.md` — item 1, the resource lock every node-less allocation takes, to measure first
- `.agents/tasks/pmm-redesign/DESIGN.md`, `.agents/skills/kernel-design/references/mm.md` — the rule

**Symptom**
Latent: no policy can be set yet. `apply_thread_policy()` switched over `AllocPolicy::kind()` with every case empty, and its only output was the `Gaf &` it was handed. A policy's answer is a node (Preferred, Interleave) or a node set (Bind), and neither fits in a `Gaf`, so no policy could have reached a request however it was written. It also read `Gaf::ThisNode` out of the raw flags before any request existed, the last such read above the request after review M4.

**Cause**
The policy was a switch outside the policy: the code that knows what Bind means was meant to live in the allocator's `switch`, not in the policy object, and the switch's output type was chosen before the answers were known. An object should answer the question it owns ("where does this allocation go?") in a type that can hold the answer, and the one place that decodes `Gaf` (the resolver) should be the one that decides whether the policy is asked at all.

**Change**

```cpp
// before: pmm.cpp, for every allocation that names no node
auto apply_thread_policy(Gaf &alloc_flags) -> Status {
    if (!!(alloc_flags & Gaf::ThisNode)) { return Status::Ok; }   // raw read
    auto const policy = /* lock, copy the Arc */;
    if (policy) { switch (policy->kind()) { case Kind::Bind: break; /* ... */ } }
    return Status::Ok;
}

// after: the policy answers, the resolver asks
struct Placement { NodeId home; NodeMask nodes; };
auto AllocPolicy::place(NodeId local, NodeMask const &allowed) const -> Placement;

inline auto resolve_request(Gaf flags, usize order, NodeMask const &nodes, Origin origin,
                            ustl::Option<AllocPolicy const &> policy = ustl::NONE) -> AllocRequest {
    auto allowed = nodes;
    if (!!(flags & Gaf::ThisNode)) {
        allowed &= NodeMask::of(origin.node);
    } else if (policy) {
        auto const placement = policy->place(origin.node, nodes);
        origin.node = placement.home;
        allowed = placement.nodes;
    }
    ...
}
```

`resolve_home()` fetches the policy only when no node is named, as before; `alloc_block()` passes it to `resolve_request()` once per block, so a policy that moves the home per allocation (Interleave) can, and starts the walk at `find_node(request.home_node())`. `resolve_request()` stays pure: the policy is an argument, so the host test drives it. The one place the lock is now taken where it was not: an allocation that names no node and carries `ThisNode`, which no production caller makes. `Placement` is a return value inside the private pipeline, not a parameter: the aggregate the user rejected on 2026-11-10 was a public parameter, and this was put to them before it was built.

**Reference**
Linux `alloc_pages_mpol()` in `mm/mempolicy.c`: the task's policy (`get_task_policy()`) becomes a node and a node mask (`policy_nodemask()`) before `__alloc_pages()` is called, and `__GFP_THISNODE` bypasses it. `place()` is that conversion; the policies' own cases (`MPOL_PREFERRED`, `MPOL_BIND`, `MPOL_INTERLEAVE`) are not built.

**Verification**
`verify` in the `feat/pmm-redesign` worktree on the default and the four-node machine: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed, `alloc-request-smoke` with its two new checks among them. `ktest all`: default 95 ran, 84 passed, 11 skipped; four nodes 95 ran, 92 passed, 3 skipped. Failure-injection build: 95 ran, 85 passed, 0 failed, with the three known `Failed to map` lines. clang-tidy reports nothing in `pmm.cpp`. Nothing inside the kernel calls `place()` yet, since every thread's policy is null; the host test is what runs it.

A narrow pre-commit review (two reviewers, each finding checked by a second agent) found no change in behaviour on any entry with a null policy, the only state reachable, and no way for the walk's start node to be null. It found that the comment on `thread_policy()` and the repair item named a writer that does not exist ("what another thread takes to replace it"), the wording `docs/changes/261004/comment-rule.md` had already corrected once in `thread.hpp`, and that the repair item's candidates left out the one its own Linux reference shows: only the thread itself replaces its policy, so its own reads take no lock. Both fixed: the comment states the rule, and the item lists that candidate and ties it to the setter's design. Two findings were refuted: that `place()` being const blocks Interleave (where its cursor lives is the next step's decision, and the call is already once per block), and that `place()`'s contract must already promise a registered home and a subset of `allowed` (true of every kind built; to be written when Preferred and Bind are).

**Deliberately not done**
Preferred, Bind and Interleave, a setter, and the lock: the user approved the interface alone for this step, then asked for the policy itself to be built next, which needs their decisions first. The lock is a performance item (`docs/repairs/261116/mm.md` item 1).

**How it was found**
`.agents/tasks/pmm-redesign/REVIEW.md`, M7, from the OOP reviewer.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why could no policy have reached a request through `apply_thread_policy(Gaf &)`, however its cases were written?
- Why does `Gaf::ThisNode` keep the policy out, and why is the resolver the place to decide that?
- Why is the policy asked once per block rather than once per call, and which policy needs that?

## Preferred, Bind and Interleave are built, set by the thread itself, and a bound request is confined (2026-11-16)

**Files**
- `kernel/mm/include/ours/mm/priv/alloc_policy.hpp` — `AllocPolicy` gains the factories `local()`, `preferred(node)`, `bind(nodes)`, `interleave(nodes)`, const members (the policy is immutable), and a `place(local, allowed, turn)` that answers for every kind; the unused `m_prev_nid` and `m_allowed_nodes` are gone
- `kernel/mm/include/ours/mm/alloc_state.hpp` — `mm::set_thread_policy()`, the calling thread's only setter; the thread's turn counter (`policy_turn()`, `count_policy_turn()`)
- `kernel/mm/include/ours/mm/priv/alloc_request.hpp` — a private `Resolved` flags word (`PolicyPlaced`, `PolicyNarrowed`); `placed_by_policy()`; `is_confined()` counts a request the policy narrowed; `resolve_request()` takes the turn
- `kernel/mm/pmm.cpp` — `set_thread_policy()`; `alloc_block()` hands the policy the thread's turn, counts it only when the request says the policy placed it, and refuses with `InvArgs` a home no node answers for
- `kernel/mm/tests/alloc_request_smoke.cpp`, `kernel/tests/pm_node_test.cpp` — each kind on the host; on the four-node machine, `APlacementPolicyPlacesNodeLessAllocations` and `ABoundWaitingRequestGoesOnToMin`
- `docs/repairs/261114/mm.md` — item 2 (a narrowed node set not treated as confined) deleted: fixed here
- `docs/repairs/261116/mm.md` — item 1 rewritten for the self-only setter, which makes a lockless own-thread read a real candidate
- `.agents/skills/kernel-design/references/mm.md`, `.agents/tasks/pmm-redesign/DESIGN.md` — the rules

**Symptom**
Latent: no thread could have a placement policy. The previous step gave the policy an answer it could carry; there was still nothing to answer with and no way to set one.

**Cause**
Not a mistake but the step the user asked for (option C of M7, 2026-11-16, with the four recommended choices). Each choice has a reason worth keeping:
- Only the calling thread sets its policy. Then its own allocations can never race a replacement, so the lock on the read path protects only readers on other threads; that is what Linux relies on to read `current->mempolicy` without a lock.
- The policy object is immutable and the Interleave position is the thread's. An immutable object shared through an `Arc` needs no synchronisation of its own; a shared cursor inside it would need an atomic read-modify-write on every allocation, and two threads sharing one policy would steal each other's turns.
- A request Bind narrowed is confined. The gate and the watchdog count the whole machine. A request that may use only node N can find N's zones short while the machine's count is healthy, and then nothing machine-wide is bound to end a wait at Low: the hang the rule already closed for `ThisNode`. A node set the caller narrowed (a test's stand-in for a smaller machine) still does not count, which is why the request records that the policy narrowed it rather than comparing masks.
- A turn is counted only for a request the policy placed. Counting before the resolver decided would let a `ThisNode` request, which the policy never sees, take a turn: alternate the two under Interleave{A, B} and every ordinary allocation lands on B.

**Change**

```cpp
// the kinds, as place() answers them
case Kind::Preferred:  if (allowed.test(m_preferred)) { return {.home = m_preferred, .nodes = allowed}; } break;
case Kind::Bind:       return {.home = local, .nodes = allowed & m_nodes};
case Kind::Interleave: { auto const round = allowed & m_nodes;
                         if (round.any()) { return {.home = nth_node(round, turn % round.count()), .nodes = allowed}; }
                         break; }

// alloc_block(): the turn is read, handed over, and counted only if used
turn = state->policy_turn();
auto const request = resolve_request(flags, order, nodes, origin, policy, turn);
if (request.placed_by_policy()) { state->count_policy_turn(); }

// the setter: the calling thread's, swapped under its resource lock with interrupts off
auto set_thread_policy(ustl::Arc<AllocPolicy> policy) -> ustl::Arc<AllocPolicy>;   // returns the old one
```

The old policy is handed back rather than dropped under the lock, since the last reference frees it. Setting a policy starts the turns again, so an Interleave begins on its lowest node. Bind leaves the home on the calling CPU's node so that the walk still goes nearest first through the bound nodes; an empty intersection leaves nothing to allocate from, and the allocation fails.

**Reference**
Linux `mm/mempolicy.c`: `policy_nodemask()` (`MPOL_BIND` gives the policy's nodes as the mask, `MPOL_PREFERRED` gives its node as the preferred nid), `interleave_nodes()` (`next_node_in(current->il_prev, pol->nodes)`), `do_set_mempolicy()` (only `current`, under `task_lock()`, and it resets `il_prev`), `alloc_pages_noprof()` (the task policy is skipped for `__GFP_THISNODE` and `in_interrupt()`). Departures: Interleave picks by a turn modulo the allowed nodes, as `interleave_nid()` does for an index, rather than stepping from the last node, which is the same while the node set does not change; confinement of a bound request is this project's own rule, since Linux has no gate.

**Verification**
`verify` in the `feat/pmm-redesign` worktree on the default and the four-node machine: dbg 46 passed and 5 skipped, rel 44 passed and 8 skipped with 7/7 required, host tests 185 passed, `alloc-request-smoke` checking each kind. `ktest all`: default 97 ran, 84 passed, 13 skipped (the two new tests skip on one node); four nodes 97 ran, 94 passed, 3 skipped. Failure-injection build: 97 ran, 85 passed, 0 failed, with the three known `Failed to map` lines. clang-tidy reports nothing in `pmm.cpp`. Controls, each reverted: Interleave always taking its first node fails the host check and the kernel test; `is_confined()` ignoring `PolicyNarrowed` fails the host check and `ABoundWaitingRequestGoesOnToMin` (the bound request is answered `ShouldWait` instead of `OutOfMem` at Min); the setter not resetting the turns fails `APlacementPolicyPlacesNodeLessAllocations`, which takes three turns before its Interleave for that reason.

A narrow pre-commit review (three reviewers, each finding checked by a second agent) confirmed the turn spent by a `ThisNode` request (fixed: `PolicyPlaced` and `count_policy_turn()`), that the kernel test could not tell whether setting a policy resets the turns (fixed: an odd number of turns before the Interleave, and the control above), that its Preferred and Bind checks could not fail when the thread ran on the target node (fixed: the target is a node other than the calling CPU's), that deleting the old repair item needed its own acceptance test (added: `ABoundWaitingRequestGoesOnToMin`), and that the repair items, the design notes and this record had not been updated in the change. Refuted: that `ThisNode` should still apply Bind (it keeps every policy out, as Linux's `__GFP_THISNODE` does), that an empty Bind must not ask the gate first, and that interrupt-context allocations are a defect today (none allocates with no node named).

**Deliberately not done**
A syscall to set a policy: the setter is kernel-internal, and a user-space interface is its own work. Interrupt context: an allocation made there while a thread is current would read that thread's policy and count a turn; nothing allocates there with no node named, and there is no in-interrupt query to keep it out, so the comment on the counter says so. The per-allocation lock stays, as `docs/repairs/261116/mm.md` item 1, to be measured first. `VmMapping`'s and `PmNode`'s unused `AllocPolicy` members are left as they were.

**How it was found**
None: a feature, `.agents/tasks/pmm-redesign/REVIEW.md` M7, option C.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a setter only the calling thread may use let that thread read its policy without a lock, and what would another-thread setters cost?
- Why is a request Bind narrowed confined while a mask the caller narrowed is not, and what would hang without the rule?
- Why must the turn be counted after the resolver, and what goes wrong with Interleave if a `ThisNode` request takes one?
