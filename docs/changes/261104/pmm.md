# pmm changes

## Cover allocation failures and rollback (2026-11-04)

**Files**

- `kernel/commands/pm_node_test.cpp` covers allocation errors, node queries and remote exhaustion.
- `kernel/commands/memory_pressure_test.cpp` covers the wait deadline and stale watcher bounds.
- `kernel/mm/tests/frame_buddy_smoke.cpp` and `kernel/mm/tests/BUILD.gn` cover buddy merge restrictions with node IDs enabled in the host test.

**Symptom**

A batch could leak pages after a partial failure while the existing successful
batch tests still passed. The wait gate's deadline and watcher-registration
failures also lacked regression checks.

**Cause**

Successful requests never take the rollback path. An empty node mask exercises
failure before acquisition, so it cannot show whether the allocator gives back
a page it has already acquired.

**Change**

The opt-in `pmnode.test --exhaust-remote` drains eligible zones on a memory-only
node, verifies fallback, and releases one frame there. A two-page bulk request
and a fragment request whose second block exceeds the maximum order must each
fail and leave that exact frame allocatable again. The host buddy fixture keeps
adjacent blocks separate when their order, zone or node differs.

**Reference**

The tests exercise this tree's PMM contracts.

**Verification**

LLVM coverage for `pmm.cpp`, `pm_node.cpp`, `pm_zone.cpp` and `frame_buddy.cpp`:

| QEMU configuration | Lines before | Lines after | Branches before | Branches after |
| --- | ---: | ---: | ---: | ---: |
| Default | 545/709 | 587/709 | 188/295 | 209/295 |
| Four NUMA nodes | 555/709 | 606/709 | 193/295 | 220/295 |
| Four NUMA nodes plus the remote-exhaustion run | | 611/709 | | 224/295 |

The remote run used `NUMA=3 SMP=2`. The host buddy test reached 56/56
implementation lines and 31/31 header lines. Both dev images booted in the
default and four-node configurations. All 163 host tests passed. Clang-tidy
completed with existing diagnostics outside the new code.

**Deliberately not done**

The coverage totals include startup failures, unfinished PMM paths and race
outcomes that these tests do not induce. The test change leaves these paths
in the measurement.

**How it was found**

`scripts/cov_kernel.py` and `llvm-cov export` identified the uncovered branches
in the eight PMM and VM regression commands.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Rename the conditional home-node allocation flag (2026-11-04)

**Files**

- `kernel/mm/include/ours/mm/alloc_flags.hpp` and `kernel/mm/include/ours/mm/priv/alloc_request.hpp` name the flag and describe its mask rule.
- The PMM allocation comments, kernel tests and host request tests use the same name.

**Symptom**

`ThisNode` reads as an unconditional node restriction at the call site.

**Cause**

The allocation resolver intersects the allowed mask with the home node and uses
the intersection only when it contains that node.

**Change**

Rename the flag to `AllocFlags::HomeIfAllowed`. The flag still occupies bit 9.
When the mask includes the home node, that node is the sole candidate. When
the mask excludes it, the resolver uses the caller's mask.

**Reference**

Linux documents `__GFP_THISNODE` as enforcing the requested node without
fallback. The conditional mask rule in this tree has its own name.

**Verification**

The host request test exercises masks that include and exclude the home node.
All 163 host tests passed. Both QEMU dev images booted in the default and
four-node NUMA configurations. The remote exhaustion case passed with
`NUMA=3 SMP=2`. Clang-tidy completed on the six touched C++ files.

**Deliberately not done**

Changes to placement policy are outside this rename.

**How it was found**

The PMM coverage review identified the mask-excludes-home case in the
existing host test.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Make ThisNode a strict NUMA allocation constraint (2026-11-04)

**Files**

- `kernel/mm/include/ours/mm/alloc_flags.hpp` defines the `ThisNode` flag.
- `kernel/mm/include/ours/mm/priv/alloc_request.hpp` restricts eligible zones to the selected node.
- `kernel/mm/pm_node.cpp` skips the thread's NUMA policy for pinned requests.
- The host request test and `pmnode.test` exercise conflicting masks and node exhaustion.

**Symptom**

A pinned request could allocate on a different node when the caller's mask
excluded its home node.

**Cause**

The previous resolver applied the singleton intersection only when it was
nonempty. Otherwise the remote nodes in the caller's mask stayed eligible.

**Change**

`ThisNode` always applies `allowed &= NodeMask::of(origin.node)`. The eligible
set can therefore be empty. The caller's explicit node selects the home. An
implicit request uses the current CPU's node and bypasses thread placement
policy. The PMM returns `OutOfMem` when the strict walk finds no suitable block.

**Reference**

Linux `gfp_zonelist()` and `build_thisnode_zonelists()` provide the no-fallback
zone walk. `prepare_alloc_pages()` passes an explicit nodemask to the zone
iterator, and `alloc_frozen_pages_noprof()` skips task policy with
`__GFP_THISNODE`.

**Verification**

The host request test covers an allowed home, a conflicting mask and an empty
mask. The remote NUMA test checks strict failure and ordinary fallback after
exhausting the same node. All 163 host tests passed. Both QEMU dev images
booted in the default, NUMA, and NUMA with allocation policy enabled
configurations. Clang-tidy completed on all six changed C++ files.

**Deliberately not done**

Linux's reclaim and cpuset mechanisms are outside this change.

**How it was found**

The previous request test exposed the difference from the Linux flag by
asserting that a remote-only mask stayed eligible.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Restore the Gaf type name (2026-11-04)

**Files**

- `kernel/mm/include/ours/mm/alloc_flags.hpp` declares `Gaf`.
- The PMM, VM, heap and KTL interfaces and tests use the renamed type.

**Symptom**

The frame-allocation flag type was named `AllocFlags`.

**Cause**

The earlier allocation vocabulary change had renamed the historical `Gaf`
enum. The user chose to restore that name.

**Change**

`enum class Gaf : u32` replaces `enum class AllocFlags : u32`. The typed
PMM, VM and heap entry points take `Gaf`, including the heap placement-new
overloads and the type exported by KTL.

**Reference**

The name comes from this tree's original frame-allocation interface.

**Verification**

Clang-format and clang-tidy checked the 38 edited C++ files. The default
and four-node NUMA dev images booted, and all 163 host tests passed.
The memory-only NUMA regression completed all 69 checks.

**Deliberately not done**

The change is scoped to the type name and its references.

**How it was found**

None.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Separate the zone walk from block transfer (2026-11-04)

**Files**

- `kernel/mm/include/ours/mm/pm_node.hpp` exposes the ordered candidate range.
- `kernel/mm/pm_node.cpp` completes each allocation and validates returns through the shared entry.
- `kernel/mm/pm_zone.cpp` withdraws and returns blocks through the zone inventory.
- `kernel/commands/pm_node_test.cpp` checks the locality counter for an exhausted remote home.

**Symptom**

The locality counter used the requested home even when the allocator
fulfilled the request on another node. The zone entry also took the full
placement request although inventory withdrawal consumed its order and flags.

**Cause**

Candidate filtering, block withdrawal and completion were expressed as a
chain of overlapping allocation methods. The old completion method compared
the current node to the home instance and ignored the returned frame.

**Change**

`ZoneList::candidates(request)` supplies eligible zones in fallback order.
`PmZone::take_block(order)` withdraws an available block. `PmNode` zeroes it
when requested after the zone lock is released, and classifies it with
`frame->nid() == initiator_node`. Single-frame and list releases share the
queue-ownership check and call `return_block()` on the frame's zone.

**Reference**

Zircon `pmm_alloc_page()` separates caller-facing adaptation from allocation.
Linux `get_page_from_freelist()` selects an available zone from the eligible
order. The local NUMA accounting uses the returned descriptor.

**Verification**

Clang-format and clang-tidy checked ten C++ files. Both dev images booted in
the default and four-node configurations. All 163 host tests passed.
`NUMA=3 SMP=2` passed all 69 PM node checks, nine PM zone checks and 17
pressure checks. The regression observed one local hit after an allocation
with an exhausted remote home returned a local frame.

**Deliberately not done**

Batch ownership and pressure publication have their own design proposal.

**How it was found**

The PMM responsibility review traced the NUMA counter from the returned frame
back to the home node's completion method.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Unify block allocation and pressure admission (2026-11-04)

**Files**

- `kernel/mm/include/ours/mm/pmm.hpp` exposes a result from each single-block allocation overload.
- `kernel/mm/pmm.cpp` checks pressure in the common node-taking overload.
- VM, heap, boot and test callers consume the result through the unified interface.

**Symptom**

Callers used separate names for the same block allocation. The raw-pointer form collapsed every failure into null, while `try_alloc_frames()` carried the pressure gate result.

**Cause**

The earlier split used a second name to expose `ShouldWait` without migrating the raw-pointer callers.

**Change**

`mm::alloc_frames()` returns `ktl::Result<PmFrame *>` for implicit and explicit home requests, including the optional physical-address output. A request with `Gaf::CanWait` checks the gate before passing the immediate allocation to `PmNode`. The address output is written after success. The VMO fault path releases its locks before calling the existing single-page wait, which retries through `alloc_frames()`.

**Reference**

Zircon `pmm_alloc_page()` reports status alongside a page result. This tree exposes both through `ktl::Result`.

**Verification**

Clang-format and clang-tidy checked 18 C++ files. Both dev images booted in the default and four-node NUMA configurations, and all 163 host tests passed. The nine targeted kernel regressions passed. The two-CPU, three-node run also passed the 69 PM node checks and 18 pressure checks. It verified `ShouldWait` across the block overloads, preservation of the address output, and propagation of `InvArgs` and `AccessDenied`.

**Deliberately not done**

The batch and fragment allocation contracts remain immediate.

**How it was found**

The interface review traced both block entries to the same `PmNode` operation and compared what each could report to its caller.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Commit zone inventory and its count under the same lock (2026-11-04)

**Files**

- `kernel/mm/pm_zone.cpp` updates the atomic count with the buddy mutation under the IRQ-save zone lock.
- `kernel/commands/pm_node_test.cpp` exercises the counter on an isolated one-page inventory from two CPUs.

**Symptom**

A pressure reader can observe a wrapped unsigned count while a zone is empty.

**Cause**

The return used to publish the block in the buddy and release the lock before adding to the count. A concurrent withdrawal could remove the returned block and subtract from zero before the return recorded its increment.

**Change**

Withdrawals and returns apply their count deltas in the same critical section as the buddy operation. Pressure sampling keeps its lock-free view of each zone's count.

**Reference**

Zircon `PmmNode::alloc_page()` removes the free page and decrements the count with the same lock token.

**Verification**

The default and four-node NUMA dev images booted and all 163 host tests passed. On two CPUs with a memory-only node, the free count stayed within the zone's one available page during concurrent transfers and agreed with the buddy walk afterwards. PM node, zone, and pressure commands passed.

**Deliberately not done**

The unfinished per-CPU cache has a separate future accounting contract.

**How it was found**

Traced the release of the zone lock against the order of the atomic count updates.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Sample pressure at the end of a PMM operation (2026-11-04)

**Files**

- `kernel/mm/pm_node.cpp` separates internal transfers from completed allocation and return operations.
- `kernel/mm/pmm.cpp` supplies the private pressure observation point.
- `kernel/commands/memory_pressure_test.cpp` checks direct operations and rollback under an armed watcher.

**Symptom**

The facade sampled pressure after allocation or return. Direct static node operations could complete the same changes without notifying the watcher.

**Cause**

Pressure sampling was tied to the caller's entry rather than the completed physical operation. The batch paths also called the single-block operation for each withdrawal.

**Change**

Static node operations sample once at completion. Batch operations withdraw privately and use `gktl::defer` to restore staged blocks before the completion sample. The facade still applies pressure admission for `Gaf::CanWait`.

**Reference**

This tree's memory-pressure gate owns observation and waiting.

**Verification**

The two dev images booted in both configurations, with all 163 host tests passing. In the default and memory-only NUMA runs, direct batch allocation and list return signalled the watcher, and partial fragment rollback produced no threshold notification. The memory-pressure and reclaim regression commands passed.

**Deliberately not done**

Selection of the node for reclaim is a separate increment.

**How it was found**

Compared the facade and static paths to the same zone mutations and followed the rollback of a multi-block request.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Reclaim on an eligible node after a single-page allocation miss (2026-11-04)

**Files**

- `kernel/mm/pm_node.cpp` records an eligible zone below High when an order-zero attempt cannot obtain a frame.
- `kernel/mm/pmm.cpp` reports the shortage at operation completion.
- `kernel/mm/memory_reclaimer.cpp` coalesces shortages, rechecks current stock and walks only the target node.
- `kernel/commands/page_reclaim_test.cpp` exercises the local shortage while global pressure is Normal.

**Symptom**

A request restricted to a depleted node can fail while free memory on the rest of the machine keeps the global watchdog at Normal.

**Cause**

The global watchdog observes total free memory and requests a global walk. It has no information about the eligible zones of a failed allocation.

**Change**

An exhausted order-zero walk records its first eligible depleted zone. At completion the pressure observer queues that zone for the background reclaimer. The worker combines pending zones by node and samples each zone before deriving its deficit against High. It uses the existing zero-page and clean-pager reclaim logic with a singleton node mask. An ordinary caller can retry after inventory becomes available.

**Reference**

This tree's zone watermarks and per-node page queues provide the target. Global pressure and its reclaim path continue to use the full online-node mask.

**Verification**

The opt-in `pagereclaim.test --target-node` exhausts a memory-only node while the machine remains above its global pressure thresholds. A strict allocation fails, the background reclaimer evicts that node's pager page, and a strict retry obtains a frame from the same node. The other node's pager page stays resident.

The default and NUMA debug and release images booted. All 163 host tests passed, as did the default pager and pressure tests and the two-CPU, three-node PM node and zone regressions.

**Deliberately not done**

The reclaim walk is scoped to a node. Admission and waiting still use the global gate, and requests for larger blocks use the existing buddy path.

**How it was found**

Compared the set of zones an allocation could use with the machine-wide count the watchdog samples.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Type the PMM shortage and pending request states (2026-11-04)

**Files**

- `kernel/mm/pm_node.cpp` returns a typed block failure with an optional depleted zone.
- `kernel/mm/memory_reclaimer.cpp` uses per-zone `Idle` and `Pending` states.
- `kernel/mm/pm_zone.cpp` takes the returned block by reference.

**Symptom**

The block attempt reported a failure through its result and a second pointer output. Pending reclaim requests were encoded through integer masks.

**Cause**

The code represented optional data and queue transitions through their storage instead of the allocation and reclaim types.

**Change**

The internal `Result<PmFrame *, BlockFailure>` keeps the error status and optional zone together. Reclaim producers and the worker exchange `RequestState` for the actual zone. A return to a zone requires a frame reference.

**Reference**

This uses the tree's `ustl::Result`, `ustl::Option` and scoped-enum conventions.

**Verification**

Clang-format and clang-tidy checked all nine changed C++ files. Both dev images booted in the default and NUMA configurations and all 163 host tests passed. The memory-only NUMA run passed targeted reclaim, all 70 PM node checks, the zone tests and all 19 pressure checks.

**Deliberately not done**

The change is scoped to the PMM internals and their direct tests.

**How it was found**

The user asked to represent states with enums and remove C-style interfaces from the recent PMM work.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.
