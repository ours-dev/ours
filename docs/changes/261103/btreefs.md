# btreefs changes

## Title: a new B+tree node starts from a zeroed block, not from the bytes the block last held (2026-11-03)

**Files**
- `system/lib/ours.btreefs/src/ours_btreefs.cpp` — `Node::leaf()` and `Node::internal()` zero the whole 16-byte header and then set the leaf flag
- `system/lib/ours.btreefs/include/ours_btreefs/ours_btreefs.hpp` — `Transaction::fresh()`; the four node-creation sites in `insert()` use it and propagate its error; comments of `cached()`, `Node` and the cache bound
- `system/lib/ours.btreefs/test/ours_btreefs_test.cpp` — `FakeDevice::fill_from()` and six new tests
- `docs/repairs/261103/btreefs.md` — three items found on the way (data blocks, padding, unchecked `cached()`)
- `docs/repairs/261103/routing-checks.md` — the intermittent `check_gpt` failure seen while verifying this

**Symptom**
The first insert or the first split on a volume whose free blocks do not hold zeros builds a node with a wrong entry count and a wrong chain link. On a device filled with `0xA5` after formatting, 10 000 ascending inserts end at height 4 instead of 3 and a scan returns 10 051 items instead of 10 000; a lone `Node::leaf()` over a buffer of `0xFF` reports 51 entries (the capacity, since the count is clamped) instead of 0. With a real disk this is a partition that held other data, or any block of an unlinked file that is allocated again. The tests used a device of zeros, so they could not see it.

**Cause**
**A new node was built on bytes the transaction had read from the device, and the device's copy of a free block is whatever the block's last owner left.** Two mistakes met. `Node::leaf()` and `Node::internal()` set the leaf flag and then zeroed `bytes[COUNT_OFFSET]`, but the count is a 16-bit little-endian field at offset 2, so byte 3, its high half, kept its old value. And the four callers in `insert()` (the first leaf of an empty volume, a leaf split, an internal split, a new root) passed `tx.cached(fresh_block)`, which reads the block from the device when the transaction does not hold it, so the old bytes were exactly what the node was built over. The comment on `Node` said "over the caller's zeroed block", and no caller zeroed it.

The effect depends on the old byte. A high count byte of 1 or more makes the count 256 or more, past both capacities (51 leaf entries, 169 internal), so the constructor clamps it to a full node of garbage: the first insert on an "empty" leaf sees a full leaf and splits it, and the tree grows nodes of invented items. The chain link (bytes 8 to 15) is not reset either, so the first leaf of a volume keeps a stale link (a leaf made by a split gets its link from the left half). `scan()` does not follow the chain, so nothing reads that link today. Nothing in the format has a checksum on a node, so nothing notices.

A second weakness sat beside it: `cached()` answers an empty span when the device read fails or the cache is full, and `Node::leaf(empty span)` writes `bytes[0]` of it.

**Change**
The header is now fully written by the factories, and a new node is built on a block that never came from the device.

```cpp
// before: Node::leaf()
bytes[LEAF_FLAG_OFFSET] = LEAF_FLAG;
bytes[COUNT_OFFSET] = 0;

// after
std::ranges::fill(bytes.first(SLOTS_OFFSET), u8{0});
bytes[LEAF_FLAG_OFFSET] = LEAF_FLAG;

// before: a creation site in insert()
auto right = detail::Node::leaf(tx.cached(right_block.unwrap()));

// after
auto const right_bytes = tx.fresh(right_block.unwrap());
if (right_bytes.is_err()) {
    return right_bytes.unwrap_err();
}
auto right = detail::Node::leaf(right_bytes.unwrap());
```

`Transaction::fresh(block)` makes a cache entry of zeros for a block the transaction has taken and does not read the device. It answers `InvArgs` for a block the transaction has not taken (the committed tree's, or a copy on the walk's path, which zeroing would erase) and `NoResource` when the cache is full, so the four sites no longer build a node over an empty span. Either half alone would have fixed the count: with only the header write the nodes were right but their slots held old bytes that reached the disk; with only `fresh()` a direct `Node::leaf()` over a dirty buffer stayed wrong. Both are kept, so the factory is correct over any bytes and the transaction never offers old ones.

Rejected: zeroing the block in `alloc_block()`. That function only chooses a block number and records it; the bytes belong to the cache, which is where `fresh()` lives.

**Reference**
Linux btrfs `btrfs_alloc_tree_block()` (`fs/btrfs/extent-tree.c`) initialises the new extent buffer's header in full and does not read it from disk. From memory, not opened.

**Verification**
- Built, booted and tested in a clean worktree (`.worktrees/verify-h2-btreefs`, HEAD `373f2480` plus these three files), because the main checkout's build is broken by another session's uncommitted edit of `system/public/ours/types.hpp`, which I did not touch.
- `python3 .agents/skills/verify/scripts/boot_check.py --no-build` on the final patch: dbg PASS, shell finished at 5.7s, 46 passed, 5 skipped, 0 failed; rel PASS, 44 passed, 8 skipped, 0 failed, 7/7 required; host-tests PASS, 161 passed. The six new gtest cases are in the log and pass: `BtreefsTree.BuildsOnBlocksThatHoldOldBytes`, `BtreefsTree.AFirstLeafOnOldBytesHoldsNothingBeyondItsItems`, `BtreefsNodes.ANewNodeStartsEmptyOverOldBytes`, `BtreefsTransactions.FreshBlocksAreZeroNotTheDevicesOldBytes`, `FreshRefusesABlockThisTransactionHasNotTaken` and `FreshAnswersNoResourceWhenTheCacheIsFull`.
- One earlier rel boot of the same patch failed with `routing_checks: Gpt.GetInfo = PeerClosed (expected Ok)`. Six rel boots in a row on the same build then passed, and the failing boot had no disk and never started btreefs (`btreefs: no partition table`). It is recorded as item 1 of `docs/repairs/261103/routing-checks.md`; I have not shown that it is unrelated beyond that.
- Fail-first: against the library as it was at HEAD (header and call sites unchanged), `BtreefsTree.BuildsOnBlocksThatHoldOldBytes` failed with height 4 instead of 3 and 10 051 items scanned instead of 10 000, and `BtreefsNodes.ANewNodeStartsEmptyOverOldBytes` failed with a count of 51 instead of 0. With `fresh()` changed to read the device instead of zeroing, `AFirstLeafOnOldBytesHoldsNothingBeyondItsItems` and `FreshBlocksAreZeroNotTheDevicesOldBytes` fail.
- `scripts/format_cpp.py --check` is clean on the three files. `clang-tidy` was not run: no `compile_commands.json` under `out/` covers the host test target.
- Three independent reviewers read the diff (completeness, mechanics, tests) before the commit. They found no defect in the change; their findings changed it in five ways: the factories now zero the whole header, `fresh()` refuses a block not taken, the scan test's comment no longer claims to test the chain, the new tests no longer sit between a comment and its test, and the cache-bound comment says `NoResource`. They also found the three repair items above.
- Not verified: with a real disk that holds old data (the dev image is zero-filled), and the `NoResource` propagation at the four sites (only `fresh()` itself is tested for it).

**Deliberately not done**
- The data blocks of a file (`write_file()` reads a new block before a partial write) and the padding bytes of a reused `Value` are the same class of mistake in the service, outside the tree; they are items 1 and 2 of `docs/repairs/261103/btreefs.md`.
- The fifteen places that build a `Node` from `cached()` without checking the span are item 3 of the same file.
- No node checksum. The count is clamped on read, so a corrupt node cannot drive the slot arithmetic out of the block, and a checksum is a format change.

**How it was found**
By the independent review in `.agents/tasks/review-last-three-days/REVIEW.md` (H2). Its reviewer ran a throw-away program outside the tree with free blocks filled with `l` and got a crash in `Node::upper_bound` on the first insert (count 27 648); a second checker re-derived it from the code.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why does a stale byte 3 turn an "empty" node into a full one of garbage, and why does the clamp make that worse rather than safe?
- Why does `cached()` return the device's bytes for a block the transaction has just allocated, and why is that right for a block the tree already owns?
- Why are both the header write and `fresh()` kept, when either one alone fixes the count?

## Title: a partial write to a new data block starts from zeros, not from the bytes the block last held (2026-11-03)

**Files**
- `system/service/btreefs/btreefs.cpp` — `write_file()` asks `ours_btreefs::data_block_to_patch()` for the starting bytes of a patch, and says whether the block is new
- `system/lib/ours.btreefs/include/ours_btreefs/ours_btreefs.hpp` — `data_block_to_patch()`, and the `<algorithm>` include
- `system/lib/ours.btreefs/test/ours_btreefs_test.cpp` — four tests in `BtreefsDataBlock`
- `docs/repairs/261103/btreefs.md` — the item for this change deleted; the two that remain are renumbered 1 and 2

**Symptom**
Found by reading, not run. One connection cannot reach it: a read clamps the seek position to the file's size, and a write leaves it at the end of what it wrote, so no connection reads past the end of a file it holds. Two connections can. Connection A holds a seek position of 5000 in a 5000-byte file. Connection B opens the file with `FileTruncate`, and the size drops to 0 while A's position stays 5000. A writes 5 bytes: block 1 has no item, so `write_file()` allocates a block for it, and since the write covers only part of the block it read the new block from the device and patched it. Bytes 0 to 903 of that block were whatever the block held before, the size became 5005, and `read_file()` returned those bytes as the file's content, though its comment says only a missing item reads as zeros. The dev disk image is zero-filled, so this appears only once a block has been freed and allocated again.

**Cause**
The same mistake as the node count in the entry above, on data blocks: a block just allocated was read from the device and used as if it were initialised, and the device's copy of a free block is whatever its last owner left, another file's content or the leftovers of an earlier volume.

There is a second way to get it wrong, which is why the fix is not just "skip the read". The service keeps one `m_block` buffer for the lifetime of the filesystem and reuses it for every read and write. A new block patched without reading would start from `m_block` as the previous operation left it, which is a different file's bytes again. The starting point has to be zeros on purpose.

**Change**
For a block that is new (the lookup answered NotFound), the patch starts from zeros and the device is not asked; for a block that has an item, it starts from what the block holds, as before.

```cpp
// before: write_file()
if (within != 0 || chunk != ours_btreefs::BLOCK_BYTES) {
    if (auto const status = device().read_block(ours_btreefs::decode_data_value(m_item),
                                                std::span{m_block});
        status != Status::Ok) {

// after
if (within != 0 || chunk != ours_btreefs::BLOCK_BYTES) {
    if (auto const status = ours_btreefs::data_block_to_patch(
            device(), ours_btreefs::decode_data_value(m_item), Status::NotFound == looked,
            std::span{m_block});
        status != Status::Ok) {
```

The decision sits in a library function, not inline in the service, because the service has no host test: it is a program that needs a block device. In the library it can be run against the fake device the tree tests use. A block written whole still skips the read, since every byte of it is overwritten.

**Reference**
Linux `__block_write_begin_int()` (`fs/buffer.c`) zeroes the parts of a newly allocated buffer that lie outside the written range (`zero_user_segments()` when `buffer_new()`). From memory, not opened.

**Verification**
- Built, booted and tested in a clean worktree (`.worktrees/verify-btreefs-data`, HEAD `ca778e00` plus these three files).
- `python3 .agents/skills/verify/scripts/boot_check.py`: build ok in 10.0 s; dbg PASS, shell finished at 5.5 s, 46 passed, 5 skipped, 0 failed; rel PASS, shell finished at 4.9 s, 44 passed, 8 skipped, 0 failed, 7/7 required; host-tests PASS, 163 passed. The four `BtreefsDataBlock` cases are in the log and pass.
- Fail-first: with `is_new` ignored (the library function always reading the device), `BtreefsDataBlock.ANewBlockStartsFromZerosNotTheDevicesOldBytes` fails (a byte of `0xA5` where `0` is expected) and so does `AFailedReadOfAnExistingBlockAnswersTheDevicesStatus`, whose new block, on a device that cannot read it, now answers OutOfRange instead of Ok.
- `scripts/format_cpp.py --check` is clean on the three files. `clang-tidy` was not run: no `compile_commands.json` under `out/` covers the host test target.
- Not verified: the two-connection case itself, which was not run, and the call in `write_file()` that passes `Status::NotFound == looked`. That one line is covered by reading only: there is no host test of the service, and the standard boot has no disk. A boot with `DISK=` whose free blocks are not zero, and a client that makes the two-connection write, would cover it.

**Deliberately not done**
- The padding bytes of a reused `Value` and the unchecked `cached()` spans stay out; they are items 1 and 2 of `docs/repairs/261103/btreefs.md`, which the entry above numbered 2 and 3.
- No test of the service itself, for the reason above.
- No change to the read side. `read_file()` already answers zeros for a block with no item.

**How it was found**
By the completeness reviewer of the change in the entry above, who read `write_file()` for places where a block just allocated is read and trusted; the two-connection case is theirs. Nothing was run to confirm it.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why can one connection never read the stale bytes of a new block, and what does a second connection change?
- Why is "skip the read for a new block" not enough, and what does `m_block` hold when the patch starts?
- Why does the decision live in a library function when it is one `if` at one call site?
