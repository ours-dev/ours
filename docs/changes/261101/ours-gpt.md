# ours-gpt changes

## Title: ours.gpt, the partition table read over the block protocol, and the block client it reads through (2026-11-01)

**Files**
- `system/lib/ours.gpt/` — the new library: IEEE CRC-32, the header and entry parses, and `read(Reader&)`, which pulls the table block by block through whatever reads blocks. Host tests beside it, wired into `//:default`.
- `system/lib/ours.block/include/ours_block/client.hpp` — the client side of the codec: txid counter, the two buffers, the wait, the epitaph check, and `read_blocks()`, the one method ours.gpt's Reader asks for. Its own `cpp_interface` target, since it makes system calls and the codec alone must keep building on the host.
- `system/lib/ours.block/BUILD.gn` — the `client` target.
- `scripts/make_gpt_disk.py` — writes the raw GPT image the booted check reads: protective MBR, signed header, one ours-fs partition with a marker at its first block.
- `system/service/init/test/routing_checks/routing_checks.cpp` — `check_gpt()`, and `check_block()` split: the block-0 magic belongs to the plain disk, the write check runs on either shape.
- `BUILD.gn` — ours.gpt's tests into the host set.

**Symptom**
None — a planned step. The block protocol answered in sectors; nothing on the disk said what the sectors meant. The macro filesystem's next milestone mounts concrete filesystems out of partitions, and that wants a reader for the table that names them.

**Cause**
A disk without a partition table is a bag of blocks: the fshost that composes `/vfs` would have no way to know which range of the disk a filesystem lives in, or which of several filesystems a partition carries. GPT is that table: a header at LBA 1 that vouches for itself (CRC32) and for an entry array (CRC32 again), each entry naming a partition by type GUID, range and name. The two CRCs are the design's spine — any byte that names a partition is covered, so a parse that survives them is a parse of what was written.

**Change**
The library takes no protocol dependency: `read()` is a template over a Reader whose one method is `read_blocks(lba, count, out)`, so the booted path passes `ours_block::Client` and the host tests pass a fake over an in-memory disk. The flow: LBA 0's boot signature; LBA 1's signature, revision, self-CRC, and field sanity; the entry array one block at a time, the CRC accumulated over exactly the bytes the header vouches for, none of the padding; the recorded CRC compared last. A type GUID of all zero is a free slot, skipped. The name's UTF-16LE decodes to ASCII below 0x80 and '?' elsewhere. Errors: NotFound (no GPT — the caller's skip case), BadState (CRC), InvArgs (unreadable fields).

The Client is the same shape the servers got: the plumbing a caller would re-paste (txid, buffers, wait, epitaph) in one place. Its one non-obvious duty: a closed connection's epitaph is a message until asked, and it must answer the epitaph's status, not feed the 24 bytes to the reply decoder — the InvArgs that check answered on the first diskless rel boot came from exactly that.

**Reference**
The UEFI specification's GPT layout, chapter 5.3; the CRC-32 is ISO 3309's, the one zlib ships. Linux's counterpart is `block/partitions/efi.c` (`efi_partition()`), which reads the same two CRCs and refuses the table the same way. The Client stands to ours.block as Zircon's generated FIDL client stands to a FIDL protocol — here written by hand, per D20.

**Verification**
Host: 13 ours.gpt tests pass (the CRC-32 against its published known answer `123456789` → `0xCBF43926`, the parses and their refusals, whole tables through the fake reader including an array that spans blocks). `boot_check.py`: dbg PASS (44 passed, 0 failed), rel PASS (42 passed, 8 skipped, 0 failed, 6/6 required), host-tests PASS 156. Three disk shapes booted: no disk (both checks skip), the plain image (magic + write check, GPT skipped), `scripts/make_gpt_disk.py`'s image (GPT parsed, ours-fs found at LBA 2048, marker read back, write check run at LBA 1000 outside the partition) — `routing_checks: passed` on all three. clang-tidy quiet on the new sources.

**Deliberately not done**
- Only 512-byte-block devices: the read buffers are one block, and a wider device is a change to make when one arrives (InvArgs until then).
- The backup header at the disk's end is not consulted: the primary header's CRC decides. A disk whose primary is torn but backup intact reads as no GPT, which is honest, and the recovery path waits for a filesystem that can be rewritten to.
- The partition's GUIDs print nowhere and the name keeps no UTF-16: the check compares bytes, and a converter arrives when a user of the name needs one.
- `check_gpt()` does not try to write into the partition: the write check already covers the path, at an LBA the script keeps outside the partition on purpose.

**How it was found**
None.

**The user's prediction**
None. The user approved the step ("可以") after its objects, events and deliverable were laid out.

**Three questions you should be able to answer**
- Why does the entry array's CRC accumulate over `num_entries * entry_bytes` rather than the blocks it was read in, and what does a disk whose padding differs test about the parser?
- The Client answered InvArgs on a diskless boot before the epitaph check went in: what is an epitaph on the wire, and why must a client ask before it decodes?
- Why is `read()` a template over a Reader rather than a parameter of type `ours_block::Client&`, and what does that cost the booted caller?
