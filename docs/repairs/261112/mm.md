# mm outstanding repairs

## 1. A frame descriptor is only valid if its storage was zeroed first

**Severity**
Medium. Correct today only because `SparseMemMap::activate_section()` asks for `Gaf::Zero`; a memmap allocated any other way would not boot.

**Symptom**
Latent, cannot be triggered today. With the `Gaf::Zero` taken off the memmap allocation at `kernel/mm/sparse_memmap.cpp:173` (tried on 2026-11-12, as the acceptance test of the order-field item): the dbg image panics in `PageFrame::init()` with "Duplicated to initialize a frame", and the rel image takes an invalid-opcode trap at an assertion's `ud2` in `FrameBuddy::release()` (`+0x3decc`) while the reserved frames are being stamped.

**Cause**
`PageFrame::init()` is not a constructor: it writes the zone type, section, node, role, order and flags, and leaves the rest of the descriptor as it found it. Four things still read that leftover storage.

- `init()` itself opens with `DEBUG_ASSERT(!(flags() & Flags::Initialized))`, which reads the Flags field before the function has written it. On zeroed storage the check means "not initialised twice"; on anything else it reads a random bit.
- The list hook `m_hook` is an intrusive hook, and an unlinked hook has a defined representation. The buddy links and unlinks through it.
- `m_references`, `m_compound` and the role body `m_body` are read by their owners on the assumption that a fresh frame has them at zero.
- `m_pins`, the pin count, added after this item was written. `prepare_alloc()` and `prepare_free()` assert it is zero, `pin()` refuses at 255, and `VmCowPages` files a page on its pinned queue only when a pin takes the count from 0 to 1, so a fresh frame with a non-zero count would be pinned without being filed.

`PageFrame() = default` is what lets a memmap be allocated as an array before `init()` runs, so the default constructor establishes nothing either. The invariant "a descriptor starts out empty" is therefore kept by the allocation call in another file.

**Change**
Make `init()` establish the whole descriptor: construct the hook, zero the reference word, the compound offset, the pin count and the body, and write every field of the status word, without reading any of them first. The double-initialisation check then needs a source that is not the field being initialised, or goes. Ruled out: keeping the dependence and documenting it on `activate_section()`; the virtual-flat memmap (`VflatMemMap`) is a second allocation site that would have to remember the same thing.

**Reference**
Linux `__init_single_page()` in `mm/mm_init.c`: `mm_zero_struct_page()` first, then every field it is responsible for, so that the caller's allocation does not need `__GFP_ZERO`.

**Acceptance**
Boot both images with the `Gaf::Zero` removed from the memmap allocation in `SparseMemMap::activate_section()` as a one-off experiment; both must pass `verify`.

## 2. CPU 0's provisional per-CPU unit is still rounded to a power of two

**Severity**
Low: boot memory, at most one unit's worth, held for the first few hundred milliseconds of boot.

**Symptom**
`CpuLocal::Mapper::replica_span()` rounds the provisional unit up to `2^k` pages, so a unit of `unit_size()` bytes can take nearly twice that from the boot allocator.

**Cause**
The rounding existed so that `CpuLocal::init()` could give the unit back with one `free_frames(replica, order)`. Since 2026-11-12 it gives it back with `adopt_frames()`, which takes any page range and cuts it into aligned blocks itself, so the rounding no longer buys anything. The comment on `replica_span()` says so.

**Change**
Size the provisional unit at `round_up(unit_size(), PAGE_SIZE)` and keep the alignment the physmap mapping wants. Measure first: the unit's size on the default and the four-node image, and whether the boot allocator's alignment for the span depends on the rounding (`init_early()` passes the span as both size and alignment).

**Reference**
None, this project's own design.

**Acceptance**
`verify` on both machines, and the boot log's reserved and free frame counts showing the unit's saving.

## 3. A waiting request confined to one node can wait for ever

**Severity**
Low: no caller asks for it today. `ALLOC_USER_VMO` names no node and may fall back to every other one.

**Symptom**
Latent, cannot be triggered today. A request with `Gaf::CanWait` and `Gaf::ThisNode` (or, later, one limited to memory below 4 GiB) on a node whose zones are all below Low is answered `ShouldWait`, and its waiter is woken only by a release that lifts one of those zones back to Low. If the node holds nothing reclaimable and nothing on it is freed, no such release comes: `AnonymousPageRequest::allocate()` logs every 5 s and waits on. The machine-wide refusal the memory watchdog switches on at OutOfMemory does not reach it while the machine as a whole has memory.

**Cause**
Waiting at a zone's Low (decision N1 of `.agents/tasks/pmm-redesign/DESIGN.md`, as revised on 2026-11-14) needs a way to end the wait when nothing can relieve the zone. The first draft of N1 had one: a node whose reclaim pass gave nothing back is marked stuck, and a request confined to it answers `OutOfMem`. It was left out of the revision because no caller can reach the case.

**Change**
Have the reclaimer record, per node, that a pass over its page queues freed nothing, clear it on the next release into the node, and make `alloc_block()` answer `OutOfMem` rather than `ShouldWait` for a request whose every candidate node is so marked. Ruled out: a timeout in the waiter, which turns a slow reclaim into a failure.

**Reference**
Linux `pgdat->kswapd_failures` and `pgdat_balanced()`: a node kswapd has failed on `MAX_RECLAIM_RETRIES` times is left alone until a page is freed into it.

**Acceptance**
A kernel test on a memory-only node: drain it below Low with nothing reclaimable, ask with `ALLOC_USER_VMO | Gaf::ThisNode`, and see `OutOfMem` after one reclaim pass instead of an endless wait.
