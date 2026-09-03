# Result

`MappingProtectionRanges` is gone; `ours::mm::ProtectionRanges` replaces it with explicit
bounds, a base-keyed B-tree, and a self-contained API.

## Storage revision (user-directed)

The rewrite first landed on a sorted `ktl::Vec`; the user asked for a B-tree instead, and the
storage now is `ustl::collections::BTree<VirtAddr, Region, RegionBaseOf, std::less<>,
ustl::collections::Allocator<Region>>` — the same container/allocator arrangement as
`VmaSet`. Two properties of that container shaped the code:

- ustl's B-tree documents **no iterator stability across insert/erase**, so `update()` never
  mutates through an iterator that survived one: boundary splits re-locate their region by the
  original key (`m_regions.find(b_base)`), and the coalesce walk is carried by a running base
  address with every lookup taken fresh.
- The first cut had a real bug the smoke tests caught: after inserting the split piece keyed
  at `base`, `region_containing(base)` resolves to the *new* piece, and truncating that one
  left an empty region behind. The fix is to save the original key before the insert and
  truncate via `find(a_base)`; the comment in `update()` records why.

## What landed

- `kernel/mm/include/ours/mm/protection_ranges.hpp` (new) — `ProtectionRanges` +
  private `Region {base, end, mmuf}` keyed into a B-tree by base + `Enumerator` walking const
  tree iterators with clip bounds. `NextResult` keeps the P0963R3 aggregate shape, so the four
  consumer loops in vm_mapping.cpp are untouched.
- `kernel/mm/protection_ranges.cpp` (new) — `update()` is the whole split/overwrite/coalesce
  algorithm in ~90 readable lines (was ~90 lines of implicit-left-bound boundary juggling in
  an incomprehensible dialect). `init()` fixes the window, so `update()` no longer takes
  caller-passed bounds. `validate()` is a DEBUG_ASSERT invariant walk (sorted, disjoint,
  tiling, coalesced) run after init/update.
- `kernel/mm/vm_mapping.{hpp,cpp}` — old class + Enumerator (~135 header lines), region
  ObjectCache, alloc/free wrappers and their init-hook block removed; `create()` calls
  `init(base, size, mmuf)`; stale update-enumerate TODO closed as won't-do.
- Bug fixes carried along:
  - protect() now protects per region (`region_base/region_size/region_mmuf`), not with the
    outer request values;
  - region storage is a B-tree whose nodes free themselves, so mapping destruction leaks
    nothing — the old Region-node leak disappears structurally;
  - the MultiSet duplicate-end hazard and the exposed internal tree iterators cease to exist.
- `kernel/mm/tests/protection_ranges_smoke.cpp` (new) + wiring — 9 hosted cases (init, 3-way
  split, restore-coalesce, regime widening, both-tails split inside one region,
  zero-permission round trip, edge-aligned updates, invalid-argument rejection, 100 flip-flop
  cycles staying at 3 regions), run in release and DEBUG (assert-live) builds. On the B-tree
  storage the test needs no allocation shims: the header is ktl-free and the ustl default
  resource is new/delete on the host.
- `BUILD.gn` (root), `kernel/mm/BUILD.gn`, `kernel/mm/tests/BUILD.gn` — new source + test
  targets wired; both mm smokes are now in the default build's test set, and the tests take
  mm headers via `//kernel/mm:headers.config` because `//kernel/mm:headers` pulls
  `//kernel/phys:handoff`, which cannot evaluate under the host toolchain (this is why
  frame-buddy-smoke had never been buildable; it is fixed by the same dep change).
- `kernel/lib/ktl/include/ktl/new.hpp`, `kernel/lib/ktl/new.cpp` — `operator delete` /
  `operator delete[]` declarations and definitions now carry `noexcept`, matching the standard
  and libc++'s <new>; this is what lets any kernel header using ktl containers compile in
  hosted unit tests.

## Verification

- `ninja -C out/bringup.x86`: full kernel image links (91/91).
- `ninja -C out/mm-host-tests` + run: protection-ranges-smoke all green, frame-buddy-smoke all
  green (first time it has ever been built).
- Manual DEBUG build of the smoke test (`-DDEBUG -include ours/prelude.hpp`): all green with
  `validate()`'s DEBUG_ASSERTs live.
- clang-format run on every touched file; clang-tidy reports zero new diagnostics on changed
  lines (remaining findings are pre-existing issues in gktl/ktl headers and old vm_mapping
  members).

## Notes for the future

- Storage history: the plan's `ktl::Vec` variant was tried first, then replaced by the
  user-requested B-tree. A `ustl::collections::Allocator` Vec variant was also tried and
  rejected back then — Vec's allocator contract is static-allocate (ktl-style), which the
  resource-backed ustl allocator does not satisfy.
- A DEBUG host build currently needs the manual prelude/sysroot invocation; making
  `out/mm-host-tests` debug-configurable through GN would be a small follow-up.
- `VmArea::protect` remains unimplemented (separate task); Fuchsia-style protect-splits-
  mapping stays deferred until `VmMapping::fault` stops being a stub.
