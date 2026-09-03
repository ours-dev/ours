# protection-ranges-redesign

## Problem

`MappingProtectionRanges` (vm_mapping.hpp) has accumulated structural debt:

1. Implicit left bounds: a `Region` stores only `end`; the left bound is a convention
   ("previous region's end"), unchecked, and `update()` pays for it with ~90 lines of
   boundary juggling.
2. The intrusive MultiSet tolerates duplicate `end` keys that would silently corrupt the
   invariant (in-header FIXME).
3. `Region` / tree iterators leak through the header (in-header FIXME); `Enumerator::shift()`
   is dead code.
4. Every `update()` call re-passes `lower_bound`/`upper_bound` from the caller.
5. Latent bug A: `VmMapping::protect()` protects with the outer `base/size/mmuf` instead of
   the current region's, so per-region distinctions never reach the page tables.
6. Latent bug B: `Region` nodes leak when a mapping is destroyed (intrusive container does not
   own nodes; nothing disposes them on the destruction path).

Exploration confirmed the class has zero consumers outside `vm_mapping.cpp`.

## Design (approved, storage revised by user direction)

In-place rewrite; keep the "one mapping tracks its own sub-range flags" model.

- Rename to `ours::mm::ProtectionRanges`; new header `ours/mm/protection_ranges.hpp`, new impl
  `kernel/mm/protection_ranges.cpp`.
- Storage (revised per user request from the original `ktl::Vec` plan): `ustl::collections::BTree`
  keyed by region base, `Region {base, end, mmuf}` as the value — the same container/allocator
  arrangement as `VmaSet`. Explicit bounds on every region; implicit left bounds gone;
  invariants checkable (`validate()` DEBUG walk). Since the tree documents no iterator
  stability across insert/erase, update() never mutates through an iterator that survived one:
  every step re-locates its region by key.
- API: `init(base, size, mmuf)` fixes the window; `update(base, size, mmuf)` no longer takes
  bound arguments; `make_enumerator(base, size)` keeps the P0963R3 `NextResult` aggregate so
  the four consumer loops keep their shape; Enumerator walks const tree iterators plus clip
  bounds.
- `update()` splits the end boundary first, then the base boundary, then overwrites the flags
  of every region intersecting the range in one sweep, then coalesces equal-flag neighbours
  walking by running base address. An OutOfMem abort can leave at most one benign boundary
  split (no flag changed; the next update coalesces it).
- Zero-permission-means-unmapped convention is preserved (protect/unmap/map rely on it).
- Fix bug A while touching its call site.
- Host smoke test `kernel/mm/tests/protection_ranges_smoke.cpp` following the
  frame-buddy-smoke convention (plain main(), host toolchain). The BTree version needs no
  allocation shims at all: the header is ktl-free and the ustl default resource falls back to
  new/delete on the host.

## update() algorithm

Clip to window → locate region A (contains `base`) and B (contains `end - 1`) → count splits
(A.base < base && A.mmuf != mmuf; B.end > end && B.mmuf != mmuf) → `reserve(size + splits)` →
split B at `end` first, then A at `base` (later index first keeps earlier indices valid) →
overwrite flags of every region intersecting [base, end) — unsplit boundary regions already
carry the target flags, so blanket overwrite is safe → one coalescing pass → `validate()`.

## Won't do (recorded)

- Fuchsia-style protect-splits-mappings (deletes the class): revisit once the `fault` path
  stops being a stub.
- Fusing update()+enumerate (old TODO): region counts are single digits and make_enumerator is
  a linear scan; the fusion saves nothing worth the API twist.
- `VmArea::protect()` implementation: separate task.

## Result

See RESULT.md.
