# btreefs outstanding repairs

## 1. One unlink or truncate transaction holds every remove's copies: the shadow cache exhausts at ~16 removes, and files over ~64-80 KiB cannot be deleted or O_TRUNC-opened

**Severity**
High: wrong results on a path that runs today — a user creates a file of a few hundred KiB, and no `Unlink` or `FileTruncate` open can ever remove it; every attempt answers `NoResource` and the blocks stay allocated for the volume's life.

**Symptom**
`unlink` of a file larger than roughly 64-80 KiB fails with `NoResource`; `open(..., FileTruncate)` of one refuses. Small files unlink fine.

**Cause**
`unlink_at` (`system/service/btreefs/btreefs.cpp:814-851`) and `truncate_file` (`ours_btreefs.hpp:1143`) loop `remove()` over **one** `Transaction` (the atomicity unit — `btreefs.cpp:791`). Every `remove` cows the root and the leaf again even when they are already this transaction's copies (`cow` → `read_block` hits the cache → `take()` appends a **new** entry, `ours_btreefs.hpp:445-465`), plus up to two sibling copies in `fix_child` (`:1019, :1030`), minus one on a merge's `free_block`. Nothing shrinks the cache mid-transaction: 48 entries (`CACHE_ENTRIES`, `:390`) at 3-4 per remove exhausts at ~16 removes ≈ 16 × 4 KiB leaves ≈ 64-80 KiB. The header's sizing note (`:378-380`) budgets one remove, not a loop. Latent worse: `cached()` (`:419-429`) returns `{}` on a full cache, and every `Node` built over it (`:899, 947, 976, 991, 1109, 1208`) would be a zero-length span with the header written out of bounds — today unreachable only because `cow` fails first.

**Change**
Two shapes:
- **Reset the cache between removes** (keeping `taken`/`dead`, which are the transaction's durable intent): after each successful `remove`, the previous removes' copies are unreachable — no later operation can read them — so dropping the cache entries is safe and the ladder never climbs. Recommended.
- Size `CACHE_ENTRIES` for the worst file (MAX_FILE_BLOCKS × path depth) — dozens of KiB of static storage for a bound that still exists.

Either way, add the `cached()`-empty guard (`BadState`) so the latent OOB shape cannot ship behind a future caller, and the host test that unlinks a 256 KiB file.

**Reference**
None, this project's own design; the taken/dead lists already separate "durable intent" from "this transaction's scratch", which is exactly the split the reset leans on.

**Acceptance**
`unlink` of a 256 KiB file passes in the host suite; `open(FileTruncate)` of one passes on the disked run.

## 1. Nodes are trusted from the device: the entry count is never validated against capacity

**Severity**
Medium: latent, needs on-disk corruption (a flipped byte in a leaf's count field) — but the device is a real disk, the superblock is CRC'd and nodes are not, and the failure is memory corruption, not an error.

**Symptom**
Latent: a corrupted count drives `upper_bound`'s `key(mid)` ~5 MiB past the 4 KiB span and `insert_leaf`'s `memmove` writes there — memory corruption, not an error.

**Cause**
`Node::Node` (`system/lib/ours.btreefs/src/ours_btreefs.cpp:121-124`) loads the count from bytes with no clamp; capacity is known (`capacity()`, from the same span).

**Change**
One clamp at construction: `m_count > capacity() →` treat the node as invalid (`BadState` from every op on it), or refuse the block at read time.

**Reference**
None, this project's own design; the block device's bytes were never promised to be node-shaped (`docs/repairs/261031/btreefs.md`'s corrosion notes make the same point for the superblock).

**Acceptance**
A host test corrupts a count byte and every operation on the node answers `BadState`.

## 2. `read_dirents`' cursor skip uses `<=`, so an entry whose folded hash is 0 is never listed

**Severity**
Low: probability ~2⁻⁵⁶ per name, but the file exists and is invisible in listings (findable by open).

**Symptom**
Latent: a DIRENT whose folded FNV offset hashes to exactly 0 is skipped on every call — the listing omits it forever, while `find_child` still opens it.

**Cause**
`system/service/btreefs/btreefs.cpp:631` skips with `key.offset <= connection.next_offset`, and the initial cursor is 0: an entry at offset 0 is treated as already streamed.

**Change**
One character, `<=` to `<`; check the initial-cursor case (an entry at offset 0 must list).

**Reference**
None, this project's own design (the folded-offset listing cursor).

**Acceptance**
A host test with an entry whose folded offset is forced 0 lists it.

## 3. The class-level commit-order contract contradicts the code, and `free_block`'s documented double-free detection cannot fire within one transaction

**Severity**
Low: documentation as bug. `ours_btreefs.hpp:53-56` says commit writes "the bitmap …, then the other superblock slot"; the code (`:556-591`) clears dead bits **after** the swap — the safe order, and `commit()`'s own comment states it. And `free_block`'s `BadState` double-free check reads the bitmap from the device, where a dead block's bit is still set until commit, so an intra-transaction double free appends a duplicate dead entry silently instead of answering BadState (benign at commit, but the documented detection does not exist).

**Symptom**
Latent: a reader of the class doc implements the documented order and ships a crash-window; an intra-transaction double free is silently ignored where the doc promises BadState.

**Cause**
The class doc (`ours_btreefs.hpp:53-56`) states the commit's opposite order from `commit()`'s own (`:546-555`), and `free_block`'s BadState check reads the device bitmap, where a dead block's bit stays set until commit.

**Change**
Fix the class doc to state the real order; either check `dead` first in `free_block` or document that intra-transaction double frees are silent.

**Reference**
The code's own `commit()` comment, which states the safe order the class doc contradicts.

**Acceptance**
Doc and code state the same order; a host test frees one block twice in a transaction and the outcome matches the (chosen) contract.
