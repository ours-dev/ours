# btreefs changes

## Title: ours.btreefs, the B+tree the ours-fs partition carries, over blocks (2026-11-01)

**Files**
- `system/lib/ours.btreefs/` — the new library: the format (superblock, free
  bitmap, B+tree nodes), the key and value codecs, and the operations --
  format, open, alloc/free, lookup, insert, scan -- as templates over a block
  device with read_block/write_block. 22 host tests beside it, wired into
  `//:default`.
- `BUILD.gn` — ours.btreefs's tests into the host set.

**Symptom**
None — a planned step. The ours-fs partition found by check_gpt has a type
and no contents: nothing yet says what bytes a filesystem puts in it, and the
user chose to design one (BTreeFS) rather than adopt FAT.

**Cause**
The design (.agents/tasks/btreefs/DESIGN.md, the user's two decisions of
2026-11-01) fixes the shape: one B+tree for the whole volume, its key naming
(object, type, offset) so a directory's children, a file's blocks and an
object's metadata are all items of the same tree and lookup, insert and split
are one code path; and in-place update for v1, copy-on-write being the
deliberate second phase. Blocks are 4 KiB; a leaf's slot is a 16-byte key and
a 64-byte value, an internal node's a child and a key; the free bitmap hangs
off the superblock at block 0.

**Change**
The library owns format and nothing else: `format()` lays the superblock and
the bitmap, `open()` reads them back, `alloc_block`/`free_block` run the
bitmap (metadata blocks are never freeable), `lookup` descends by upper
bound, `insert` splits full leaves -- the right sibling's first key becoming
the separator that climbs, internal nodes splitting the same way, a root's
split growing the tree -- and re-links the leaves' chain, `scan` walks that
chain from a start key. DIRENT keys hash the name with FNV-1a into the
56-bit offset; the value carries the name, so the hash only routes. The
separator is a routing copy, which is why an internal node routes to the
child past the last key not greater than the target. The insert's descent
keeps block numbers only and re-reads nodes on the climb back, so a stack
frame costs 128 bytes, not 64 KiB.

**Reference**
The tree is B+tree by the book: separators as copies, data only in leaves,
the leaf chain for ranges -- the shape of btrfs's `__btrfs_cow_block()` world
without the COW, of XFS's directory blocks, of Linux's `insert_in_node()`
paths in `fs/btrfs/ctree.c`. The value-carrying DIRENT is btrfs's
`(dir, DIR_ITEM, name_hash)` key pared to fixed size. The library standing
free of any protocol via a one-method template is ours.gpt's Reader again.

**Verification**
Host: 22 tests pass -- the key order, the codecs and their refusals, the
allocator (distinct blocks, free-and-reuse, double-free and metadata refused,
NoSpace), lookups on an empty volume, duplicates refused, splits keeping
every item at height 2 (102 items), height 3 (10'000 items, the root's split
included), descending inserts, objects kept apart, scans in order and from a
start key with an early stop, the root surviving open. `boot_check.py`: dbg
PASS (44 passed, 0 failed), rel PASS (42 passed, 8 skipped, 0 failed, 6/6
required), host-tests PASS 157. clang-tidy quiet on the new sources.

**Deliberately not done**
- No booted component yet: plugging ours.block's Client in and serving
  ours.io is the next step; the fake device proves the format, not the wiring.
- No free/delete of tree items yet: nothing removes an item, so nothing
  frees a file's blocks -- the write path of the component will need it, and
  the deleting-a-node-may-empty-it case comes with it.
- The DIRENT hash-collision insert is refused, not resolved: recorded as the
  known v1 limit the design names.

**How it was found**
None.

**The user's prediction**
None. The user picked both design decisions from cards, and "你写" for the
split (the hand-written-function offer of the milestone, declined this time).

**Three questions you should be able to answer**
- Why does an internal node route to the child past the last key that is not
  greater than the target, and what would a lookup for exactly a separator's
  key find under the other rule?
- The descent keeps block numbers instead of node buffers -- what does that
  trade, and what did it save?
- What does "in place" mean for a crash between the leaf split and the
  parent's update, and which second-phase design choice makes that
  impossibility disappear?
