# loop outstanding repairs

## 1. Loop::add() finds a free slot by scanning the table from index 0

**Severity**
Low: a performance observation, not measured. Nothing is wrong, and today's tables are small.

**Symptom**
Latent, cannot be observed today. Each add() costs time in proportion to the number of slots in use below the first free one. The root will add a slot for every Open and every Clone (one vfs connection each), and D27 sizes its table at 3 slots per component plus one per connection, up to a few hundred, so an Open under load would walk most of the table before it is served, on the one thread everything waits behind (D16).

**Cause**
`Loop::add()` (`system/lib/loop/src/loop.cpp`) looks for the lowest slot whose handle is Invalid, starting at 0 each time. Taking the lowest free slot also reuses freed slots soonest, which is what makes the generation check in `run()` matter, but it is not needed for correctness: any free slot would do.

**Change**
Measure first, as CLAUDE.md asks of any performance work: a benchmark in `benches/` that fills a table of the root's size, frees slots in a scattered order, and times add() against the number in use, before deciding with the user. Candidates:
- a free list threaded through the free slots (an index in each free slot, the head in the Loop): O(1) add() and free, no extra memory, at the cost of reusing the most recently freed slot first;
- a bitmap of free slots scanned a word at a time with a count-trailing-zeros instruction: still a scan, 64 slots per step, and keeps "lowest free first".
Rejected: none yet; the choice waits for the numbers.

**Reference**
Fuchsia's async-loop has no table: the caller owns each `async_wait_t` and its address is the key (`sdk/lib/async-loop/loop.c:23-31`), so it allocates nothing. Zircon allocates handles from `HandleTableArena::Alloc()` (`zircon/kernel/object/handle.cc:103`), over `fbl::GPArena::Alloc()`, which pops a lock-free free list before it grows the arena (`zircon/kernel/lib/fbl/include/fbl/gparena.h:219-226`).

**Acceptance**
The benchmark's numbers are in the change record, the user has chosen, and `system/lib/loop/test` still passes, with a test that a slot freed and taken again gets a new generation whichever slot the new policy picks.
