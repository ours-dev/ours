# btreefs: the on-disk filesystem the ours-fs partition carries

**Date**: 2026-11-01
**Status**: Proposed

## Problem
The ours-fs partition that `check_gpt` finds is typed but empty. The macro
filesystem's milestone list needs a concrete filesystem to mount into `/vfs`,
and the user chose to design one rather than adopt FAT: a B+tree filesystem,
whose on-disk format this project defines itself.

## Goals and non-goals
Goals: files and directories, read and write, "basically usable" behind the
ours.io protocol the fshost already forwards; every on-disk byte's meaning
written down here or in the code's comments. The B+tree is the point: lookup,
insert and split are the interview material.

Non-goals for v1: permissions and ownership (the capability layer answers who
may open), symlinks and hard links, crash consistency (in-place update, no
journal, no COW -- COW is the deliberate second phase), volumes above ~1 GiB,
renames of directories, sparse files.

## Design

### Objects
- A block is 4 KiB, eight sectors, the unit every structure lives in.
- Superblock, block 0: magic `BTREEFS\0`, version, block count, the root
  node's block number, the bitmap's location, the next unused object id.
- Free bitmap: contiguous blocks from block 1, one bit per block, count in
  the superblock.
- B+tree node, one block each: a header (leaf flag, entry count, next-leaf
  pointer for range scans) and then slots. Internal nodes hold keys and child
  block numbers; leaves hold keys and values, chained left to right.

### The one tree
Key, 16 bytes: `u64 object_id`, then `u64` packing `type` in the top 8 bits
and a 56-bit offset below it. `object_id` 0 is reserved. Types: `INODE`
(offset 0; the object's size and kind), `DIRENT` (offset = the FNV-1a hash of
the name; the value carries the name itself and the child's object id),
`DATA` (offset = the block's index within the file; the value carries the
physical block). A file's size lives in its INODE item; a directory's
children are its DIRENT items. One lookup path, one insert path, one split --
the type byte is the only thing that differs.

Values are fixed-size, 64 bytes, interpreted per type, so slots are uniform
80 bytes on disk and no node carries variable-length packing. DIRENT names
are capped accordingly (v1 refuses longer names with InvArgs); a hash
collision between two names in one directory refuses the insert and is
recorded as a known v1 limit.

Update is in place: insert walks to the leaf, makes room, splits on full by
writing a new right sibling and pushing the separator up, parents the same.
A full path rewrite is not atomic and a crash mid-write corrupts the tree;
that is accepted for v1 and revisited as COW, which will redo the allocator
and the commit -- the redo is the second phase's material.

### Placement
The library (`system/lib/ours.btreefs`) is format logic only: nodes, keys,
lookup, insert, split, allocate, free -- templated over a block device with
`read_block`/`write_block`, as ours.gpt is templated over a Reader. The
component (`system/service/btreefs`) plugs ours.block's Client into it and
serves ours.io's Directory, so fsio clients and the fshost mount work without
new protocol work.

### Concurrency (the part to teach)
The fs serves on the ProtocolServer base: one `on_message` at a time, so the
tree is touched by one thread and the v1 answer is "the Loop serializes".
The design question this sets up for the COW phase: which lock covers what,
and what a reader sees mid-insert.

## Alternatives considered
- FAT16 (recommended earlier, declined by the user in favour of this design):
  no tree to build, less to learn.
- Separate namespace tree + per-file extent trees + inode table: the
  conventional split; declined for the one-code-path property and because the
  cross-structure consistency work buys no teaching here.
- COW from day one: crash-consistent immediately, but the first milestone
  would carry the allocator, transaction and root-swap all at once; the user
  chose to reach usable first and meet COW as a rewrite.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| On-disk model | single unified tree / separate trees | single tree | -- | single tree, 2026-11-01 |
| Update strategy | in-place first / COW from day one | in-place first | -- | in-place first, 2026-11-01 |

## Testing and verification
Host tests of the format library over an in-memory device: lookups, inserts,
splits that climb several heights, range scans, the allocator's exhaustion
and the collision refusal. Booted: the component serves a crafted image and
fsio reads a written file; fshost mounts the partition into /vfs and
routing_checks reads through it. Performance: not in this task's scope.

## Open questions
- The milestone's hand-written function: the leaf split is the candidate.
- Whether DIRENT's fixed name cap is 51 bytes or the value grows to fit
  255-char names later -- decided with the first real directory test.
