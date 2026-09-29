# vmo-snapshot-child: a copy-on-write child VMO, made by inserting a hidden node above the parent

**Date**: 2026-09-29
**Status**: Proposed

## Problem
The launcher (K23b, `system/lib/launch`) maps a writable segment from a private VMO that it fills with an explicit `Copy`. Zircon's loaders instead make a copy-on-write child of the file VMO (`ZX_VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE`, used by musl `dynlink.c`, elfldltl `VmarLoader::MapWritable` and `sdk/lib/ld/include/lib/ld/remote-abi-heap.h`), so only the pages a program writes are ever copied, and the unwritten ones are shared between every process that runs the same program. The user decided on 2026-09-29 to build the kernel side first and to replace `Copy` afterwards.

The kernel cannot do it today. `VmoDispatcher::create_child()` answers `NotSupported` for everything but `Slice`, and a slice is not copy-on-write: it shares its parent's `VmCowPages`, so a write through it lands in the parent, which for an executable is the file every process shares. `VmCowPages` (`kernel/mm/include/ours/mm/vm_cow_pages.hpp`) has a page store and a `Cursor`, but no parent, no parent offset, no notion of a node that only holds shared pages, and no per-page share count.

## Goals and non-goals
Goals:
- `vmo_create_child(SnapshotAtLeastOnWrite)` on a paged parent gives a child whose unwritten pages are the parent's, mapped read-only, and whose first write to a page copies it.
- The parent may be written after the clone, and neither side ever sees the other's later writes.
- Many children of one parent, which is every process that runs the same program, all hang under one shared node.
- A child larger than its parent reads as zero past the parent's end, through `ZERO_PAGE`, which is how `.bss` and the tail of the last file page come out zero.
- The mechanism is Zircon's bidirectional clone (a hidden node), for anonymous pages only.

Non-goals:
- Page sources and pagers, dirty tracking, discardable VMOs, compression, reclamation and eviction, unidirectional clones. Table in "Design" says which parts of Zircon these are.
- `Snapshot` (full), `SnapshotModified`, `Resizable`, `NoWrite`, `Reference`: each still answers `NotSupported`.
- Cloning a parent that has pinned pages: refused with `BadState`, as Zircon does.
- Any change to the launcher; that is the step after this one.
- Performance work. Where a choice caps the ceiling (the lock) it is a decision for the user, and the measurement comes after the feature works.

## Design
### Objects
- **Leaf**: a `VmCowPages` that a `VmObjectPaged` sits on. The only kind a user can see, and the only kind that is written.
- **Hidden node**: a `VmCowPages` with no `VmObject`. It holds the pages its children share and is never written directly, only copied or migrated out of. It has two or more children.
- **Parent link**: `m_parent`, `m_parent_offset` (where this node's byte 0 is in the parent) and `m_parent_limit` (this node sees parent bytes only in `[0, m_parent_limit)`; Zircon's `ClampedLimit`). Content past the limit reads as zero.
- **Share count**: per page, the number of leaves that can reach the content minus one (`vm_page_t::object.share_count`, `zircon/kernel/vm/include/vm/page.h:72`). Non-zero only on pages in a hidden node. Zero means one viewer.
- **`ZERO_PAGE`**: one shared, pinned, zeroed frame (`kernel/mm/init_vmm.cpp`), read-only wherever it is mapped.

### Events
- **Clone** (`CreateCloneLocked()`, `zircon/kernel/vm/vm_cow_pages.cc:1831`). If the parent has no page of its own in the range (it has been cloned before), the walk in `FindParentAndRangeForCloneLocked()` (`:1526`) finds the hidden node above it and the new child simply hangs under that node, which now has three children. Otherwise `CloneNewHiddenParentLocked()` (`:1582`) runs:
  1. Remove Write from every mapping of the original (`RangeChangeUpdateLocked` with `RemoveWrite`), because its pages are about to be shared.
  2. Move all of the original's pages into a new hidden node, at the same offsets, and rewrite each page's owner back-pointer through the page queues.
  3. Give every page in the child's window a share count, one more than it had.
  4. Link the original (offset 0, limit its size) and the new child (offset, limit) under the hidden node. The original keeps its identity and its `VmObject`; only its page list is now empty.
- **Read** (`FindPageContentLocked()`, `:2958`). Look in the node; if absent and the offset is below `m_parent_limit`, add `m_parent_offset` and look in the parent, up the chain. Nothing found, or past a limit: `ZERO_PAGE`. A page found anywhere but in the node itself comes back with `writable = false`, and is mapped read-only.
- **Write** (`CloneCowPageLocked()`, `:2671`). If the page is in the node itself, write it. Otherwise it is in a hidden ancestor: if its share count is above zero, copy it into the node and decrement the count (a fork); if zero, move the page itself into the node (a migrate), because no one else can see it. A write is never worse than a copy. The slot is reserved before anything is allocated, so a failure changes nothing. A fork also unmaps the writer's old read-only mapping.
- **Death** (`MaybeDeadTransition()`, `:1022`). A leaf dying frees the pages only it saw and decrements the rest. If its hidden parent is left with one child, `MergeContentWithChildLocked()` (`:2117`) moves the hidden node's pages into that child and splices the hidden node out.

### A write to `.data`, as a timeline
```text
launcher   vmo_create_child(file, SnapshotAtLeastOnWrite, off, size)
             file has pages -> hidden node H takes them; file and child hang under H
launcher   vma_map(child, RW)              -> mapping, no PTEs
program    reads .data page 1   -> fault (absent) -> walk up: H's page, count 1 -> PTE mapped R
program    writes .data page 1  -> fault (present, read-only) -> mapping allows W
                                -> count > 0: fork, copy H's page into the child, count 0
                                -> replace the PTE with the new page, mapped RW
program    writes .data page 1  -> no fault
second     launches the same program: a second child hangs under H; it still reads the
process    original page, and sees none of the first process's writes
```

### Fault and mapping rules
This is where copy-on-write can go wrong, and the one place the current code is actively in the way. In `VmMapping::page_fault()` (`kernel/mm/vm_mapping.cpp`), a present PTE whose permissions differ from the mapping's is treated as spurious and fixed with `arch_aspace.protect(va, 1, mmuf)`, without asking the VMO. With a shared page mapped read-only under a writable mapping, a write would take exactly that branch and make the shared page writable in place, and the write would land in every node that shares it. The rules:
- A page the cursor reports as not writable is mapped with `mmuf` minus Write, even in a writable mapping (Zircon `vm_mapping.cc:1318`). The mapping's own permission, the VMA's, stays Write; only the PTE is read-only.
- A write fault on a present PTE that is not writable, in a mapping that allows write, goes to `require_owned_page()`, then replaces the PTE with the new page. It must not go through `protect()`.
- `VmMapping::map_paged()` calls `require_owned_page()` for every page today; over a node with ancestors it asks for reads and maps by `writable`.
- `VmMapping::protect_locked()` widens permissions on a whole range and must not make a shared page's PTE writable (open question).

### Locking
Each node needs the lock that guards its page store, and a walk up the chain touches several nodes. Decision 3 says how. Today `VmObject::m_lock` is a reference to `VmCowPages::m_lock`, and a slice shares its parent's `VmCowPages`, so both would change with either option.

### Rights and options
`VmoDispatcher::create_child()` accepts `SnapshotAtLeastOnWrite` alone. Zircon's rights rule (`zircon/kernel/lib/syscalls/vmo.rs:255`): the parent handle needs Duplicate and Read but not Write; the child gets Write and loses Execute. Offset must be page aligned; size rounds up and may exceed the parent's. A slice is cloned through its `VmCowPages` with `m_cow_offset` added to the offset.

### What Zircon has that this leaves out
| Part | Why it can be left out |
|---|---|
| Page source, user pager, dirty state, zero intervals | No VMO has a page source |
| Unidirectional clones (`CloneChildLocked()` under a non-hidden parent) | Needs a pager-backed root |
| Discardable trackers, compression references, reclamation, eviction, page-queue aging, loaned pages | No such VMOs |
| Parent-content markers (`tree_has_parent_content_markers`) | An optimisation: an empty slot in a leaf can simply mean "look in the parent" |
| `TreeWalkCursor`, high-priority counts, attribution tracking, `ever_pinned_` | Only for the parts above, or accounting |

The core that stays: the hidden-node insert with its page-list move and back-pointer rewrite, the share count, the offset and limit arithmetic, `CloneCowPageLocked()` (fork or migrate), release plus merge on death, `RemoveWrite` on the original's mappings, and the unmap of the writer's old read-only mapping on a fork.

### Order of work
Each step is one commit, builds and boots, and is introduced to the user before it starts. The list can change as the steps teach us things.
1. `Cursor::require_*` return the page with a `writable` bit. Done, `add18462`.
2. One mutex per tree (decision 3): the lock moves out of `VmCowPages` into `HierarchyState`, which every node of a tree holds. No node has a parent yet, so every tree has one node and nothing changes. Done.
3. Done. `VmCowPages` gains the parent link (`m_parent`, offset, limit), the hidden flag and a factory that makes a node under a parent and adopts its `HierarchyState`. The read walk up the chain, with `ZERO_PAGE` past a limit and `writable = false` for a page from an ancestor. Tested by a kernel command `vmosnapshot.test` (modelled on `user_copy_test.cpp`, and run over COM1, since `boot_check.py` does not run kernel commands) that builds small chains by hand, including that a child still has its lock when its parent handle is dropped. The list of children comes with the clone and the merge (steps 5 and 6), the first code that walks it.
4. The share count on pages and the fork-or-migrate write, tested through `VmObject::read`/`write`.
5. The clone: the hidden-node insert, the page move, the back-pointer rewrite, and `RemoveWrite` on the original's mappings.
6. Death: release and merge.
7. The fault and mapping rules above, tested through a real mapping and `soft_fault`.
8. `create_child()` accepting the option, the rights, `ou::vmo_create_child`, and a check in `examples/shell/shell.cpp`.
9. The launcher: `Copy` becomes a snapshot child plus clearing the tail of the last file page.

## Alternatives considered
- **A one-level unidirectional clone of a parent that never changes** (the plan until 2026-09-29). Much smaller: no hidden node, no share count, no merge. It needs the parent sealed against writes so that no clone can observe a change, and it cannot serve a parent that is written later. Rejected by the user on 2026-09-29 for the hidden node; the `sealed` flag it needed is dropped.
- **Inserting the hidden node lazily, when the parent is first written after a clone.** The user's first suggestion. Rejected: it needs the parent to know all its children and to re-parent them under a lock at a moment when a write is already in flight, and it gives the nodes below the hidden one no simple "hidden nodes are immutable" rule. Zircon inserts at clone time.
- **Keeping the explicit `Copy`.** Simple and already written; its cost is a copy of the whole data segment at every launch and no sharing of unwritten pages. It stays until this lands.
- **A slice of the file VMO.** Not copy-on-write; a write would change the file.
- **A tag-aware lookup in `VmPageMap`** (`Owned` or `Borrowed`). Nothing inserts a `Borrowed` slot, and a child's page store holds only its own pages, so it has no caller. Decided against on 2026-09-29; the tag stays as it is.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Mechanism | Zircon's hidden-node clone made at clone time; the same, made when the parent is first written; one-level unidirectional clone of a sealed parent | Hidden node at clone time | None | Hidden node at clone time, 2026-09-29 |
| How a parent qualifies | A `sealed` flag; any paged parent | Not needed: the hidden node makes any paged parent safe | None | Dropped, 2026-09-29 |
| A read of a hole or the zero tail | Answer with `ZERO_PAGE`, read-only; allocate a zeroed page into the node on the read | `ZERO_PAGE` | None | `ZERO_PAGE`, 2026-09-29 |
| `VmPageMap` tag lookup | Add it; do not | Do not | None | Do not, 2026-09-29 |
| Lock | One mutex per tree, held by a reference-counted object that every node points to (Zircon's earlier design); one mutex per node with a lock-order number and the walk taking them in ascending order (Zircon's design now, commit `2dad682fe14`) | One mutex per tree first, measured later | "Only one accessor in a tree at a time; leaves are accessed concurrently, and the worst case is every leaf at once, so the others wait for one" (2026-09-29). Graded: right about the mechanism and the contention ceiling; it omitted the invariant (one page per slot, share counts, no half-linked tree, the mapping lists), the non-leaf accessors (faults, kernel read and write, clone, destruction, mapping changes), and the correctness interleavings (two forks of one page, migrate while another leaf reads it, a clone racing a fault); and the lock protects structure, never the bytes of a mapped page | One mutex per tree, 2026-09-29 (the user: use a plain mutex for now); to be revisited by the benchmark after step 7 |
| Replacing a PTE that maps a shared page | Unmap then map under the aspace lock; a new arch operation that swaps the frame in place | Unmap then map | None | |

The lock caps the ceiling. With one mutex per tree, every process that starts the same program faults on the same lock and every first write to a data page takes it; with a mutex per node the walk holds each only for one lookup, but takes several under a lock order that has to be argued and covered by lockdep. The tree's mutex cannot stay a member of the first `VmCowPages`, because that node can die while the others live; it has to be an object of its own that they share. This is a question for a benchmark in `benches/` after step 7, measuring time held, contention and latency with N processes.

## Testing and verification
- Kernel command `vmosnapshot.test` (from step 3): a child reads its parent's bytes without allocating, a write copies exactly one page and leaves the parent's unchanged, the parent's write after the clone is not seen by the child, the zero tail reads as zero and a write there allocates, a second and third child of one parent do not see each other's writes, a write to a page nobody else sees migrates it instead of copying (the allocation count is unchanged), a leaf dying merges the hidden node away, a clone of a parent with pinned pages is refused, and a write to a mapped shared page changes the writer and no one else (the hazard in the fault rules).
- `ours-verify` on both images at every step.
- Ring-3 check in `examples/shell/shell.cpp` (step 8).
- Not verifiable yet: behaviour under several CPUs (see the first open question), and any performance claim.

## Open questions
- **TLB shootdown, and it is now a correctness question.** Step 1 of a clone removes Write from the original's mappings, and `PageTable::protect_pages()` does a local `invlpg` only. Another CPU running the same address space can keep a stale writable translation of a page that has just become shared, and write into it. The one-level design avoided this because nothing was ever writable; this one cannot. It needs a cross-CPU invalidation in the `RemoveWrite` path before a clone is allowed on a machine with more than one CPU, or the clone must be refused there until it exists.
- Where the share count lives. `page::VmmBody` (`kernel/mm/include/ours/mm/pm_frame.hpp`) has `vmo`, `vmo_index`, `num_mappings`, `num_users` and `pinned_count`, and no count; Zircon puts a `uint32_t` in the same union.
- The page queues already hold a back-pointer per page (`set_queue_backlink_locked()` in `kernel/mm/page_queues.cpp`); moving pages into the hidden node has to go through it, as Zircon's `BatchPQUpdateBacklink` does.
- What `protect_locked()` should do to PTEs of shared pages (skip and re-fault, or track which are shared).
- What `commit_range` and pin mean on a node with ancestors: Zircon forks or migrates first.
- The merge can fail only on an allocation failure, and then leaves a one-child hidden node until its child dies (Zircon `vm_cow_pages.cc:2048`). Whether ours needs the same escape or can panic.
- `Resizable`, which musl passes to grow the tail of a child, is not needed if the child is created at its final size, as elfldltl does; this design assumes that.
