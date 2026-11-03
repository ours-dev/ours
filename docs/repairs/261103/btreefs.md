# btreefs outstanding repairs

## 1. A new file's partial write shows the bytes of the block's last owner

**Severity**
Medium: another file's bytes become readable as content, but only through a case that needs two connections and a freed block that was reused.

**Symptom**
Not run; found by reading. Connection A holds a seek position of 5000 in a 5000-byte file. Connection B opens the file with `FileTruncate`, and the size drops to 0 while A's position stays 5000. A writes 5 bytes: block 1 has no item, so `write_file()` allocates one, and since the write is partial it reads the new block from the device and patches it. Bytes 0 to 903 of that block are whatever the block held before, the size becomes 5005, and `read_file()` returns those bytes as content, although its comment says only a missing item reads as zeros. The dev disk image is zero-filled, so this shows only once a block has been freed and allocated again.

**Cause**
The same mistake as the node count of `docs/changes/261103/btreefs.md`, on data blocks: a block just allocated is read from the device and used as if it were initialised. In `system/service/btreefs/btreefs.cpp` (`write_file()`, the branch that allocates when the lookup answers NotFound, then the read-patch-write at the partial-write test), the device's copy of a free block is what its last owner left.

**Change**
When the lookup answers NotFound, start from a zeroed buffer and do not read the device; keep the read for a block that already has an item. A boot check or a service-level test needs a disk image whose free blocks are not zero.

**Reference**
Linux `__block_write_begin_int()` (`fs/buffer.c`) zeroes the parts of a newly allocated buffer outside the written range (`zero_user_segments()`). From memory, not opened.

**Acceptance**
A test that writes a few bytes into a new block of a device whose free blocks hold a non-zero pattern, and reads back zeros around them.

## 2. A new item keeps the padding bytes of the value that was encoded before it

**Severity**
Low: no reader looks at the padding, but the bytes on disk depend on what was created before, and a file name ends up in an unrelated item.

**Symptom**
Latent, cannot be triggered today by any reader. `create_object()` encodes the DIRENT into a scratch `Value` and then `encode_inode_value()` rewrites only bytes 0 to 8 of the same `Value`, so the new INODE item carries the DIRENT's name length and name bytes in its padding. `write_file()` does the same with the `Value` of the previous lookup for a new DATA item.

**Cause**
The encoders write the fields they own and leave the rest, and the callers reuse one `Value`.

**Change**
Start each encode from `ours_btreefs::Value{}`.

**Reference**
None, this project's own design.

**Acceptance**
A test that creates a file and checks bytes 9 onward of its INODE item are zero.

## 3. A failed device read or a full cache makes a node over an empty span

**Severity**
Medium: not reached with a healthy device and a bounded operation, and an out-of-bounds read when it is.

**Symptom**
Latent, cannot be triggered today. About fifteen places in `ours_btreefs.hpp` build `detail::Node{tx.cached(block)}` and use the node without looking at the span. `cached()` answers an empty span when the device read fails or the 48-entry cache is full, and `Node`'s constructor reads `bytes[0]` of it.

**Cause**
`cached()` reports failure by an empty span, which nothing forces a caller to check. The four places that make a new node no longer use it (`fresh()` answers a Result), but the places that read an existing node do.

**Change**
Make `cached()` answer a `Result`, or give the walk a helper that answers `Result<Node>`, and propagate the error from each place. The failure is `NoResource` for the full cache and the device's status for a failed read. This item does not depend on item 1 or 2.

**Reference**
None, this project's own design.

**Acceptance**
A fake device that fails reads: `lookup`, `insert`, `remove` and `scan` answer an error and read nothing out of bounds, under a sanitizer.
