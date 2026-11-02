# btreefs-truncate changes

**Files**
- `system/lib/ours.btreefs/` — `truncate(tx, object, from_block)`: every
  DATA item of the object at `from_block` and beyond leaves, its block back
  to the transaction as it goes; the INODE is the caller's to size. Two
  host tests beside it.
- `system/service/btreefs/btreefs.cpp` — `FileTruncate` on an open of an
  existing file empties it: one transaction takes the blocks and zeroes the
  INODE's size, so a crash leaves the file whole or empty, never half.
- `system/service/init/test/routing_checks/` — check_disk reopens the check
  file with `FileTruncate` and reads zero bytes.

**Symptom**
None — a planned gap. The `FileTruncate` flag existed in the protocol and
was ignored: reopening an existing file "for overwrite" kept the old bytes,
so a ten-byte write over a full block read back as new bytes atop stale
ones.

**Change**
Truncate is unlink's data walk without the farewell: the same
first-DATA-item-per-round scan (from `from_block`, past the INODE item,
stopping at the next object's items), each item removed and its block
freed inside one transaction. The component adds the INODE size zeroing to
the same transaction. Reading the item back through the transaction's own
view -- not the device -- is what lets the loop terminate: the first
truncated item vanishes from the transaction's tree, and the next scan
starts from `from_block` again finding the next one.

**Reference**
POSIX `open(O_TRUNC)` / `ftruncate`, Fuchsia's `fuchsia.io/File` with the
`OpenFlags.FILE_TRUNCATE` bit this protocol's flag answers to. Linux's
`ext4_truncate()` frees the tail blocks through the same
free-after-unhook order this transaction encodes, with the journal standing
where the commit point stands here.

**Verification**
Host: two tests -- a cut from block 2 leaves items 0 and 1, removes 2 to 5,
and the next four allocations are distinct and none is a kept item's block;
a cut past the end and of an object with no items answer Ok. `boot_check.py`:
dbg PASS (44), rel PASS (42, 6/6 required), host-tests PASS 157. Disked:
`routing_checks: passed` -- the check file reopened with `FileTruncate`
reads zero bytes before its unlink. clang-tidy quiet.

**Deliberately not done**
- Partial truncate to a nonzero length: the component only ever asks for
  block zero (the flag's O_TRUNC semantics); the library takes
  `from_block`, so a nonzero cut is a small step when something wants it.
- memfs stays as it was: its files are memory rows a rewrite overwrites
  anyway.

**How it was found**
None.

**The user's prediction**
None. The user asked what truncate was, then approved the step.

**Three questions you should be able to answer**
- Why does the scan loop read the item back through the transaction instead
  of trusting the scan's visit -- what did the visit already consume?
- The INODE size update rides the same transaction as the block frees:
  which intermediate state does that rule out, and what would a reader see
  if the two committed separately?
- `truncate` answers Ok for an object with no items past the cut -- why is
  that not NotFound's job?
