# btreefs-remove changes

## Title: the tree's delete, and the file's unlink behind ours.io's Unlink (2026-11-01)

**Files**
- `system/lib/ours.btreefs/` — `remove()`, the full rebalance: leaf erase,
  borrow from either sibling, merge with the cascade up the parents, the root
  shrinking when one child is all it has left; the blocks of merged-away
  nodes go back to the bitmap. Six host tests beside the 22.
- `system/lib/ours.io/` — `Directory.Unlink`, a two-way call whose request
  carries the entry's name and whose reply is the status form.
- `system/lib/fsio/` — `unlink(path)`: the namespace walk, then the call.
- `system/service/memfs/` — Unlink empties the pool row.
- `system/service/btreefs/` — `unlink_file()`: the file's DATA items leave
  one at a time, each block freed as its item goes, then the INODE, then the
  DIRENT last.
- `system/service/init/test/routing_checks/` — `check_disk()` unlinks the
  file it made: a second unlink misses, a read of the reopened name misses,
  the listing no longer names it.

**Symptom**
None — a planned step. A filesystem that cannot delete grows forever; the
tree had insert's split but nothing for the underflow removal leaves.

**Cause**
Deletion is the half of the B+tree the insert never shows: a removal can
leave a node past its minimum, and the minimum is what keeps lookups bounded
-- a chain of half-empty leaves is a tree in name only. The rule the code
follows: a node below `LEAF_MIN`/`INTERNAL_MIN` takes one item from a
sibling that can spare it (the parent's separator between the two moves with
the item), or merges with one when neither can spare -- the merge taking a
routing pair out of the parent, which may itself drop past its minimum, and
the same rule climbs. An internal root with a single child is a level for
nothing; the child rises. The root alone is exempt from the minimum: it
carries what the tree needs, one key included.

**Change**
`remove_from()` recurses down the path (height at most 16, one frame per
level) and answers whether the node it just trimmed underflowed;
`fix_child()` rebalances at each parent -- left borrow, right borrow, else
merge, the left one preferred so the underflowed child is the node that
dies. `unlink_file()`'s order is the crash-safety order: each DATA item is
removed and only then is its block freed, so a crash mid-way leaks blocks
but never leaves a name over freed data; the DIRENT goes last, the name
surviving until nothing is under it. The file's first DATA item each round
is found by a scan that skips the INODE item sorting before the DATA ones
and stops at the next object's items.

Three byte-level defects the tests caught before any boot did, all in the
merge-and-borrow surgery: `internal_erase`'s shift stopped 16 bytes short,
leaving the parent's last child pointing at the node the merge had just
freed; `internal_borrow_left`'s shift ran the wrong way over its own bytes;
`internal_merge` copied the right sibling's slots from its second one, so
every child landed one position high and the last child never moved at all.

**Reference**
The rebalance is CLRS's B-tree-delete -- cases 1 through 3a/3b, borrow and
merge -- against the B+ shape whose data lives only in leaves: Linux's
`__btree_del` paths in `lib/btree.c`, Zircon's absent equivalent (no in-kernel
B-tree). `Directory.Unlink` is Fuchsia's `fuchsia.io/Directory.Unlink` pared
to this protocol's status form.

**Verification**
Host: 28 ours.btreefs tests pass -- the misses, the borrow both ways with
every lookup checked after each removal, 10'000 items removed one by one
until the tree is a single leaf again and every node's block but the root
leaf's is back in the bitmap, the scan ordered past 250 removals, the root
leaf's block kept. `boot_check.py`: dbg PASS (44), rel PASS (42, 6/6
required), host-tests PASS 157. With the GPT disk: `routing_checks: passed`
-- the file created, written, read back, unlinked, the second unlink
NotFound, and the host-side disk file confirms the freed block's bit is
clear in the bitmap while its stale bytes remain (free clears bits, not
bytes). clang-tidy quiet.

**Deliberately not done**
- Truncate: FileTruncate's flag is refused implicitly -- no item removal
  from a file's middle, only whole-file unlink. It wants the same DATA-item
  walk with a new size, and nothing calls for it yet.
- No delete of a directory subtree: the flat root has nothing below it.
- The freed block's stale bytes: free clears the bitmap bit and nothing
  else, the standard laziness -- a reused block is written whole before the
  device reads it, and a zeroing pass is performance work.

**How it was found**
None.

**The user's prediction**
None. The user picked full rebalance over lazy deletion from the two cards,
and "你写" for the merge-and-borrow core, the hand-written-function offer
declined again.

**Three questions you should be able to answer**
- Why does the underflowed child's merge free the child and not the
  sibling, and what would break if the parent kept a routing pair to an
  empty node?
- Which of the three surgery bugs is invisible to lookups of a tree whose
  internal nodes never merge -- and which test class caught it?
- Unlink removes data items, then the inode, then the dirent. Which crash
  between them loses what, and why is every answer acceptable?
