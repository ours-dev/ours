# vmm-responsibilities: review of VMM state ownership

**Date**: 2026-11-04

## Scope and method

Reviewed COW frame replacement, mapping creation and teardown, and permanent residency at `edb734e6` using `kernel-design`, `codebase-design`, and `modern-cpp-development`. Compared the corresponding functions in the local Zircon checkout. A temporary QEMU probe in `vmosnapshot.test` reproduced H1 and M1. The two new assertions failed and the other 248 checks passed. The probe source was restored byte for byte. On the restored tree, both development images booted and 163 host tests passed. M2 and M3 are source findings, with existing repair records where applicable.

## What is done well

- `kernel/mm/vm_cow_pages.cpp:265` delegates propagation to a representative VMO. The COW node keeps page ownership while the VMO layer owns view and mapping traversal. The reclaim path at line 94 demonstrates a synchronous invalidation before releasing a frame.
- `kernel/mm/vm_mapping.cpp:544` applies object-driven invalidation to page tables and retains the mapping's logical permissions so the next access can resolve against the current backing.

## High severity

### H1. A COW fork leaves another mapping of the writer on the old frame

`kernel/mm/vm_cow_pages.cpp:1157` installs the new backing frame without a range notification. `kernel/mm/vm_mapping.cpp:862` gets the new frame, then `remap_locked` updates only the faulting virtual address. Two mappings of the same clone at the same object offset start on one inherited frame. After a write fault, QEMU reported the writer PTE at `0x141000` and the second PTE still at `0x1f7000`. An ordinary read through the second mapping can therefore reach old content without faulting. A direct VMO write uses the same owned-page path and has no faulting mapping to refresh.

Have the COW transition issue the existing range notification while it still holds the shared lock and before it returns the new frame. The VMO layer can forward it to all affected views and let the faulting mapping install its new entry. Audit marker-to-page replacement at `kernel/mm/vm_cow_pages.cpp:1259` for the same issue. A transfer that keeps the same frame has no stale frame address to invalidate. Check the kernel mapping restriction in `range_change_locked_object` when designing the fix. Zircon's `VmCowPages::CloneCowPageLocked` gates its range update on whether the resulting frame changes, and `CompleteAddPageLocked` removes mappings of the former backing.

## Medium severity

### M1. The mapping window is validated in different layers for different entry points

`kernel/object/vma_dispatcher.cpp:118` checks whether a user mapping fits in its VMO. `kernel/mm/vm_area.cpp:348` publishes a kernel mapping before invoking an optional installer, and `kernel/mm/include/ours/mm/vm_object_paged.hpp:226` rejects a paged window only when a cursor is created. A QEMU probe called the kernel entry with a one-page Lazy mapping starting at the end of a two-page VMO. Creation failed to return `OutOfRange` as expected. This leaves validation dependent on who calls the interface and whether a page is installed immediately.

Enforce the ordinary mapping window at construction after size normalization, before linking it. Leave handle policy and ABI-specific errors with the dispatcher. `VmArea::reserve` at `kernel/mm/vm_area.cpp:794` uses a zero-length placeholder VMO to describe already installed boot mappings. Give adoption of such mappings an explicit internal path before applying ordinary VMO-window validation there. Zircon's `VmAddressRegion::CreateVmMapping` checks page alignment and offset overflow at creation and checks VMO bounds for physical objects. Its paged-object policy is broader than this repository's current user contract.

### M2. Mapping publication and teardown have several owners

`kernel/mm/vm_area_or_mapping.cpp:50` marks a node Active before fallible insertion. `kernel/mm/vm_mapping.cpp:159` ignores that result and links the VMO, and `VmMapping::create` returns success at line 153. An insert failure can leave an Active mapping visible on the VMO side and absent from address lookup. The existing failure is recorded in `docs/repairs/260908/heap.md`. Separately, normal creation increments the parent's count at `kernel/mm/vm_area.cpp:207`. Reserve calls `VmMapping::create` directly, and the mapping teardown decrements that count at `kernel/mm/vm_mapping.cpp:239`. The count has no readers.

Place tree insertion and removal with `VmArea`, and make the activation path publish Active only after the mapping has joined both sides. Drop the unused count or give its updates to the tree owner if a consumer needs it. `VmMapping::destroy` also inspects the address space's fault cache and vDSO registration directly at lines 196 and 209. A mapping removal notification would let `VmAspace` own both indexes and keep the bookkeeping under the same address-space lock. Zircon's `VmMapping::ActivateLocked` checks insertion, unwinds it if VMO registration fails, and sets the live state last. The activation failure was reviewed from source without allocation fault injection.

### M3. Permanent residency is copied into both the view and the backing node

`kernel/mm/vm_object_paged.cpp:96` copies the permanent residency flag into COW. The node uses it for page classification at `kernel/mm/vm_cow_pages.cpp:48`, while the view guards clone and transfer at `kernel/mm/vm_object_paged.cpp:346` and line 511. The clone and transfer methods in COW check counted pins, which is a separate state. The internal entry can miss the permanent restriction if reached without the view guard. This is existing debt recorded in `docs/repairs/261031/mm.md`.

Let the backing node enforce whether its frames may move. Keep the VMO responsible for translating its own range. Keep counted pins with their existing pin/unpin contract. Zircon's `VmCowPages::CreateCloneLocked` checks pinned-page state at the COW mutation seam. The proposed treatment of permanent residency follows this repository's own semantics.

## Minor and style

- Whole-file clang-format passed on six of the seven inspected implementation files and reported existing violations in `vm_area.cpp`. Clang-tidy completed on all seven with existing diagnostics. No style changes were made during the audit.

## Teaching opportunities

None.

## Suggested follow-up order

1. Fix frame replacement notification and retain a regression that checks both aliases.
2. Validate mapping windows before publication and keep boot adoption explicit.
3. Consolidate mapping activation and teardown ownership, starting with fallible tree insertion.
4. Centralize the permanent-residency guard in the backing node.

## Frame replacement repair

The copy and zero materialization paths invalidate under the existing shared lock before returning a new frame to the caller. The prefaulted alias cases exercise mapped writes, direct writes, a local marker and an inherited hole. Both development images and the snapshot, page-source and reclaim suites passed.

## Mapping window repair

The bounds check runs after placement normalizes the size and before the mapping is linked. The regression exercises every mapping mode and then maps a valid window at the same address. The ordinary and boot reservation paths both passed their verification.

## Mapping lifecycle repair

A failed tree insertion leaves the active state clear and returns before registration with the VMO. Both factories release the unpublished object. The duplicate mapping regression exercises this error and checks the original lookup result. Both development images booted, and the seal, snapshot and large-page suites passed.

## Permanent residency repair

The COW node rejects clone and transfer before it changes slots. The VMO still validates and translates the requested window. The contiguous VMO suite exercised the node directly and checked that a rejected transfer left the splice list fresh.

## Coverage follow-up

The regression cases check state visible to callers after failure, including retained objects, rolled-back page requests and page protections. The host allocator replacement is limited to construction of the test range tracker. Both development images booted, all 163 host tests and all 46 instrumented kernel test commands passed. The four repairs reached 70/70 executable changed lines, and the VMM scope measured 4173/4763 lines. The remaining gaps include kernel allocation failures, exceptional pager and reclaim branches, and legacy unsupported methods.

## Allocation failure follow-up

The opt-in scope targets the current thread and one allocation site. Tests reached tree allocation failure after protection initialization, partial address-space construction, failed clone creation and a failed fork. They checked the original state and successful retry where applicable. The default and opt-in boots and all 46 instrumented commands passed.

The next pass exercised page transfers, page supply, protection splits and raw heap failures in VMA setup. The original data or permissions survived each failed operation. Both configurations passed verification, and instrumented VMM coverage reached 4260/4768 lines.

The frame failure point reached the allocator error returned to VMO callers. The instrumented suites passed the large-block, contiguous, eager VMO and pager VMO cases. Both configurations passed verification. The full run measured 4291/4763 VMM lines and its only failed command, preempt.test, passed in a standalone instrumented boot.
