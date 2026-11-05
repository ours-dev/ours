# pmm-static: review of static PmNode allocation across nodes

**Date**: 2026-11-04

## Scope and method

Reviewed pm_node.hpp, pm_node.cpp, pmm.cpp, and pm_node_test.cpp against kernel-design, modern-cpp-development, the user's chosen principal models, and the existing pmm contracts. The local Zircon pmm/PmmNode interface was previously compared; this project intentionally retains its multi-node model and now uses the user's static PmNode interface.

Ran real LLVM 23 changed-line formatting and clang-tidy on all four files using out/vm-address. Comparing before/after diagnostics found no new diagnostics. Built/booted disked dbg/rel under default and four-node NUMA, with all 163 host test executables passing in each configuration. Both images pass with dbg 46 passed / 5 skipped and rel 44 passed / 8 skipped, 7/7 required.

Default kernel tests pass: pmnode 28, pmzone 3, vmocontig 16, mempressure 15, pager 23, and pagereclaim 21 checks. Four-node tests pass: pmnode 65, pmzone 9, vmocontig 16, and mempressure 15. Logs and diagnostic manifests are in /tmp/pmm-static-checks/.

## What is done well

- pm_node.hpp:253 declares class-qualified static allocation with implicit/explicit home forms. The same convention covers bulk/fragment operations; release remains static.
- pm_node.cpp:176 resolves an implicit home once and routes to the explicit-home backend. Out-of-range hints keep the established implicit-home convention; absent in-range nodes are rejected before dereference. Home selection does not replace the caller's allowed-node mask.
- The named backend verifies the chosen instance and preserves allocation-disable/order checks before touching zones. Private instance helpers operate on the selected node's resources; allocation no longer depends on a caller's this pointer.
- Implicit placement resolution moved with allocation, retaining the narrow thread-resource lock scope. Explicit home selection skips thread-default policy as before. No extra zone/global lock or per-request allocation is introduced.
- pmm.cpp delegates all allocation forms statically while retaining its pressure gate, flag stripping, one-page waiting protocol, and notifications. Free routing still uses the frame's node/zone identity.
- pm_node_test.cpp directly invokes the static entry points, exercises local/explicit home and permitted remote allocation, empty masks, oversized orders, and both direct allocation-disable paths. Static release disposes every successfully acquired frame, including unexpected success paths.

## High severity

None introduced by this change. The existing per-zone lock remains the buddy lock owner; the thread-resource lock is dropped before allocation. Node/zone publication and lifetime are unchanged.

## Medium severity

None introduced. The known NUMA33 vmalloc startup failure remains separately recorded in docs/repairs/261104/mm.md and was already reproduced before these changes. This slice does not claim to fix policy enablement, selector watermarks, or locality statistics.

## Minor and style

- C++ allows static methods to be called through an instance. Every active PmNode allocation caller now uses class qualification; callers intending that instance as home pass its nid() explicitly. Private PmZone instance allocation remains appropriate.
- Existing tidy diagnostics were retained without repository-wide suppression. Allocation/free ownership preconditions and the immediate-backend versus pressure-facade distinction are documented.

## Teaching opportunities

None requested.

## Suggested follow-up order

1. Keep static PmNode operations as the selected allocation/release interface.
2. Implement any remaining resource/registry/CPU-relationship decomposition independently, retaining these calling and ownership semantics.
