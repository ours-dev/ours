# pmm-naming: allocation names converge with the existing memory model

**Date**: 2026-11-04

## Delivered

Implemented and committed on `codex/pmm-naming`:

- `8d708eda`: Gaf becomes AllocFlags, with alloc_flags.hpp, ALLOC_KERNEL/USER/USER_VMO presets, alloc_flags parameters/members, and AllocRequest::flags(). DirectReclaim becomes CanBlock with its original bit and accurate current-behavior documentation. Migrated every active consumer, including heap, VMO, kernel objects, host tests, and the large-page benchmark.
- `b6e2d40f`: MemoryAllocationPolicy/State become AllocPolicy/AllocState, with matching headers, member names, and Thread::alloc_state() accessors. Policy behavior, configuration, enable/disable logic, and locking remain unchanged.
- `7412e729`: count helpers use alloc_frames_n/get_frames_n consistently. The node-specific direct-map helper calls the existing get_frames implementation, fixing an uninstantiated-template compiler error. pmnode.test now exercises both direct-map count-helper overloads: three requested pages produce a usable, zeroed four-page block; the explicit-node case also checks node restriction. Both release their actual allocations.
- `db127084`: explicit PhysAddr construction at the SRAT integer-address boundary fixes the additional NUMA build uncovered during verification. This is separate from the naming commits.

The existing Pm/Vm principal model names, AllocRequest, ZoneList, ZoneSelector, NodePlacement, FrameBuddy, and allocation geometry are retained. No compatibility aliases for the removed type/header names were added. Fair scheduler sources were not edited. The naming direction and active allocation documentation/examples are synchronized with the new interfaces.

## Verification

All edited C++ files were checked with the repository's LLVM 23 tooling and actual compile-database flags: 109 files in the flag slice, nine in the policy/state slice, two in the helper slice, and SRAT under the NUMA build flags. These sets overlap. Changed-line formatting passed; existing tidy warnings were compared against pre-change snapshots with identifier/path normalization and no new diagnostics were found. The benchmark had no diagnostics on its edited line; the SRAT compile error was removed without new diagnostics. Documentation passes lint_docs on changed lines.

The real-kernel-flag compile probe failed before the helper repair with undeclared get_frame and compiled successfully after the repair.

| Configuration | Build/boot | Host executables | Kernel suites |
|---|---|---|---|
| Default, disked dbg/rel | Both pass: dbg 46 passed / 5 skipped; rel 44 passed / 8 skipped and 7/7 required | 163 passed | Ten suites passed |
| Four-node NUMA, disked dbg/rel | Both pass with the same shell counts | 163 passed | pmnode 59, pmzone 9, vmocontig 16, mempressure 15 checks passed |
| Generated 33-domain NUMA | Both finish boot but fail the first vmalloc self-check | 163 passed | Not reported as a pass |

The ten default kernel suites are pmnode (23 checks), pmzone (3), vmocontig (16), vmopin (28), vmosnapshot (241), pager (23), pagereclaim (21), mempressure (15), vmaddress (42), and bti (79).

NUMA33 was compared with a source snapshot of pre-refactor commit f7b22de4 plus only the same SRAT PhysAddr conversion required for that configuration to compile. Unchanged submodules and the prebuilt tools were reused. That snapshot also fails both images on the same single-page vmalloc mapping at 0xffffff9000000000. This establishes that the extra failure predates the naming/helper changes.

LLDB stopped on the current failing mapping: status NoMemory, nr_mapped zero, data physical frame 0x80e000. The stack runs from MappingCoalescer::commit through VmMapping, VmArea, vmalloc, its self-test, and init_vmm. The underlying page-table allocation cause is not established. The temporary debugger and its QEMU were stopped after investigation.

Logs are under /tmp/pmm-naming-checks/: flags/state/helper-verify.log, kernel-tests.log, numa-verify.log, numa-kernel-tests.log, numa33-verify.log, parent-numa33-verify.log, public_api_before/after.log, source-tool logs, and the saved NUMA33 serial captures. All disk runs use the same test disk through QEMU snapshot overlays.

## Deviations from the design

The user prioritized naming convergence, so this delivery is the naming slice of pmm-responsibilities/DESIGN.md. The larger class/responsibility proposal remains Proposed. The SRAT boundary conversion was added as a separate fix because the extra NUMA verification could not compile without it.

## Found along the way

- The node-specific minimum-count helper referred to a nonexistent function. Fixed with production-path coverage in 7412e729.
- SRAT still passed an integer where EarlyMem requires PhysAddr. Fixed in db127084.
- The independent NUMA33 vmalloc startup failure is recorded in docs/repairs/261104/mm.md, with before/after source comparison and debugger evidence. It is not marked fixed.

## Not done

The broader allocator/registry/CPU-relationship decomposition, watermark admission, zone ordering, policy enablement, and per-CPU cache work remain separate from this behavior-preserving naming slice. The NUMA33 vmalloc failure remains open in its repair record.
