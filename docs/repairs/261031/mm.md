# mm outstanding repairs

## 1. create_contiguous zeroes and frees a whole block however little of it is kept

Since 2026-11-12 the code this item describes is `mm::alloc_frames_contiguous()` in `kernel/mm/pmm.cpp`, which `VmObjectPaged::create_contiguous()` calls. It was moved as it was: the whole block is still zeroed and the tail still goes back one frame at a time. The candidates below apply to that one function.

**Severity**
Low: a cost a program holding a BTI with Map can choose, not a wrong result. Measure before any change (the rule for performance items in CLAUDE.md).

**Symptom**
Latent, cannot be triggered by anything but a program that asks for it. `vmo_create_contiguous(bti, 4096, 22, &h)` is one page kept of a 4 MiB block: the allocator zeroes all 4 MiB (`Gaf::Zero` is applied to the block, `PmZone::alloc_frames()`), and `VmObjectPaged::create_contiguous()` then gives the 1023 tail frames back one `free_frames()` at a time, each taking the zone's spin lock with interrupts off and calling the free-memory signal check. A loop of such calls costs about a thousand times the zeroing and lock round trips of the one page it keeps, and free memory dips by the block's size for a moment each time, which can fire the free-memory watcher.

**Cause**
The tail is returned page by page because that is the pattern `fill_block_locked()` uses to give back a failed large block, and the block is zeroed whole because zeroing is a flag of the allocation. Neither was chosen for a block where most of the frames are given back. The ordinary case (a ring of a few pages on a page's alignment) loses at most the rounding up to a power of two, a few pages.

**Change**
Not chosen; to be decided with measurements. Candidates, and what each costs:
- allocate without `Gaf::Zero` and zero only the pages kept: saves the zeroing of the tail (up to 4 MiB), adds a zeroing loop here;
- give the tail back as aligned power-of-two chunks, at most two calls per order, not one per frame: cuts 1023 lock round trips to about 20, and needs the chunks' orders stamped as `FrameBuddy` expects;
- refuse an alignment more than some factor above the size: a limit on a feature nobody has asked to use.
Measurement: the cycles of `create_contiguous()` and the drop of the VMO, for sizes of 1, 4 and 64 pages at alignments of a page and of the block, on the dbg image's kernel shell with the TSC, each stage a CSV in `docs/perf/` (`scripts/perf_stats.py`).

The same call takes a user-class `Gaf`, which never passes the allocator's delay gate (`GATE.admit()`), so one 4 MiB request can take memory below the level kept for the kernel. `create_paged()` does the same with a page; the block is larger. Whether a block this size should wait at the gate is the same decision.

**Reference**
None, this project's own design.

**Acceptance**
A `docs/perf/` report with the baseline and each stage, and a smaller cost for the alignment-above-size case without a slower common case.

## 2. Nothing can ask for memory below 4 GiB

**Severity**
Low: no driver of this image needs it, and a virtio device is given 64-bit addresses.

**Symptom**
Latent, cannot be triggered today. `vmo_create_contiguous` and ordinary VMOs hand out frames from anywhere, so a device that can address only 32 bits may be given an address it cannot reach. The page allocator has no flag for a DMA zone (`Gaf` has no such bit).

**Cause**
The stub IOMMU translates nothing, so a device sees physical addresses, and no one has asked for a limit.

**Change**
A `Gaf` bit for the low zone, or an address limit on `vmo_create_contiguous`, when a device that needs it is added. The BTI is the place for the limit, since it is one device's identity.

**Reference**
None: Zircon's `pmm_alloc_contiguous()` (`kernel/vm/include/vm/pmm.h:83`) takes a page count and an alignment and has no address limit either.

**Acceptance**
A contiguous VMO made for a BTI with a 32-bit limit lies below 4 GiB, in a kernel test on a machine with memory above it (`MEM=3072`).
