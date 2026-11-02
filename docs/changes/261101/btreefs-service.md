# btreefs-service changes

## Title: btreefs, the volume served as ours.io, mounted at /vfs/disk (2026-11-01)

**Files**
- `system/service/btreefs/` — the new component: ours.block's Client behind a
  4 KiB adapter, the ours-fs partition found through ours.gpt, the volume
  opened (or formatted when blank), and the ours.io Directory/File dispatch on
  memfs's connection shape -- Open, ReadDirents, Read, Write, Clone, Close.
- `system/lib/ours.btreefs/` — the superblock's object counter
  (`next_object`, `alloc_object`), `update` (a value replaced where it lies),
  `dirent_offset` folded to the key's 56-bit offset, and `insert`'s descent
  keeping block numbers only, nodes re-read on the climb back.
- `system/service/init/pools.hpp` — `MAX_INSTANCES` 8 to 10: dev carries one
  more component than the design's count named.
- `system/service/fshost/` — the optional `/disk` mount: no handle, no entry.
- `kernel/products/image/dev/BUILD.gn` — the btreefs child, the block
  protocol offered to it, its directory offered to fshost and routing_checks.
- `scripts/make_gpt_disk.py` — lays a formatted volume (superblock, bitmap)
  on the partition instead of a marker.
- `system/service/init/test/routing_checks/` — the `/disk` use and
  `check_disk()`: create, write, read back through a fresh open, and find the
  file in the listing; check_gpt checks the volume's magic now.

**Symptom**
None — a planned step. The B+tree existed only in host tests; nothing served
a real disk's volume, and no client could reach one through the namespace.

**Cause**
The library is format logic over a Device; the component is the plug: the
adapter turns 4 KiB blocks into the Client's 512-byte sectors (two chunks of
four, the codec carrying at most seven per call), the GPT names the partition,
and the ours.io dispatch turns fsio's calls into tree operations. A file's
bytes live in DATA items -- one per 4 KiB, the value naming the physical
block -- created as writes reach them; the INODE item's size is replaced with
`update` on every write that grows the file. A DIRENT's key offset is the
name's hash, and its value carries the name, so the listing walks the leaves'
chain in hash order and resumes from a cursor.

Three defects came out on the first real disk, none the host tests could see:
- `insert`'s descent kept its whole path of node buffers on the stack -- 16
  levels at 4 KiB each, 64 KiB against a user stack of far less. A page fault
  at sp-8. The descent now keeps block numbers and re-reads on the climb;
  no one else writes the volume, the Loop being one thread.
- `dirent_offset` returned the full 64-bit FNV-1a, and a key's offset is 56
  bits: the first name whose hash reached the top 8 bits was refused InvArgs
  at insert, which surfaced as a refused Open. The hash now folds its top 8
  bits into the bottom.
- `make_gpt_disk.py` wrote a marker at the partition's first block, and
  btreefs auto-formats a blank partition -- the format replaced the marker,
  and check_gpt failed on a byte btreefs had every right to write. The image
  now lays the formatted volume itself, and the check reads its magic.

**Change**
The volume a boot serves: the ours-fs partition opened; blank means
`format()` then open, so a fresh disk becomes a filesystem without a host
step. The root directory is object 1, made when the volume has no tree. File
reads zero-fill holes (a DATA item absent means never written); writes
allocate and insert as they reach each block and patch whole 4 KiB blocks;
sizes grow by `update`. ReadDirents lists "." then walks the DIRENT items,
resuming after the last one that fit the caller's buffer.

**Reference**
The dispatch is memfs's, method for method -- the Open checks in ioservice's
order, one message per wait, the per-connection Binding -- the shape the
guide's second chapter teaches. The DATA-per-block mapping is btrfs's file
extent items pared to one block each; the hole-reads-zeros is the sparse-file
semantics both Linux's `page_cache_seek_hole` world and Zircon's
`minfs` answer.

**Verification**
`boot_check.py`: dbg PASS (44 passed, 0 failed), rel PASS (42 passed, 8
skipped, 0 failed, 6/6 required) -- both diskless, where btreefs exits having
served nothing and /disk skips. With `scripts/make_gpt_disk.py`'s disk:
`routing_checks: passed` -- GPT parsed, the volume's magic checked, the file
created, written, read back byte for byte, and named in the listing; the
host-side disk file confirms the pattern at the partition's third block under
the BTREEFS superblock. Host tests PASS 157 (the library's, unchanged but for
the hash fold). clang-tidy quiet on the new sources.

**Deliberately not done**
- No delete: nothing removes a file or truncates one -- the fs-level delete
  and the tree's item removal come with the next step, and the freed blocks
  they must give back.
- No directories below the root: the flat root, memfs's shape, until the
  mount story needs more.
- The crash windows stay open (dirent without inode, a data block written
  before its size lands): the copy-on-write phase's material, recorded in the
  design.

**How it was found**
The first disked boot's three failures, in order: the lockup report's page
fault at sp-8 (the stack), the refused Open traced by a temporary dispatch
log that named the path and flags (the hash), and check_gpt's mismatch read
against what btreefs had written (the marker versus the format). None of the
three was visible to the host suite; each needed the real disk.

**The user's prediction**
None. The user approved the step ("ok") after the design cards.

**Three questions you should be able to answer**
- Why does a refused Open surface as InvArgs at the first write, three
  methods later, rather than at the Open itself?
- The descent re-reads each node on the climb back -- what does that assume
  about who else writes the volume, and which server shape makes it true?
- A blank partition becomes a filesystem at boot: what does that decide for
  the host-side image script, and what would break if btreefs instead
  refused to serve an unformatted partition?
