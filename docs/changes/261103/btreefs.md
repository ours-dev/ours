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
