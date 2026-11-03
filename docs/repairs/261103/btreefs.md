# btreefs outstanding repairs

## 1. A new item keeps the padding bytes of the value that was encoded before it

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

## 2. A failed device read or a full cache makes a node over an empty span

**Severity**
Medium: not reached with a healthy device and a bounded operation, and an out-of-bounds read when it is.

**Symptom**
Latent, cannot be triggered today. About fifteen places in `ours_btreefs.hpp` build `detail::Node{tx.cached(block)}` and use the node without looking at the span. `cached()` answers an empty span when the device read fails or the 48-entry cache is full, and `Node`'s constructor reads `bytes[0]` of it.

**Cause**
`cached()` reports failure by an empty span, which nothing forces a caller to check. The four places that make a new node no longer use it (`fresh()` answers a Result), but the places that read an existing node do.

**Change**
Make `cached()` answer a `Result`, or give the walk a helper that answers `Result<Node>`, and propagate the error from each place. The failure is `NoResource` for the full cache and the device's status for a failed read. This item does not depend on item 1.

**Reference**
None, this project's own design.

**Acceptance**
A fake device that fails reads: `lookup`, `insert`, `remove` and `scan` answer an error and read nothing out of bounds, under a sanitizer.
