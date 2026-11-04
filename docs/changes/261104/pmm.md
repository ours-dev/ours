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
