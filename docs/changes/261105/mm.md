# mm changes

## Extend VMM tests around failed operations and cleanup (2026-11-05)

**Files**

- `kernel/commands/vma_seal_test.cpp` checks duplicate registration, stale handles and cached lookups, protection preflight, and vDSO registration withdrawal.
- `kernel/commands/page_source_test.cpp` checks failed immediate mapping and the timeout and retry of a pager-backed pin.
- `kernel/commands/vma_physical_test.cpp` checks byte offsets, physical mapping permissions and preservation of the cache attribute.
- `kernel/commands/vmo_snapshot_test.cpp` checks rejection of accesses beyond a slice window.
- `kernel/mm/tests/protection_ranges_smoke.cpp` injects allocation failure through the existing host memory resource.

**Symptom**

The normal tests exercised successful mutations more often than rejection, rollback and retained references to destroyed objects.

**Cause**

An operation's intermediate state is visible only when a step fails after earlier work. A pager wait and a fallible tree insertion give deterministic ways to exercise those boundaries.

**Change**

The tests verify that `pin_range` releases an already pinned prefix after a timeout, then completes after supply and holds each page once. Failed activation leaves the original mapping intact. A rejected range leaves the slice and user buffer data unchanged. A host allocator that returns OutOfMemory verifies that failed protection splits preserve every page's permissions and can be retried.

**Reference**

None. The tests exercise the local interfaces and their existing contracts.

**Verification**

Clang-format and clang-tidy checked the five changed C++ files. Both dev images booted, all 163 host tests and 46 instrumented kernel commands passed. The four targeted suites passed 264 snapshot, 34 sealed-mapping, 81 page-source and 11 physical VMO checks. The host tracker reached 113/117 lines (96.58%) and 44/46 branches (95.65%).

The VMM scope covers instrumented `kernel/mm` files named `vm_*`, `page_*`, `protection_ranges.*`, `init_vmm.*`, `vmm.*` and `stack.*`. Kernel instrumentation covered 4173/4763 lines (87.61%) and 1714/2246 branches (76.31%). All 70 executable lines changed by the four repairs were covered. The overall line target of 95% still requires 352 additional lines. The raw coverage report is in `out/vmm-coverage/coverage/vmm-report.txt`.

**Deliberately not done**

None.

**How it was found**

The source-coverage report identified gaps in registration failure, retained mapping operations, pager pin rollback and protection coalescing.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Exercise VMM allocation failures in an opt-in kernel (2026-11-05)

**Files**

- `kernel/mm/alloc_failure.cpp` and its private header implement one scoped failure on the owning test thread.
- The kernel collection resource, object cache, page table and VMO frame allocation supply the failure points.
- `kernel/commands/vma_seal_test.cpp` and `kernel/commands/vmo_snapshot_test.cpp` exercise initialization, registration, mapping and COW rollback.

**Symptom**

Normal QEMU tests could reproduce duplicate registration. They could not reliably make VMA tree growth, object allocation or page-table allocation fail at a specific step.

**Cause**

Allocation failures depend on memory and allocator state. Exhausting memory globally would also disrupt unrelated kernel threads and make the failure site nondeterministic.

**Change**

The optional `mm_test_alloc_failure=true` configuration enables a thread-owned scope with a selected allocation site and skip count. The scope fails one matching allocation and records that it fired. Tests assert both the error and the resulting mapping or backing state, then retry. The default configuration compiles the failure sites out.

**Reference**

None. The failure points use the repository's existing allocation contracts.

**Verification**

The default and opt-in configurations booted both dev images and passed 163 host tests each. LLVM 23 clang-format and clang-tidy checked the C++ changes, and the repository GN formatter checked the three build files. All 46 instrumented commands passed. The seal suite passed 45 checks and the snapshot suite passed 270. The opt-in VMM line result was 4234/4768 (88.80%). It takes 296 more covered lines to reach 95% in that build.

Run the fault cases with `python3 scripts/cov_kernel.py --out out/vmm-coverage` after configuring the output with `--variant coverage-kernel --args 'mm_test_alloc_failure=true'`. The injected page-table error is logged as an error before the test reports its passing verdict.

**Deliberately not done**

None.

**How it was found**

The VMM coverage gaps included rollback paths that could not be reached with the ordinary test workloads.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Exercise allocation failure while transferring pages (2026-11-05)

**Files**

- `kernel/lib/heap/src/new.cpp` provides the opt-in failure site for kernel new and new[].
- `kernel/commands/page_source_test.cpp`, `kernel/commands/vma_seal_test.cpp` and `kernel/commands/vmo_snapshot_test.cpp` exercise supply, protection and transfer rollback.

**Symptom**

An allocation error while preparing a range or transferring its pages could leave the operation partially applied. The tests could only check successful transfers and permission changes.

**Cause**

Range splits can allocate tree nodes. A COW fork can allocate both a frame and a page-list slot. A splice must reserve space to hold transferred frames before it takes them from the backing node.

**Change**

The tests fail allocation at these steps and verify the original PTE permissions, backing data or empty destination slot as appropriate. They then retry the operation. Raw heap failures can also exercise hierarchy allocation during anonymous mapping and reservation.

**Reference**

None. These are failure contracts of the existing operations.

**Verification**

Clang-format and clang-tidy checked the five changed C++ files. The default and opt-in configurations booted both dev images and passed all 163 host tests each. All 46 instrumented kernel commands passed. The VMM measured 4260/4768 covered lines (89.35%). The 95% target requires another 270 lines.

**Deliberately not done**

None.

**How it was found**

The coverage report showed gaps when the kernel allocates range metadata and copies or transfers page slots.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Exercise frame and VMO creation failures (2026-11-05)

**Files**

- `kernel/mm/pmm.cpp` injects a failure in the implicit-node block allocation entry point.
- The large-page, contiguous VMO, page-source and snapshot commands exercise the callers' error paths.

**Symptom**

The previous frame failure point returned from the COW node before it received the allocator's error. It could not exercise block allocation or contiguous creation.

**Cause**

Large-page fill and contiguous creation call the frame allocator directly. Creating a pager VMO can also fail after it has accepted responsibility for the source.

**Change**

The fault site supplies the allocator's normal error to each of these callers. The tests cover single-page fallback after failing to allocate or partially insert a large block, publication failure after a partial eager commit, contiguous VMO creation at four failure points, and closing a pager source when its VMO cannot be created.

**Reference**

None. The tests use the existing frame and VMO allocation entry points.

**Verification**

Clang-format and clang-tidy checked the eight C++ paths. Both images and 163 host tests passed in the default and opt-in configurations. The large-page, contiguous VMO, page-source and snapshot commands passed 32, 26, 85 and 275 checks respectively. The full instrumented run measured 4291/4763 VMM lines (90.09%), 234 short of 95%. Its only command failure was a timer deadline check in `preempt.test`, which passed in a standalone instrumented boot.

**Deliberately not done**

None.

**How it was found**

The first fault-injection pass left the frame allocator error, large-block rollback and pager source transfer paths uncovered.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Let an interrupted eager map discard its staged batch (2026-11-05)

**Files**

- `kernel/mm/vm_mapping.cpp` owns the temporary batch of physical addresses.
- The page-source and sealed-mapping commands test interruption before and after a page-table batch is installed.

**Symptom**

A commit mapping of a pager VMO panicked when the first page was present and the next page needed supply. An allocation failure after staging the first page had the same path.

**Cause**

The cursor returned an error while the coalescer still held addresses. The coalescer's destructor asserted that its batch was empty, even though none of those staged entries had reached the page tables.

**Change**

Only successful completion requires a final batch commit. Staged physical addresses own no frames and need no cleanup. The mapping factory still destroys the failed mapping, which removes earlier batches already installed in the page table and releases the virtual range.

**Reference**

None. The repair uses the local mapping factory's existing rollback.

**Verification**

The standard debug image reproduced the old assertion. After the repair, the pager regression passed in that image. Both images and all 163 host tests passed in the default and opt-in configurations. Clang-format and clang-tidy checked all three C++ files. The instrumented pager and seal suites passed 86 and 53 checks. The full run reached 4286/4758 VMM lines (90.08%). The preemption timer deadline check failed again in the combined run.

**Deliberately not done**

None.

**How it was found**

Reviewing allocation failure after the first page exposed a return between staging an address and committing the batch.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Prepare protection metadata before a partial unmap (2026-11-05)

**Files**

- `kernel/mm/protection_ranges.cpp` and its header separate boundary preparation from the infallible flag update.
- `kernel/mm/vm_mapping.cpp` orders preparation, page-table removal and publication of the hole.
- `kernel/commands/vma_seal_test.cpp` injects a failure while creating an unmap boundary.

**Symptom**

Partial unmap could return OutOfMem after removing a PTE, leaving the target page absent even though its recorded permissions still allowed access.

**Cause**

The code removed translations before updating its protection tree. Updating a subrange can allocate tree nodes to split either endpoint.

**Change**

Prepare both boundaries under the existing locks before touching the page table. A successful architecture unmap commits the permissionless range without allocating. If the architecture operation fails, the original permissions are retained and the prepared boundaries are coalesced. The regression checks the original PTE after injected allocation failure, a successful retry, denial of a later fault, and preservation of an adjacent translation.

**Reference**

None. The change orders the existing mapping and protection operations.

**Verification**

Before the repair, two injected failures returned OutOfMem after clearing the target PTE. Clang-format and clang-tidy checked all four C++ files. Both images and 163 host tests passed in each configuration. The seal command passed all 72 checks, and all 46 instrumented kernel commands passed with the preemption test run first. VMM line coverage reached 4301/4770 (90.17%), 231 lines short of 95%.

**Deliberately not done**

None.

**How it was found**

The protection fault-injection cases revealed that partial unmap used the same tree allocation in the opposite order from protect.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Exercise page-table and COW merge allocation failures (2026-11-05)

**Files**

- `kernel/commands/vma_physical_test.cpp` exercises the physical VMO factory and both sizes of physical map batch.
- `kernel/commands/vma_seal_test.cpp` fails a page table allocation during a write fault.
- `kernel/commands/vmo_snapshot_test.cpp` interrupts consolidation of a hidden COW node.

**Symptom**

The existing tests exercised mapping setup, faults and COW consolidation when all storage was available.

**Cause**

An interrupted consolidation must keep a page or a zero marker available through the hidden parent. A failed page-table allocation can leave a VMO page committed before the translation exists.

**Change**

Test both kinds of COW slot, verify that the survivor still sees the inherited value, then write through it. Verify that a demand mapping leaves a committed page behind and installs its PTE on retry. Physical mapping failures cover object allocation, a short batch and a full batch, and release the address for another attempt.

**Reference**

None. The cases exercise the existing failure contracts.

**Verification**

Clang-format and clang-tidy checked all three C++ files. Both images and all 163 host tests passed in the default and opt-in configurations. All 46 instrumented commands passed. The physical VMO, seal and snapshot commands passed 17, 75 and 279 checks. The VMM measured 4315/4770 covered lines (90.46%), with 217 more needed for 95%.

**Deliberately not done**

None.

**How it was found**

The instrumented run distinguished failed VMA registration from the paths that allocate after a mapping or COW relationship already exists.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Check VMM placement and physical ownership boundaries (2026-11-05)

**Files**

- `kernel/commands/vm_address_test.cpp` checks placement exhaustion, invalid map modes and permission ceilings.
- `kernel/commands/vma_phys_test.cpp` exercises demand fault and protection rejection for a physical mapping.
- `kernel/commands/vma_physical_test.cpp` checks operations that would require ownership of a physical window.

**Symptom**

The ordinary mapping tests focused on successful placement and physical address translation. They rarely exercised invalid or unsupported requests at those boundaries.

**Cause**

Placement, permissions and backing ownership are independent checks. A physical window has no paged backing to fault in, copy through generic VMO I/O, pin by count or transfer into a pager.

**Change**

The cases verify that rejected requests leave the virtual range or PTE intact, and that unsupported physical VMO operations do not consume the splice list or modify the caller's data.

**Reference**

None. The tests exercise the local contracts.

**Verification**

Clang-format and clang-tidy checked the three changed C++ files. Both development images and all 163 host tests passed in each configuration. The three boundary suites passed in the default kernel. The corrected placement and protection tests also passed in the opt-in kernel, and the physical ownership cases passed in the instrumented run.

**Deliberately not done**

None.

**How it was found**

Coverage highlighted missing rejection paths in placement and the inherited physical VMO interface.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Publish the page aging runner after startup succeeds (2026-11-05)

**Files**

- `kernel/mm/page_queues.cpp` owns the runner during startup.
- `kernel/commands/page_queues_test.cpp` tests worker allocation failure followed by another start attempt.

**Symptom**

If allocating an aging thread failed, the next call to start the runner asserted that it had already started.

**Cause**

The queue recorded the runner before the runner had a thread. On error the caller got OutOfMem, but the queue still looked started.

**Change**

Hold the runner locally during startup and publish it after success. A failed start drops the unpublished runner. The test first fails thread storage, then retries and fails allocation of the runner itself, proving that the first call left the queue eligible to start.

**Reference**

None. The change follows the queue's existing single-start contract.

**Verification**

The opt-in debug image reproduced the duplicate-start assertion before the fix. Clang-format and clang-tidy checked both C++ files. Both images and all 163 host tests passed in each configuration. All 46 instrumented commands passed, including 14 page-queue checks. The VMM measured 4376/4773 lines (91.68%) and 1800/2244 branches (80.21%). The 95% line target requires 159 additional covered lines.

**Deliberately not done**

None.

**How it was found**

Reviewing the aging worker's uncovered allocation errors showed that startup published state before its final fallible operation.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Name asynchronous page requests request_pages (2026-11-05)

**Files**

- `kernel/mm/include/ours/mm/page_request.hpp` and `kernel/mm/page_request.cpp` declare and implement the request operation.
- `kernel/mm/vm_cow_pages.cpp` and the existing pager commands call the renamed entry point.

**Symptom**

The name get_pages suggested immediate retrieval even when the operation records, combines or sends a pager request and returns ShouldWait.

**Cause**

The old name described the eventual content rather than the operation performed by this call.

**Change**

Rename `PageSource::get_pages(offset, len, request)` to `PageSource::request_pages(offset, len, request)` at every code and comment reference.

**Reference**

Zircon PageSource::GetPages supplies the domain contract. The user selected request_pages to express asynchronous request semantics using the project's Rust-based naming convention.

**Verification**

LLVM 23 clang-format and clang-tidy checked all five C++ files. Both development images booted and all 163 host tests passed. The existing pagesource.test and pager.test commands passed.

**Deliberately not done**

The unmap policy and placement helper names await the user's selection.

**How it was found**

The user revisited the twelve-item VMM naming list. A symbol search showed that its first approved rename had not been applied.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Scope fault causes to VmFault (2026-11-05)

**Files**

- `kernel/mm/include/ours/mm/vm_fault.hpp` declares the nested Cause bitmask.
- Fault resolution, task, architecture, user-copy and command callers use the nested type.

**Symptom**

VmfCause used an abbreviation separate from the fault descriptor it describes.

**Cause**

The type's global name repeated its association with a VM fault. The user selected a nested type so callers can write the association directly.

**Change**

Move `VmfCause` into the descriptor as `VmFault::Cause` and keep the field `Cause cause`. Use the nested enum bitmask declaration and migrate every type, flag expression and assembly comment.

**Reference**

None. VmFault::Cause is the user's selected project vocabulary.

**Verification**

LLVM 23 clang-format and clang-tidy checked all seventeen changed C++ files. The enum's underlying type and its six values match the old definition, including the values captured by the x86 user-copy assembly. Both development images booted, all 163 host tests passed, and usercopy.test and vmosnapshot.test passed.

**Deliberately not done**

None.

**How it was found**

The user selected the nested Cause spelling while reviewing item seven of the VMM naming list.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Name the page lookup cursor and its result (2026-11-05)

**Files**

- `kernel/mm/include/ours/mm/vm_cow_pages.hpp` and `kernel/mm/include/ours/mm/vm_object_paged.hpp` expose the cursor factory.
- Their implementations, mappings and existing kernel command tests use the new names.

**Symptom**

The cursor factory's old name did not express its page lookup role or required lock. RequireResult was also returned by a lookup that might find no page.

**Cause**

The factory name described construction, and the result name was tied to one of several operations producing the same page and writability record.

**Change**

Rename `make_cursor` to `lookup_cursor_locked` on the COW and paged VMO layers. Rename `Cursor::RequireResult` to `Cursor::PageLookup` throughout its producers and consumers.

**Reference**

Zircon VmCowPages::LookupCursor supplies the domain vocabulary. The factory and result spellings were selected by the user.

**Verification**

The original lock annotations and page lookup logic are preserved. LLVM 23 clang-format and clang-tidy checked all twelve C++ files. Both development images booted and 163 host tests passed. The snapshot, pin, contiguous, large-page and page-source commands passed.

**Deliberately not done**

The independent xarray cursor uses its existing name.

**How it was found**

The user accepted items eight and nine of the VMM naming list.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Name VMO-backed mapping map_vmo (2026-11-05)

**Files**

- `kernel/mm/include/ours/mm/vm_area.hpp` and `kernel/mm/vm_area.cpp` expose map_vmo.
- Kernel stack, heap, VMA dispatcher, benchmark and command callers use the new name.

**Symptom**

The map_with_vmo spelling was longer than the selected verb-object vocabulary for the VMM interface.

**Cause**

The interface maps a VMO window into a virtual area. The VMO is the operation's direct object.

**Change**

Rename `VmArea::map_with_vmo(...)` to `VmArea::map_vmo(...)` and update all code references and nearby comments.

**Reference**

None. The user selected map_vmo and retained create_subvma for child area creation.

**Verification**

The mapping arguments, defaults and rollback bodies are preserved. LLVM 23 clang-format and clang-tidy checked all sixteen C++ files. Both images booted, 163 host tests passed and all six focused mapping and snapshot commands passed.

**Deliberately not done**

None.

**How it was found**

The user accepted map_vmo in item eleven of the VMM naming list.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Name the unmap child policy UnmapScope (2026-11-05)

**Files**

- `kernel/mm/include/ours/mm/vm_area.hpp` nests UnmapScope in VmArea.
- The area, VMA dispatcher, syscall and undo tests use the named scope.

**Symptom**

VmAreaOpChildren::No and Yes did not state what an unmap would be allowed to destroy.

**Cause**

The type name came from the handle right, while its values describe the scope of an unmap operation.

**Change**

Replace `VmAreaOpChildren::{No, Yes}` with `VmArea::UnmapScope::{MappingsOnly, IncludeSubvmas}`. The dispatcher exposes the same UnmapScope and translates Rights::OpChildren through `unmap_scope_from_rights`. The default still includes fully covered child VMAs. Both modes reject partial coverage of a child VMA.

**Reference**

Zircon VmAddressRegion::UnmapInternalLocked has the corresponding can_destroy_regions policy. The user selected the explicit scoped-enum spelling.

**Verification**

The preflight, teardown and handle-right checks keep the same conditions and branches. LLVM 23 clang-format and clang-tidy checked all six C++ files. Both development images booted, 163 host tests passed, and the undo and seal commands passed.

**Deliberately not done**

None.

**How it was found**

The user asked what item three controlled and then selected MappingsOnly and IncludeSubvmas.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Name range resolution resolve_range_locked (2026-11-05)

**Files**

- `kernel/mm/include/ours/mm/vm_area.hpp` and `kernel/mm/vm_area.cpp` declare and use the range resolver.
- `kernel/mm/include/ours/mm/vm_area_or_mapping.hpp` refers to its range validation.

**Symptom**

create_subaom_preamble used an abbreviation and a processing-stage name for a helper shared by child area creation and VMO mapping.

**Cause**

The helper returns a validated, aligned base and length. It also resolves a fixed address or calls automatic address selection while holding the address-space lock.

**Change**

Rename `create_subaom_preamble(...)` to `resolve_range_locked(...)` at both creation paths and in its supporting comments.

**Reference**

Zircon VmAddressRegion::CreateSubVmarInner contains the corresponding common creation checks and calls AllocSpotLockedRegion. The user selected resolve_range_locked and retained alloc_spot for address selection.

**Verification**

The resolver body, allocation policy and existing lock annotation are preserved. LLVM 23 clang-format and clang-tidy checked all three C++ files. Both images booted, 163 host tests passed, and the address placement, seal and large-page commands passed. A symbol search found no remaining old approved names in kernel, system or benchmark source.

**Deliberately not done**

None.

**How it was found**

The user accepted resolve as the verb and then selected range as the noun in item four of the VMM naming list.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Shorten the unmap scope variants to Mapping and Subarea (2026-11-05)

**Files**

- `kernel/mm/include/ours/mm/vm_area.hpp` names and documents the two scopes.
- The area, VMA dispatcher and existing undo command use the selected variants.

**Symptom**

MappingsOnly and IncludeSubvmas were longer than the user's preferred scope vocabulary.

**Cause**

UnmapScope already supplies the operation and scope context.

**Change**

Rename `UnmapScope::MappingsOnly` to `UnmapScope::Mapping` and `UnmapScope::IncludeSubvmas` to `UnmapScope::Subarea`. Update the default argument, rights conversion, callers and comments.

**Reference**

None. Mapping and Subarea are the user's selected names.

**Verification**

The enum values, default scope and child coverage checks are preserved. LLVM 23 clang-format and clang-tidy checked all five C++ files. Both development images booted, all 163 host tests passed, and vmaundo.test and vmaseal.test passed.

**Deliberately not done**

None.

**How it was found**

The user selected Mapping and Subarea while refining the delivered UnmapScope names.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Name the zone free-frame snapshot num_free_frames (2026-11-05)

**Files**

- `kernel/mm/include/ours/mm/pm_zone.hpp` names the counter getter and explains its snapshot contract.
- PMM admission, reclaim, aggregate counting and existing node tests use the getter.

**Symptom**

PmZone::free_frames looked like the operation that releases frames. Calling the value approximate without explaining its relationship to the buddy lists also obscured the actual counting mechanism.

**Cause**

The atomic counter is maintained using the actual frame deltas, but reading it does not acquire the lock that groups a buddy mutation and its counter update. A read may therefore observe the preceding count during an in-flight transfer, and later operations may change it again.

**Change**

Rename `PmZone::free_frames()` to `PmZone::num_free_frames()` at every call site. Document the actual frame updates, the read's independence from the zone lock, and the distinction from `count_free_frames()`, which traverses buddy lists under that lock.

**Reference**

None. The user selected num_free_frames and requested that the snapshot semantics be expressed in the comment.

**Verification**

The counter representation, memory ordering, update sites and lock scope are preserved. LLVM 23 clang-format and clang-tidy checked all six C++ files. Both development images booted, all 163 host tests passed, and pmnode.test, pmzone.test and mempressure.test passed.

**Deliberately not done**

None.

**How it was found**

The user questioned the proposed approximate-count name and selected num_free_frames after inspecting the concurrent update sequence.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Name the eligible zone range ZoneCandidates (2026-11-05)

**Files**

- `kernel/mm/include/ours/mm/priv/zone_candidates.hpp` replaces zone_selector.hpp.
- The node interface, request comments and existing allocation tests use ZoneCandidates.

**Symptom**

ZoneSelector suggested an object that selects the final zone, while its interface exposes an iterable range of eligible candidates.

**Cause**

Filtering eligibility and obtaining a block from a candidate are separate operations. The node allocation path decides which candidate actually supplies the allocation.

**Change**

Rename `ZoneSelector` to `ZoneCandidates` and its header to `zone_candidates.hpp`. Migrate includes, test helpers and iterator owner names. ZoneList::candidates keeps its existing factory name.

**Reference**

Linux next_zones_zonelist supplies the corresponding filtered traversal role. ZoneCandidates is the name selected by the user for this project's range interface.

**Verification**

Candidate order, filters and iterator behavior are preserved. LLVM 23 clang-format and clang-tidy checked all four C++ files. Both development images booted, all 163 host tests passed, and pmnode.test and pmzone.test passed.

**Deliberately not done**

None.

**How it was found**

The user accepted C1 from the PMM helper-class naming review.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Name the runtime CPU-node relation CpuNodeMap (2026-11-05)

**Files**

- `kernel/mm/cpu_node_map.cpp` and `kernel/mm/include/ours/mm/cpu_node_map.hpp` replace the node_placement files.
- Node bring-up, allocation callers, NUMA documentation, tests and the MM build entry use CpuNodeMap.

**Symptom**

NodePlacement did not identify which resources the class places. It could be read as the allocation policy that chooses where frames should come from.

**Cause**

The owned state is a runtime CPU-to-node table and its per-CPU pointer cache. Allocation policy and fallback ordering are separate responsibilities.

**Change**

Rename `NodePlacement` to `CpuNodeMap`, rename the source and header, and migrate all includes, definitions and callers. The table, early lookup path, cache updates and node migration sequence keep their existing implementation.

**Reference**

Linux cpu_to_node names the corresponding CPU-to-node relation. The project also keeps its runtime table separate from the firmware topology.

**Verification**

The node ids, cache layout, publication hooks and synchronization are preserved. LLVM 23 clang-format and clang-tidy checked all nine C++ files, and the GN formatter checked the source list. Default and four-node NUMA configurations booted both development images and passed 163 host tests each. pmnode.test and pmzone.test passed in both configurations.

**Deliberately not done**

None.

**How it was found**

The user accepted C2 from the PMM helper-class naming review.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.

## Scope allocation origin to AllocRequest (2026-11-05)

**Files**

- `kernel/mm/include/ours/mm/priv/alloc_request.hpp` nests the origin record.
- The node allocation path and existing kernel and host request tests name AllocRequest::Origin.

**Symptom**

Origin was a generic namespace-level name for the node and initiating CPU of one allocation request.

**Cause**

The record's meaning depends on the allocation request it describes. Its global spelling required surrounding context to identify that meaning.

**Change**

Move `Origin` into the public scope of `AllocRequest` and migrate resolver parameters and callers to `AllocRequest::Origin`. The resolver definition follows the completed class, and keeps its nodiscard attribute.

**Reference**

None. The user selected the nested record as part of the PMM naming review.

**Verification**

The record fields, designated initialization, request storage and resolver body are preserved. LLVM 23 clang-format and clang-tidy checked all four C++ files. Default and four-node NUMA configurations booted both images and passed 163 host tests each, including the multi-node request smoke test. pmnode.test passed in both configurations.

**Deliberately not done**

None.

**How it was found**

The user accepted the proposed AllocRequest::Origin scoping change along with the two helper-class renames.

**The user's prediction**

None.

**Three questions you should be able to answer**

None.
