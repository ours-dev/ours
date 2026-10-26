# mm outstanding repairs

Found by a scan of `kernel/mm/` for one pattern: a framework written whole with one step in the middle never landed, in a form the compiler cannot catch. Five instances of it had already been fixed by the time this file was written (the empty node-creation lambda, the dropped root section table, the ignored `gaf`, the skipped `PmZone` members, and the hole cursor in item 0 of `docs/changes/261026/mm.md`); what is below is what is left. Every item was confirmed by reading the code at the line given, not inferred.

## 1. `PmNode::alloc_frames()` reports failure as success, and its callers dereference the null

**Severity**
High. On an allocation path the kernel runs today, and it writes through a null pointer rather than faulting on a read.

**Symptom**
Out of memory turns into a write to address 0x10. `ustl::Result::operator bool()` is `is_ok()`, so `if (auto frame = this->alloc_frames(...))` is true for `ok(nullptr)`, and the body then does `list.push_back(*frame.unwrap())`, which stores two hook pointers through the null.

**Cause**
`kernel/mm/pm_node.cpp:275-276` ends with `alloc_frames_postamble(frame, gaf, order, context); return ustl::ok(frame);` and never tests `frame`. Both the fast path (`get_frame_from_zone_list`) and the slow path (`alloc_frames_slowpath`) return a plain `PmFrame *` that is null on failure, so the null reaches `ustl::ok()` unexamined. The two callers that trust the `Result` are `alloc_frames_fragment` at `kernel/mm/pm_node.cpp:292-293` and `alloc_frames_bulk` at `kernel/mm/pm_node.cpp:308-309`; both are written correctly against the contract the signature advertises, which is what makes this the callee's bug.

**Change**
Return `ustl::err(Status::OutOfMem)` when `frame` is null, before the postamble. Decide at the same time whether `alloc_frames_postamble()` should still be called on the failing path — today it only bumps the local hit/miss counters, and a miss arguably belongs in the OOM case too, so the test probably goes between the postamble and the return rather than before both.

**Reference**
Linux `__alloc_frozen_pages_noprof()` in `mm/page_alloc.c` returns `NULL` and every caller tests it; there is no success-wrapping layer to get wrong. The equivalent of the mistake here is specific to carrying a pointer inside a `Result`.

**Acceptance**
A kernel test that asks for an order the zones cannot satisfy and gets an `err` back rather than a fault. Until there is one, reading the function is the check: no path may reach `ustl::ok()` with a null.

## 2. `PmNode::alloc_frames_slowpath()` spins for ever when it cannot allocate

**Severity**
High. The first kernel-side allocation failure hangs the machine with no output.

**Symptom**
A hang with no panic and no log line, which `ours-verify` would report as `no "--- end ---" within Ns` or, once the detector fires, as a lockup report.

**Cause**
`kernel/mm/pm_node.cpp:161-189` is a `while (1)` over three sources. Two of them do not exist yet: `alloc_frames_direct_reclaim()` at `kernel/mm/pm_node.cpp:151-154` and `alloc_frames_direct_compact()` at `kernel/mm/pm_node.cpp:156-159` are both `return 0;`. The only way out other than success is `if (!can_direct_reclaim) break;`, and `can_direct_reclaim` is `!!(gaf & Gaf::DirectReclaim)`, which `GAF_KERNEL` sets. So for any kernel allocation the loop has no exit: it retries the same `get_frame_from_zone_list()` against an unchanged set of zones for ever. `GAF_USER` does not set `DirectReclaim`, so a user allocation breaks out and then hits item 1 instead.

**Change**
Bound the loop until reclaim and compaction are real: a retry count, or break as soon as both stubs are known to be stubs. The retry only makes sense once something in the loop can change the outcome, so the counter should come in with the first real reclaimer rather than being tuned now. `kernel/mm/include/ours/mm/gaf.hpp:23` already notes this.

**Reference**
Linux `__alloc_pages_slowpath()` in `mm/page_alloc.c` loops too, but every iteration can change the outcome (reclaim, compaction, the OOM killer) and it has `should_reclaim_retry()` plus a `no_progress_loops` counter as the exit. The loop shape is right; what is missing here is anything that makes progress and anything that counts the lack of it.

**Acceptance**
An allocation that cannot be satisfied returns instead of hanging, demonstrated by the same test item 1 needs.

## 3. `MemSection::memmap()` returns a span with the wrong length

**Severity**
High. Wrong today on every section, and the only reason it does not trap is that bounds checking is off in this build.

**Symptom**
Latent in this build, immediate in a hardened one. `pfn_to_frame()` indexes the returned span with `pfn & PFN_SECTION_MASK`, and that mask is 32767, so any pfn more than 512 frames into a section indexes past the span's stated end. `ustl::views::Span` is `std::span`, so turning on libc++ hardening makes `operator[]` trap on almost every lookup and the kernel does not boot.

**Cause**
`kernel/mm/include/ours/mm/priv/sparse_memmap.hpp:142` computes the length as `PAGE_SIZE / sizeof(FrameSpan *)`, which is 4096/8 = 512. The divisor is the size of a pointer to the span type, which is not the element size and has nothing to do with how many frames a section covers. `activate_section()` at `kernel/mm/sparse_memmap.cpp:171-172` actually allocates `FRAMES_PER_LEVEL[level]` descriptors, which at level 0 is `1 << (SECTION_SIZE_BITS - PAGE_SHIFT)` = 32768. The neighbouring `secmap()` at `kernel/mm/include/ours/mm/priv/sparse_memmap.hpp:152` divides by `sizeof(*this)`, which is correct, so this looks like a copy of that line with the type left wrong.

**Change**
The length is `FRAMES_PER_LEVEL[level()]`. Worth checking at the same time that a terminal section is always level 0, in which case the constant is enough and the lookup is not needed.

**Reference**
Linux's `__section_mem_map_addr()` in `include/linux/mmzone.h` returns a bare `struct page *` and carries no length at all, which is why it cannot get this wrong; the length lives in `PAGES_PER_SECTION`. Keeping a span is this project's own choice, and it is the right one — it just has to carry the real length.

**Acceptance**
A static assertion that the span length equals `FRAMES_PER_LEVEL[0]`, and a boot of both images with libc++ hardening enabled.

## 4. `INDEX_BITS` and `ENTRIES_PER_SECTION` disagree, so the section table is indexed past its end

**Severity**
Medium. Only reached on a machine with more physical memory than the current index range covers, but then it is an out-of-bounds table index.

**Symptom**
Latent below roughly 64 GiB of physical memory. Above it, `secnum_to_index()` returns a value up to 16 times the number of entries a section table actually has, and `section[index]` reads past the table.

**Cause**
`kernel/mm/include/ours/mm/priv/sparse_memmap.hpp:264` derives `INDEX_BITS` from the address width and the number of levels, giving 13 bits, so `secnum_to_index()` at `kernel/mm/include/ours/mm/priv/sparse_memmap.hpp:374` masks to 0..8191. But a section table is one page of `MemSection`, so `ENTRIES_PER_SECTION` at `kernel/mm/include/ours/mm/priv/sparse_memmap.hpp:230` is 512, which is 2^9. `LEAVES_PER_LEVEL` in the same header is built from 512, so the header holds two inconsistent notions of how wide a level is. The three indexing sites are `kernel/mm/include/ours/mm/priv/sparse_memmap.hpp:110`, `:139` and `:384`.

**Change**
`INDEX_BITS` should be derived from the table it indexes: `ustl::bit_width(ENTRIES_PER_SECTION - 1)`, which is 9. Then check whether `MAPPING_LEVEL` levels of 9 bits still cover the architecture's physical address width, and raise the level count if not — that is the trade-off this constant was presumably trying to express, and it should be expressed as an assertion over the three numbers rather than a division.

**Reference**
Linux's `SECTIONS_PER_ROOT` in `mm/sparse.c` is `PAGE_SIZE / sizeof(struct mem_section)` and `SECTION_NR_TO_ROOT()` divides by it, so the fan-out and the index are the same number by construction. The fix is to do the same here.

**Acceptance**
A static assertion that `MAPPING_LEVEL * INDEX_BITS` covers `SecNum`'s range for the configured physical address width, and that `BIT(INDEX_BITS) == ENTRIES_PER_SECTION`.

## 5. `PageFrame::init()` never writes the Order field

**Severity**
Medium. Correct today only because another allocation site happens to zero the memory.

**Symptom**
Latent. `init()` leaves `Index::Order` holding whatever the descriptor's memory held before, and `FrameBuddy::is_mergeable()` reads `buddy->order()` to decide whether two blocks may be merged. Today `activate_section()` asks for `Gaf::Zero`, so the field reads 0 and the decision is accidentally right.

**Cause**
`kernel/mm/include/ours/mm/pm_frame.hpp:257-267` writes `Index::ZoneType`, `Index::Section`, `Index::Node`, `Index::Role` and `Index::Flags`, and stops. The comment on the last write says "The one write of the whole field: whatever the descriptor held before is garbage", which is true of the Flags field and reads as if it covered the whole word — it does not. So the descriptor's own initialiser leaves one of the fields the buddy allocator depends on unset, and the invariant is propped up from `kernel/mm/sparse_memmap.cpp` instead. Note that the `Gaf::Zero` on that allocation was itself silently dropped until recently, which is exactly how this would have become live.

**Change**
Add `m_status.set<Index::Order>(0)` to `init()`, and correct the comment to say which field it is talking about. An `init()` that sets every field it names is also the precondition for ever relaxing the `Gaf::Zero` on the memmap allocation.

**Reference**
Linux `__init_single_page()` in `mm/mm_init.c` sets every field of `struct page` it is responsible for and does not rely on the allocator having zeroed it; the order of a free block lives separately in `page_private()`, set by `set_buddy_order()` at the moment a block is freed into the buddy.

**Acceptance**
Read `init()` and confirm every field named in `Index` is either written or documented as not belonging to it. Then boot both images with the `Gaf::Zero` removed from `activate_section()` as a one-off experiment, which must still pass.

## 6. `FrameBuddy::is_mergeable()` trusts `role` without checking that the descriptor was ever initialised

**Severity**
Medium. Depends on nothing else being wrong, but it is the one place that could have caught every upstream mistake of this kind and does not.

**Symptom**
An uninitialised or reserved descriptor that reaches the buddy is accepted as a free buddy, and the merge then manipulates an intrusive list node that is not linked where the list believes. The failure is `__builtin_trap()` inside `ustl::collections::intrusive::detail::require()` — a bare `ud2` with no message, in `FrameBuddy::release()`, several frames away from whatever produced the bad descriptor.

**Cause**
`kernel/mm/include/ours/mm/priv/frame_buddy.hpp` checks `role() == Role::Pmm`, `order() == order`, and equal `zone()` and `nid()`. `role` is written in exactly one place, `insert_frame()`, so "role is Pmm" does mean "this descriptor is in a free list" — but that invariant has no assertion guarding it, and an all-zero descriptor reaches the check with every field readable and no way to tell it apart from a real one. `Flags::Initialized` exists precisely to distinguish them and is not consulted; neither is `Flags::Reserved`, which marks a frame nothing may hand out.

**Change**
Add `Flags::Initialized` set and `Flags::Reserved` clear to the checks. Both are reads of a field already loaded, so this costs nothing on the merge path. Consider promoting the span check in `FrameBuddy::release()` from `DEBUG_ASSERT` to `ASSERT` at the same time: it is the guard written for exactly this failure and it is compiled out of the build the images are verified with, which is the build where the trap was seen.

**Reference**
Linux `page_is_buddy()` in `mm/page_alloc.c` checks `PageBuddy(buddy)` — a flag set only by `set_page_buddy()` — rather than inferring membership, and `__free_one_page()` additionally runs `VM_BUG_ON_PAGE(bad_range(zone, page), page)`. The shape to copy is "a flag that only the insert path sets", which `role` already is; what is missing is the second flag that says the descriptor is real at all.

**Acceptance**
A kernel test that frees a deliberately uninitialised descriptor and gets a named assertion failure rather than a `ud2` from inside ustl.

## 7. `gaf_to_allocation_control()` ignores its argument, so every allocation option is dead

**Severity**
Medium. Silently wrong on every allocation, but what it disables are optimisations and refusals rather than correctness.

**Symptom**
`AllocationOptions::NoBlock`, `NoFragment`, `Contiguous` and `Kswapd` are never set by anyone. The branch in `PmZone::alloc_frames()` that tests `options & NoBlock` can therefore never be taken, and neither can the watermark branch beside it (see item 8), so a caller that asked not to block will block.

**Cause**
`kernel/mm/pm_node.cpp:26-28` is `auto gaf_to_allocation_control(Gaf gaf) -> AllocationOptions { return AllocationOptions(); }`. The parameter is named and never read. Its result is passed to the zone list walk and on into `PmZone::alloc_frames()`, so the whole options mechanism downstream is driven by a default-constructed value.

**Change**
Map the `Gaf` bits onto `AllocationOptions`. `Gaf::NoRetry` to `NoBlock` is the one with a caller today — `GAF_GROW` in `kernel/mm/cpu_local.cpp` sets it precisely so that growing the per-CPU area under a spin lock cannot block — so it is wrong in a way that matters now, not only in theory.

**Reference**
Linux `gfp_to_alloc_flags()` in `mm/page_alloc.c`, which is this function's direct counterpart and is where to look for which bit maps to what.

**Acceptance**
A kernel test that passes `Gaf::NoRetry` into a zone that would have to block and gets a failure back instead.

## 8. `PmZone::Flags::BelowHighWmark` has no writer, so the watermark path is dead

**Severity**
Medium. The whole watermark mechanism is inert, and nothing says so at the point that reads it.

**Symptom**
`PmZone::alloc_frames()` opens with `if (is_below_high_wmark() && !(options & NoBlock))`, and both halves are permanently false — the first because nothing sets the flag, the second because of item 7. So a zone under pressure never tells the caller to move on and let reclaim catch up.

**Cause**
`kernel/mm/include/ours/mm/pm_zone.hpp:213` reads `m_flags & Flags::BelowHighWmark`; the flag is declared at `:222` and set nowhere in the tree. Related state in the same class is in the same condition: `m_watermarks` is neither read nor written and is also absent from the constructor's initialiser list, so on memory from `EarlyMem::allocate<PmZone>()`, which is not zeroed, it holds three indeterminate values. `m_managed_frames` is only ever incremented by `free_frame()` and decremented by `finish_allocation()`, starting from zero, so its actual meaning is "frames currently free" and not the `present - reserved` its own comment claims.

**Cause (dependency)**
Item 7 must be fixed too for the condition to become reachable; either alone leaves the branch dead.

**Change**
Decide first whether watermarks are in scope now. If they are: initialise `m_watermarks` in the constructor, compute them from `present_frames` when the zone is built, and set and clear the flag where `m_managed_frames` moves. If they are not: delete the flag, the accessor, `m_watermarks` and the `WaterMark` enum, and leave a comment at `PmZone::alloc_frames()` saying there is no pressure signal yet — the present state, where the code reads as if there were one, is the worst of the three.

**Reference**
Linux `zone_watermark_ok()` in `mm/page_alloc.c`, with the thresholds held in `zone->_watermark[]` and set by `__setup_per_zone_wmarks()` in `mm/mm_init.c`.

**Acceptance**
Either a test that drives a zone below its high watermark and observes the caller moving to the next zone, or the deletion, with `PmZone`'s constructor initialising every member it declares.

## 9. `PageQueues::start_runners()` is empty and has no callers, so page aging never runs

**Severity**
Medium. A whole subsystem that exists and has never executed; harmless until something depends on reclaim.

**Symptom**
`m_mru_runner` and `m_lru_runner` stay empty `Arc`s for the life of the kernel. `rotate_reclaimable_queues()` has exactly one caller, `LruRunner::run()`, which itself has none, so no page is ever aged out of a queue.

**Cause**
`kernel/mm/page_queues.cpp:125-139` has a body of comments only, and nothing in the tree calls it. `MruRunner` is an empty class holding one `[[maybe_unused]]` thread handle. `m_max_rotate_time` is assigned in the constructor and never read; `m_last_age_reason` is written by `rotate_reclaimable_queues()` and never read, and the `to_string(AgeReason)` declared as its friend has no definition.

**Change**
Either start the runners from the mm init sequence, or mark the whole queue-rotation half of `PageQueues` as not yet wired and say where it will be started from. The dead fields should go with whichever way it is decided, since each of them is a reader or writer with no counterpart.

**Reference**
Linux `kswapd_run()` in `mm/vmscan.c`, called from `kswapd_init()` as a `module_init`, with the per-node thread in `pgdat->kswapd`.

**Acceptance**
A debug line from the rotation path appearing in a dbg boot log, or the dead members and the empty function removed.

## 10. `dump-frame` is registered as a kernel shell command and does nothing

**Severity**
Low. A debugging aid that silently is not one, at a time when descriptor state is what needs inspecting.

**Symptom**
Typing `dump-frame` in the dbg image's kernel shell prints nothing and returns success.

**Cause**
`kernel/mm/pm_frame.cpp:51-53` is `static auto cmd_dump_frame(int argc, char **argv) -> int { return 0; }`, and `GKTL_COMMAND(FrameDumper, "dump-frame", cmd_dump_frame, "")` on the next line registers it. `PageFrame::dump()` immediately above it is written and works; the command just never calls it, and it has no help text either.

**Change**
Parse a pfn or a physical address from `argv`, resolve it with `pfn_to_frame()`, refuse politely when `pfn_valid()` is false, and call `dump()`. Give the command a help string. This is worth doing before the next descriptor-state bug rather than after.

**Reference**
None, this project's own design.

**Acceptance**
`python3 scripts/kshell_run.py --out out/<dir> "dump-frame 0x1000"` prints the descriptor, and a bad argument prints a message rather than nothing.
