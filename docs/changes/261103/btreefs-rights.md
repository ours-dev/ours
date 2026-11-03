# btreefs-rights changes

## Title: the unlink and truncate rights gates, and the checkpoint that unbounds the per-transaction remove loop (2026-11-03)

**Files**
- `system/service/btreefs/btreefs.cpp` — the UNLINK dispatch gates on `ModifyDirectory`; the open path's `FileTruncate` branch gates on `WriteBytes` in the open's own ask; the unlink loop checkpoints between removes.
- `system/lib/ours.btreefs/include/ours_btreefs/ours_btreefs.hpp` — `Transaction::checkpoint()` (write-through of the shadow cache, keeping the taken/dead intent lists); `truncate()` checkpoints per turn; the intent lists grow 64 → 512.
- `system/service/init/test/routing_checks/routing_checks.cpp` — the ro-unlink case (`fsio::unlink_at` on an r* handle answers AccessDenied, then the rw unlink of the same name succeeds — the discriminator), and the ro-truncate case (`refused("/disk/…", FileTruncate)` answers AccessDenied at the open).
- `system/lib/fsio/` — `fsio::unlink_at(dir, name)`, the handle form of unlink, whose message rides the caller's connection so its gate answers in the reply.

**Symptom**
Three High defects from the three-day review (docs/repairs/261103/btreefs.md items 1–3): a read-only directory connection could unlink files (the UNLINK dispatch checked decode and target, and no right at all); `open(r* | FileTruncate)` emptied a file from a handle holding only read (the flag is not a right, and nothing intersected it); and one unlink or truncate transaction held every remove's cache copies — ~16 removes filled the 48-entry shadow cache, so files over ~64-80 KiB answered `NoResource` forever and were undeletable.

**Cause**
The rights model gates every mutator — create checks `ModifyDirectory`, read/write check the bytes rights — but Unlink and the truncate flag were written before the views existed and never taught them. The cache exhaustion is a sizing mistake of the other kind: the header budgets the cache for ONE remove ("the path down, the split's siblings, the rebalance's"), while unlink and truncate run a bounded operation many times over one transaction — atomicity requires it — and `cow` appends a new cache entry per call even for blocks already this transaction's. Two bounds, one visible (NoResource forever), one latent (`cached()` returns an empty span on a full cache, and every node built over it would write its header out of bounds — unreachable only while `cow` fails first).

**Change**

```cpp
// the UNLINK dispatch, after decode and target
if (!holds(connection.rights, Operations::ModifyDirectory)) {
    answer_status(connection.channel, ours_io::UNLINK, request->txid, Status::AccessDenied);
    return loop::Action::Rearm;
}

// the O_TRUNC branch, before truncate_file
if (!holds(requested, Operations::WriteBytes)) {
    refuse(open.object, Status::AccessDenied);
    return;
}

// the loops, per turn
if (auto const status = tx.checkpoint(); status != Status::Ok) {
    return status;
}
```

`checkpoint()` writes every cached block to the device and empties the cache, keeping the taken/dead lists — which are the transaction's durable intent — so the loop never climbs. Safe under the crash discipline: the superblock has not swapped, so the written blocks are unreferenced garbage if the transaction dies — leak, never corrosion — and the bitmap bit is set only by commit. The truncate gate checks the **open's own ask** (`requested`), not the connection's ceiling: an rw* view holds WriteBytes whatever the client meant, so a ceiling check would have been a gate that never fires; the FileTruncate bit is not one of `Operations`, so `r* | FileTruncate` requests no Write right at all, exactly Zircon's flag-to-right table (`fuchsia.io`: `FILE_TRUNCATE` requires `WRITE_BYTES`).

**Reference**
Zircon's `Directory.Unlink` requires `Rights::MODIFY_DIRECTORY`, and `fuchsia.io`'s flag table makes `FILE_TRUNCATE` require `WRITE_BYTES` (`sdk/fidl/fuchsia.io/io.fidl`). The checkpoint is minfs's write-back of a transaction's blocks before its commit point, at user-space scale; the crash discipline it keeps is this library's own (leak, never corrosion — `docs/changes/261031/btreefs.md`).

**Verification**
Host: the two new big-file tests — a 64-block (256 KiB) file unlinked through the service's own loop shape with a checkpoint per turn, and the library's `truncate` over the same — pass, with the create loop checkpointed too (65 inserts in one transaction exceed the cache on their own; the service's real path writes one block per transaction, the bulk create plays them all). Target, disked: `routing_checks: passed` with the ro-unlink case answering AccessDenied and the rw unlink of the same name succeeding — the discriminator that proves the denial was rights, not absence — and the ro-truncate ask answering AccessDenied at the open. dbg PASS (46), rel PASS (44, 7/7), host 161. clang-format/tidy clean.

**Deliberately not done**
- The block-server completion fix (`docs/repairs/261103/block-server.md` item 1) — its own design decision, recorded there.
- The node-count clamp, the dirent `<=`, the header contract (btreefs.md items 4–6) — filed, each claimable.
- pc-ps2's AlreadyBound ritual — deliberately untouched (its own record).

**How it was found**
The three-day review's filesystem agent (the rights gates, the cache arithmetic) and the user's go-ahead to fix in severity order. The routing_checks truncate case taught its own lesson twice: the fire-and-forget dir-handle open does not surface a refusal (the epitaph rides the connection until first use), and the namespace open waits — the two forms answer the same gate differently, which is why the pin uses the namespace form.

**The user's prediction**
None. The user approved the fix order (the three Highs first).

**Three questions you should be able to answer**
- Why does the truncate gate check `requested` rather than `connection.rights` — what would an rw*-view client be able to do to a file if the gate read the ceiling?
- `checkpoint()` writes shadow blocks to the device before the superblock swap. Which property of the COW discipline makes that safe, and what would a crash mid-loop leave behind?
- The ro-unlink case asserts AccessDenied and then asserts the rw unlink of the same name succeeds. What does the second assertion rule out that the first alone cannot?
