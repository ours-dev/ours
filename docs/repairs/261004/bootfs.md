# bootfs outstanding repairs

## 1. View::from() compares every pair of entries for the page rule, quadratic in the file count

**Severity**
Low: correct, and cheap at today's sizes (5 files in each BootFS item of the dev image); the cost grows with the square of the file count and is paid at every parse, by every reader.

**Symptom**
Not measured. For n entries, `View::from()` makes n(n-1)/2 pair comparisons, each after a 32-byte `memcpy` of the earlier entry: 10 for today's images, about 500 000 for a BootFS of 1000 files. The BootFS is parsed by phys's loaders, userboot, init (the root since step 13 of the component-framework plan) and the dbg_1 shell, each at least once per boot, and so would anything that maps the BootFS later; the standalone launcher that also parsed it is gone since step 16.

**Cause**
The page rule's second half (no two files on a common page, unless they are one file under two names) is a property of pairs. Finding a bad pair in one pass needs the entries in offset order, and neither holds here:
- the format does not order entries by offset (Zircon's orders them by name, `sdk/lib/zbi-format/include/lib/zbi-format/internal/bootfs.h:17-18`, and ours orders them not at all);
- View cannot sort a copy: it runs in phys and in freestanding user code, with no allocator, and `from()` takes no scratch space.

So `from()` compares each entry with every entry before it (`system/lib/bootfs/include/bootfs/view.hpp`, the loop over `earlier`).

**Change**
Measure first: a benchmark in `benches/` that parses Builder images of 10, 100, 1000 and 10 000 files and reports the time per parse, split into the linear checks and the pair loop. Then choose, with the user:
- (a) Make ascending data offset part of the format. The Builder already writes its entries in layout order. View then keeps the largest end seen so far among non-empty entries and the last non-empty entry: an entry that starts before that end is refused unless it is identical to that entry. O(n), no allocation. Cost: a rule Zircon's format does not have, and an image that orders entries by name, as Zircon's do, would be refused.
- (b) Let the caller pass a scratch buffer of n (offset, size) pairs, sort it, and compare neighbours. O(n log n), but `from()`'s signature grows and phys has to find the memory.
- (c) Keep the pair loop, if the numbers show it does not matter at the sizes this system will reach.

**Reference**
Zircon's `zbitl::BootfsView<>::iterator::Update()` (`src/lib/zbitl/include/lib/zbitl/items/bootfs.h:315-376`) checks each entry alone and checks no pair: it leaves "no partial sharing of ranges" to the format's promise. Fuchsia's `BootfsSvc::ingest_bootfs_vmo()` (`src/sys/component_manager/src/bootfs.rs:262-288`) finds entries on the same range with a `HashMap`, which needs the allocator View does not have.

**Acceptance**
- The benchmark's numbers are in the change record, before and after.
- `system/lib/bootfs/test:view` still passes, and gains a test for whatever the chosen option adds (an out-of-order image under (a)).
- `boot_check.py` passes both images.
