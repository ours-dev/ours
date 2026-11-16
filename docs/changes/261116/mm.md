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
