# btreefs-cow changes

## Title: the volume goes copy-on-write: transactions, two superblock slots, one commit point (2026-11-01)

**Files**
- `system/lib/ours.btreefs/` — the mutators rewritten onto `Transaction`:
  a cache of copied blocks, taken/dead lists, and `commit()` in a fixed
  order -- cached blocks, bitmap bits, the other superblock slot with the
  next generation. `update`/`insert`/`remove` copy every node they touch;
  `scan` no longer trusts the leaves' chain (a COW split does not reach
  back to the predecessor) and walks the tree in order instead. Both
  superblock slots carry a generation and a CRC32 over their first 128
  bytes (ours.gpt's); `open()` takes the highest sound generation. The
  superblock sits in blocks 0 and 1, the bitmap from block 2 -- an
  on-disk layout change, the old test volumes void.
- `system/service/btreefs/` — every mutating dispatch op (create, write,
  unlink, mount's root creation) runs one transaction and commits it; the
  transaction's ~200 KiB of storage lives in the Filesystem object, far
  past a user stack, begun fresh per operation with `begin()`.
- `scripts/make_gpt_disk.py` — lays the new-format volume: two CRC-signed
  slots at generation 1, the bitmap from block 2.
- `system/service/init/test/routing_checks/` — the inner-listing check
  reads dirents once (a second call answers the listing's continuation,
  nothing) and scans that reply.

**Symptom**
None — the planned second phase, the user's pick from the candidates. The
in-place tree corrupted on a crash mid-insert by design; nothing crashed it,
but nothing survived one either.

**Cause**
The commit point is one block write: until the other superblock slot with
the next generation lands, the disk holds the committed tree, its bitmap,
and its metadata -- whole and consistent, because nothing was modified
where it lies. A mutation copies each node it changes to a fresh block
(the original on the dead list, the committed tree still naming it), the
parent's pointer moving to the copy; blocks taken are on the taken list,
their bitmap bits untouched. `commit()` then writes the copies, sets the
taken bits, clears the dead bits, and swaps the slot. A crash anywhere
before that last write drops the transaction at zero cost -- the copies'
blocks were never marked used, so they are simply free. Freed blocks stay
referenced by the committed tree until the commit clears their bits, which
is also why a transaction never reuses its own frees.

**Change**
The three defects the first real run exposed, each invisible to a suite
that committed honestly:
- `cow()` read the original from the device; a block an earlier step of
  the *same* transaction created existed only in the cache, so the copy
  was of nothing. The read goes through the transaction's own view.
- `insert` never moved the shadow root to the walk's topmost copy, and
  `fix_child` took its underflow threshold from the parent's kind -- every
  commit after a rebalance wrote a stale root.
- The `Transaction` was a stack object: ~200 KiB of cache against a user
  stack, a page fault at first use. It lives in the Filesystem now.
And one latent driver defect the load uncovered, recorded beside this in
`virtio-blk-avail.md`: the avail ring's idx resetting at the queue size,
fatal at request 256.

**Reference**
Shadow paging by the btrfs shape: the transaction is `btrfs_trans_handle`,
the copied nodes are `__btrfs_cow_block`'s, the two superblock slots with
generations are `btrfs_root_superblock`'s primary/backup pair written
alternately, the CRC over the fixed prefix is `btrfs_csum_superblock`'s.
The commit order -- data, metadata-bitmap, superblock last -- is WAL's
rule in its shadow-paging clothing.

**Verification**
Host: 36 ours.btreefs tests pass -- the old 28 reworked onto the
transaction API (each mutation one `apply()`, one transaction, one
commit), plus the transaction's own: an abandoned insert changed nothing
(lookup misses, generation and root unchanged), an abandoned block take is
free again, generations climb and the slots alternate, a torn slot leaves
the survivor to `open()`, a CRC-tampered slot is refused. `boot_check.py`:
dbg PASS (44), rel PASS (42, 6/6 required), host-tests PASS 157. Disked:
`routing_checks: passed` (create, write, read back, subdirectory, inner
file, listings, unlinks), the host-side image shows the two slots at
generations 10 and 9 both CRC-sound and the freed block's bit clear; a
second boot on the same volume passes again with generations 18 and 17 --
the volume reopens and keeps committing. clang-tidy quiet.

**Deliberately not done**
- Group commit: one transaction per mutating operation, per the user's
  card; batching waits for a block cache, whose dirty blocks make it
  natural.
- No read cache: reads go to the device every time -- page cache is its
  own future milestone, not smuggled in here.
- The dead list and taken list are fixed at 64 entries and the block cache
  at 48: one operation's copies fit with room; an operation past that is
  refused NoResource rather than silently miscommitting.

**How it was found**
The first disked boot's failures in order: the stack fault at mount (the
transaction's storage), the stale-root bug visible as lookups missing
after honest commits, and the driver's request-256 wall -- each pinned by a
temporary witness line, then removed.

**The user's prediction**
None. The user picked COW and the double-superblock card; asked whether
group commit was page cache, they asked -- and the answer (no: page cache
is the read/write layer, group commit is the transaction layer's batching)
is recorded in the conversation, group commit deferred to the cache
milestone.

**Three questions you should be able to answer**
- Why is the freed block reusable by the *next* transaction without a
  generation check, and what would two concurrent committers break first?
- The commit writes cached blocks, then bitmap bits, then the superblock.
  A crash between the second and third leaves bits set for blocks whose
  new contents never landed -- why is that still consistent, and what does
  the next transaction do with those blocks?
- `scan` stopped trusting the leaves' chain -- what exact act of a split
  breaks the chain, and why does the in-order walk never notice?
