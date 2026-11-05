# review-last-three-days: review of the commits of 2026-10-30 to 2026-11-02

**Date**: 2026-11-02

## Scope and method

What was reviewed: the 148 non-merge commits from `59a8405d` (exclusive) to `44878457` (the `baseline/20260824` tip when this review started), 508 files, +51,316 and -3,746 lines. Documents, generated lists and the 19 merge commits were read only where a finding needed them. Eleven irq commits from another session (`02d20ed6` to `891797e9`) landed while the review ran and are not reviewed; where a finding cites a line in `kernel/kernel/irq/`, `kernel/include/ours/irq/` or `kernel/object/interrupt_dispatcher.cpp`, the verifiers read the newer tree and the line numbers follow it. Everything else in scope was unchanged by those eleven commits except `kernel/arch/x86/kernel/time/{hpet,time}.cpp`.

Method: six reviewers, one per area, each following `cpp-code-review` with `modern-cpp-development` and `ours-kernel-design`, and each reading the earlier task reviews first. The areas were the interrupt-line layer, memory management and DMA objects, clocks and the vDSO, the file-system stack, drivers and services, and the public headers with the build tooling. Every bug-class finding was then handed to independent skeptics whose job was to refute it against the committed code: three lenses (code path, outside trigger, intent and prior records) for the findings that carry the most weight, one for the rest.

Verification labels used below. **Probe** means the area reviewer ran a throw-away program or script outside the tree (ASan, compile, generator run). **3/3** or **1/1** means that many independent skeptics re-derived the claim from the code and none refuted it. **Read-only** means judgement from reading, not re-derived. All 35 findings submitted to the skeptics survived; one skeptic of three refuted the trigger of M6 (see there). That does not prove nothing else is wrong: the single-lens findings had one skeptic each, and the skeptics agreed with the reviewers every time.

What was not done: nothing was built or booted, so no finding is confirmed at run time. `clang-tidy` could not run because every `compile_commands.json` under `out/` is stale (it lacks `ours/vdso_decls/bti.inc`), except for two time files that came out clean. `clang-format` was skipped because the pre-commit hook enforces it on changed lines. Lockdep annotations could not be checked (lockdep does not link when switched on). The only existing tests that were run were the 165 in `system/test_component_manifest.py` (all pass).

## What is done well

- The syscall header split lost nothing. The 62 description entries before and after `0b02c363` are byte-identical apart from the added `category`; from the base to the tip, 53 calls became 66 with none removed; no enum, struct, constant or `static_assert` disappeared (85 became 94 asserts); the record layouts of every `ours::` struct are identical; every public header compiles on its own in user and kernel include modes (Probe, tooling reviewer).
- The B+tree core holds its invariants. A randomised run against a `std::map` oracle (seeds 1 to 4, up to 30,000 operations, heights 1 to 3, full drain) checked key order, separator bracketing, uniform depth, minimum fill, root collapse, scan order and that the bitmap equals the reachable nodes plus metadata: no errors, no leaks (Probe, file-system reviewer). Slot parity and torn-slot recovery by CRC are sound.
- The pin and DMA object lifetimes are sound: a pinned page cannot move or be freed (`vm_cow_pages.cpp:541`, `reclaim_page` rechecks the kind under the owner's lock); `m_pin` is exchanged under the PMT lock so there is no double unpin (`pmt_dispatcher.hpp`); `bti_pin` checks rights per permission bit, caps the count before pinning and cannot overflow on `offset > m_size - size` (`kernel/lib/syscall/src/bti.cpp:117-163`); `vmo_create_physical` cannot overflow its range check and refuses RAM.
- The grace-period argument for `IrqRcu` holds on x86: the retract reaches each CPU through its mailbox lock, each answers with a release, the remover reads `outstanding` with acquire, the guard keeps preemption off, and CPU offlining is serialised by the hotplug lock (`kernel/kernel/task/irq_rcu.cpp:36-58`, `kernel/include/ours/task/irq_rcu.hpp:94-145`).
- ACPI table export is bounds-checked end to end: lengths are read once, 32-bit overflow is guarded, the export VMO is read-only so there is no time-of-check gap (`kernel/lib/acpi/src/table_export.cpp`).
- The component compiler is deterministic and fails clearly: `_order_children` runs after `_check_cycles`, duplicate children, missing targets and cycles give a `ManifestError`, and `VERSION=3` offsets match `format.hpp`; `gen_syscalls.py` output is byte-identical across runs (Probe, tooling reviewer).
- HEAD has exactly one `clock_get_monotonic` after the merge, revert and re-apply churn, and no conflict markers remain anywhere in `kernel`, `system` or `build`.

## High severity

### H1. A pin followed by an unpin turns a written pager page back into a clean, reclaimable page

`kernel/mm/vm_cow_pages.cpp:1085` and `:1117`, with `:52-53`, `:61-68`, `:79-99`, `:1261-1263`. Re-derived 3/3.

The "this page was written" fact for a pager-backed VMO lives only in the page's queue kind: Anonymous means written, Reclaimable means clean. `pin_range_locked` overwrites the kind with Pinned (`:1085`). The last unpin refiles the page by node type through `file_page_locked(Fill::Content)`, and `:52-53` sends any node with a page source to Reclaimable, so everything written before or during the pin is forgotten. The page table entry stays writable, because the write fault returned `.writable = true` (`:1263`) and `file_dirty_locked` (`:61-68`) leaves a Pinned page alone.

Timeline: `pager_create_vmo`, supply page 0, `vma_map` it RW, `bti_pin` it with write permission. The device DMAs into the page, or the program stores to it. `pmt_unpin` files the page Reclaimable. Later stores do not fault. Under memory pressure `reclaim_page` evicts it as clean (it does not look at the hardware dirty bit), and the next read fetches the pager's old content. The device's data and the stores are gone.

Reachability: nothing in `BtiDispatcher::pin` (`bti_dispatcher.cpp:124`) or `VmObjectPaged::pin_range` (`vm_object_paged.cpp:418`) refuses a page-source VMO, and `with_source` (`:126`) rejects only the `Pinned` flag. The caller needs a BTI, which needs the system IOMMU resource, so this is a driver-level bug, not an unprivileged one. The comment at `:1196-1203` states the invariant that this breaks.

Fix: on the last unpin of a page-source node, file the page Anonymous (dirty), or keep a clean/dirty bit separate from the queue kind and refile from it. Also correct acceptance item 3 of `docs/repairs/261030/mm-pin.md` ("reclaimable again after the unpin"), which as written would bless this behaviour, and add the missing test (see Minor). The reviewer compared Zircon's `VmCowPages::MoveToNotPinnedLocked`, which returns only Clean pages to the reclaim queue and sends Dirty ones to the dirty queue.

### H2. A new B+tree node starts with a garbage count

`system/lib/ours.btreefs/src/ours_btreefs.cpp:126-136`, callers at `ours_btreefs.hpp:899`, `:947`, `:976`, `:991`. Probe plus re-derived 1/1.

`Node::leaf()` and `Node::internal()` zero `bytes[COUNT_OFFSET]` only, but the count is a little-endian 16-bit field at offset 2, so byte 3 keeps whatever was there. Every caller passes `tx.cached(fresh_block)`, and `cached()` (`:419-427`) reads the free block from the device, so nothing zeroes it. The header comment says "over the caller's zeroed block"; no caller zeroes it.

Trigger: the first insert or split on a volume whose free blocks hold old bytes, for example a re-formatted partition or the blocks of an unlinked file. The reviewer's ASan probe filled free blocks with `l` and the first insert crashed in `Node::upper_bound` (count 27648). The tests use a zeroed `FakeDevice`, so they cannot see it. The skeptic found the worst case at `:991`: a new root takes `insert_child`, which moves `m_count * 32` bytes, up to about 2 MB, and a freed file-data block can hold client-chosen bytes, so the count is partly client-controlled.

Fix: give the transaction a `make(block)` that hands out a zeroed cache entry without reading the device (the btrfs counterpart, `btrfs_alloc_tree_block`, initialises the whole header), and zero the entire node header in `leaf()` and `internal()`. This also removes the `cached()` read-error path where `Node{empty span}` would write out of bounds.

### H3. One failed superblock read makes `mount()` format the volume

`system/lib/ours.btreefs/include/ours_btreefs/ours_btreefs.hpp:733-757` (`open`), `system/service/btreefs/btreefs.cpp:1090` (`mount`). Re-derived 1/1.

When a superblock slot read fails, `open()` returns `best`, which starts as NotFound. `mount()` reads NotFound as "blank partition" and calls `format()`. A single transient I/O error at mount time wipes a good volume.

Fix: return the I/O status from `open()` and distinguish "no valid superblock in either slot" from "could not read"; try slot 1 before giving up.

### H4. Rights are not enforced on truncate and unlink, and memfs reuses a freed row under live connections

`system/service/btreefs/btreefs.cpp:751` (truncate) and `:980-1005` (unlink); `system/service/memfs/memfs.cpp:529-545`. Re-derived 3/3.

`FileTruncate` is an open flag, not a right, so `rights_of(flags)` adds nothing for it. `open_below_dir` (`btreefs.cpp:751`) truncates whenever the flag is set, with no `PermWriteBytes` test. `ioservice` answers InvArgs for the same call (`system/lib/ioservice/src/server.cpp:365`, and `btreefs.cpp:687` says "the checks are ioservice's"), but for a mount point `ioservice` forwards the flags unchanged, so nothing upstream catches it. A client holding a read-only directory capability (`R_STAR_DIR` has neither `WriteBytes` nor `ModifyDirectory`) can open any file with `FileTruncate` and empty it. Both `Unlink` handlers check no `ModifyDirectory` right although `fsio::unlink` asks for it.

Separately, memfs `files[found] = {}` (`memfs.cpp:~542`) frees the row while connections still index it. A later create reuses the row, so an old connection reads and writes another file. btreefs is safe because object ids never repeat. The reviewer also found that both services' `handle()` drop `request_handles` on every decode error (see Minor).

Fix: enforce the rights in the services (or move the check to a layer the forward path also passes through), and give memfs rows a generation or refuse to free a row with open connections.

### H5. The skills the repository tells every task to use do not resolve in Claude Code

`.claude/skills/ours-verify` and `.claude/skills/ours-kernel-design`. Verified by me with `ls -l`, `test -e` and `git ls-tree HEAD`.

Both are tracked symlinks into `.agents/skills/ours-*`, which commit `60026403` renamed to `verify` and `kernel-design`. The commit added no `.claude/skills/{verify,kernel-design,merge}` links, so the two old links dangle and the three skills CLAUDE.md names as needed by nearly every task (`verify`, `kernel-design`, `merge`) are invisible to Claude Code: this session's own skill list shows none of them. The eval suite ran in "ZCode mode" (`.agents/evals` README, 2026-11-03), which cannot see this.

Fix: relink with `ln -s ../../.agents/skills/<name> .claude/skills/<name>` for `verify`, `kernel-design` and `merge`, and delete the two dangling links. Living references to the old names remain in `.agents/skills/language/cpp-code-review/SKILL.md` (lines 13, 58, 93, 109, 126), `scripts/lldb_mcp.py:22` (a dead path), `docs/design/261004/component-framework.md` (lines 19 and 391) and prose in several scripts; `docs/guide/05-testing-and-debugging.md:244` cites `boot_check.py:560` and `:588`, which are now `:563` and `:591` (read-only, tooling reviewer).

## Medium severity

### M1. Truncating or unlinking a file of more than about 32 blocks fails with NoResource

`ours_btreefs.hpp:445` (`cow`), `:1107` (`remove_from`), `:1142` (`truncate`); `btreefs.cpp:814-852` (`unlink_at`), `:874` (`truncate_file`). Probe plus re-derived 1/1.

`cow()` never reuses a block that this transaction already copied: every `remove` re-copies the whole root-to-leaf path, costing one taken bit, one dead bit and one cache entry per level, and the old copy is not dropped. Truncate and unlink loop over all of a file's blocks inside one transaction with limits of 64, 64 and 48. The reviewer's probe: truncate fails from 33 blocks (132 KiB) at height 1 and from 22 blocks (88 KiB) at height 2; the skeptic checked the arithmetic (dead bits grow by 2 per block at height 1, by 3 at height 2). The test truncates only 6 blocks. No corruption: the transaction is abandoned. But a large file can be neither emptied nor deleted.

Fix: modify in place when the block is already in `m_taken` (btrfs `should_cow_block()` tests generation against the transaction), or commit in batches.

### M2. A write over an existing data block goes to the committed block, before any commit

`btreefs.cpp:558-570` (`write_file`). Re-derived 3/3.

For an existing DATA item, `write_file` reads the committed block, patches it and writes it back through the raw `BlockAdapter` (`:145`), bypassing the transaction. The superblock swap comes later at `:588`. A crash or error after the first block of a two-block overwrite leaves the old tree naming a block that holds new bytes, and a torn write. This contradicts the intent recorded in `btreefs-truncate.md` ("whole or empty, never half") and the library header's "nothing is ever modified where it lies"; `btreefs-cow.md` is silent on file data, so it is not a recorded decision.

Fix: copy-on-write the data block as well, or record the limit in `btreefs-cow.md` as a decision for the user to take.

### M3. `commit()` reports failure after the operation took effect, and its documentation describes the old order

`ours_btreefs.hpp:556-591` and the class comment at `:53-59`. Re-derived 1/1.

`commit()` returns the dead-bit-clear error after `m_committed = m_shadow` (the swap), so the caller sees a failure for an operation that is already durable and the remaining dead bits are skipped; the record's own question 3 says this must not fail the commit. The class comment and the record still describe the old order, and the record's claim that "a crash before the superblock costs zero" is false: taken bits are written first, so a crash leaks up to 64 blocks and there is no reclaim. There is also no flush: `ours.block` has no Flush call and virtio-blk accepts no features (`virtio_blk.cpp:651`), so the commit order is program order, which only works on a write-through device. The docs should say so.

### M4. `block_server` drops the Close that `close_client()` returns, leaving a live Loop slot on a freed entry

`system/lib/block-server/include/block_server/block_server.hpp:164` and `:172`; contract at `system/lib/server/include/server/server.hpp:65-73` and `:147-156`. Re-derived 3/3.

`complete()` calls `Base::close_client(...)` as `auto _ = ...` and discards the returned `Close`. The entry is freed, but the Loop only closes a slot when a waiter returns `Close` (`loop.cpp:106-135`), so the slot stays armed on the channel. `server.hpp:65-73` states that an entry may be retaken only after the Loop has freed the slot it released; this path breaks that.

Timeline with the shipped driver, whose `start()` calls `complete()` inline (`virtio_blk.cpp:479`): client A sends a Write and dies; the synchronous reply fails with PeerClosed and entry e0 is freed; client B's Open was queued ahead of A's PeerClosed packet, so `connect()` hands e0 to B and adds a second slot for the same waiter; A's PeerClosed then runs `release(e0)` and wipes B's channel; B's next message gets BadHandle and its connection closes without a reply. The other branch at `:164` ("driver bug") leaves the client blocked forever with the channel open. No test reaches either branch.

Fix: mark the entry closing and let the next `on_signals` return `Close`, or give the Loop a close that a callback can call. Mark `close_client` `[[nodiscard]]` so the next caller cannot do this silently.

### M5. virtio-blk exits without stopping the device, so a clean exit unpins memory the device still owns

`system/driver/virtio-blk/virtio_blk.cpp:50-52` against `:641` (the only `set_status(0)`), `:684` and `:702-719`. Re-derived 3/3.

The header promises "the device is stopped and observed stopped" before the driver leaves. The only reset is at bring-up. Every `return 1` after `set_queue_pfn` and DRIVER_OK (`add_protocol` failure, `serve` failure, the exit after `loop.run()`) runs `~DmaRegion`, which unmaps and then calls `pmt_unpin` while the device still holds the queue address. A crash is safer than a clean exit here: closing the PMT token without an unpin makes the kernel quarantine the pages (the case `check_quarantine` models), whereas an explicit unpin releases them. This is a regression from `0751eb22`, which had a `cleanup()` that reset the status.

Fix: a scope guard declared after the region that writes status 0 and polls until it reads 0, or have `DmaRegion` abandon rather than unpin unless the caller says the device is quiesced.

### M6. After any non-completion exit from `submit()`, every later request is answered by the previous request's completion

`virtio_blk.cpp:331-411`, cursors at `:383` and `:404`. Re-derived 2/3 (one skeptic refuted the trigger, not the consequence).

If `interrupt_wait` returns an error (`:398-401`) or the loop gives up after 100 wakeups (`:410`), the device still owns descriptors 0 to 2, the header and the data area, and `m_offered` is one ahead of `m_seen`. The next `submit` rewrites the same bytes; the old chain's late completion makes `used.idx == m_seen + 1` true at once, and the request returns Ok with stale data, permanently until reboot. The status byte is never poisoned before submit and the used `id` and `len` are never checked, so a device that skips the status write reads as success.

Trigger: the refuting skeptic is right that no software caller can reach this. A client of `ours.block` controls only `lba` and `count`, out-of-range requests still get a used entry with an error status, and the interrupt handle is private to the driver. What remains is a hung device, or a foreign device storming the hard-coded shared line (GSI 23, `:119`). Severity is therefore medium for a driver whose job is to keep a disk honest, not high.

Fix: on any non-completion, reset the device and fail all later requests; poison the status byte to 0xFF before each submit; check the used `len`.

### M7. virtio-blk chooses the queue depth, but the legacy Queue Size register is read-only

`virtio_blk.cpp:126`, `:654`, `:663`. Re-derived 1/1.

The driver writes `QUEUE_NUM` and reads `queue_max` only to compare it with 0. The device lays out the used ring from its own queue size, so with a size other than 256 (QEMU `queue-size=128`) the device expects the used ring at page 1 while the driver puts the avail ring there. It works today only because QEMU's default is 256.

Fix: lay the rings out from the register's value, or refuse to run when it is not 256.

### M8. `ddk::MappedMmio` never unmaps, and the ddk move assignments double-release

`system/lib/ddk/include/ddk/ddk.hpp:190-195` and `:219-224` (MappedMmio), `:207`, `:261`, `:338` (move assignment). Re-derived 1/1 each.

`MappedMmio::map()` never assigns `mapped.m_vma`, so `~MappedMmio` never unmaps. Commit `69e0cddb` says it fixed this but its diff adds the assignment only to `DmaRegion` (`:308`). The effect: virtio-blk's ECAM window (`virtio_blk.cpp:557-564`, `:591-596`, comment "the window goes") stays mapped read-write for the driver's life, and it covers the whole MCFG range, up to 256 MiB, which is the configuration space of every PCI function. Enumeration only reads. Separately, `operator=(X&&) = default` on `MappedMmio`, `Bti` and `DmaRegion` copies the handle fields without clearing the source or releasing the target, so `a = std::move(b)` leaks `a` and then both destructors release the same handles; it is not triggered today because only move construction is used. `ddk_test.cpp` covers only `Resources` and `startup_handle`, so neither defect can be seen by a test.

Fix: assign `m_vma` after a successful map and give `map()` a read-only option; use move-and-swap or delete the move assignments; add a fake `vma_map`/`vma_unmap` so the test covers `MappedMmio`, `Bti`, `DmaRegion` and `irq::take`.

### M9. The TSC clock source can never be selected, and fixing that would expose a wrong vDSO read

Re-derived 1/1 for the first two, 3/3 for the third.

Three latent bugs, each masked by the previous one, so they must be fixed together and in this order.

1. `kernel/arch/x86/lib/arch/include/arch/x86/processor/feature.hpp:53`: `InvariantTsc` is read only when the basic maximum CPUID leaf (leaf 0, about 0x20) is at least 0x80000007. The extended maximum must come from leaf 0x80000000. So the bit is never set on any CPU, including KVM `-cpu host`, and `tsc.cpp:64` never enrols. Commit `234970a2` saw "not invariant" only under TCG, which hid this. There is no host test for `X86CpuFeature::init` because `cpuid` is not injectable.
2. `kernel/include/ours/time/clock_source.hpp:26` and `kernel/kernel/time/clock_source.cpp:114`: `Flags::HpClock` is never set by the HPET or the TSC. In one-shot mode (the dev-image default) `select_best_source` skips every source and returns null, and `enroll` returns Ok with no switch and no log. The boot CPU is already one-shot after the APIC timer enrols, so the TSC can never win there.
3. `kernel/kernel/time/anchor_page.cpp:74` and `:58`, `kernel/lib/userabi/src/userboot.cpp:795`: the user-space counter address is bound once to the HPET page (`bind_counter_va` has one caller and nothing unbinds), but `publish_anchor` sets `accessible` from that address alone and publishes whatever source is current. With the TSC selected, the page holds the HPET counter address with the TSC's ticks and ratio; the vDSO computes `hpet_now - tsc_ticks`, which wraps, and `monotonic_from` saturates, so every read returns `INT64_MAX`. The record says "user-space reads switch to the TSC"; `DESIGN.md` names this as future work. Fix: make "user-mappable counter address, or none" a property of the source and publish `accessible = 0` for a source without one, or give the vDSO an `rdtsc` path.

Also (4, 1/1): the TSC read in `tsc.cpp` and `arch/x86/tick.hpp` is a bare `rdtsc`, which is not ordered against loads. CPU0 reads the clock and then stores a flag with release; CPU1 sees the flag, but its `rdtsc` may have executed before the flag load, so `y < x` by tens of nanoseconds. "Invariant" in CPUID means constant rate, not synchronised cores, and there is no per-CPU sync check although the record claims "coherence across CPUs". Fix: `lfence; rdtsc` or `rdtscp`, and a sync test per application processor (Zircon's `platform_current_raw_ticks_synchronized`, Linux's `rdtsc_ordered()` and `check_tsc_sync_source()`).

The guest check `shell_tests.cpp:52` (`check_clock`) passes if the vDSO always traps and only tests monotonic increase, so it would catch none of the four.

### M10. The published anchor page can stay odd forever, and the vDSO reader spins without a bound

`kernel/kernel/time/anchor_page.cpp:88`, `kernel/lib/userabi/vdso/clock.cpp:22-43`. Re-derived 1/1; it adds to items 1 and 2 of `docs/repairs/261102/kernel-time.md`, which still stand.

The closing even store is `auto _ = STORE(...)`. If it fails, the page stays odd while the kernel's sequence says even, and every vDSO read spins forever in `snapshot()`, with no bound and no `pause`. The writer's odd window also contains `VmObject::write`, which takes locks and is preemptible, so a reader on the same CPU spins through the writer's preemption; `SeqLock` keeps interrupts off for exactly this reason. On weak memory models the reader's relaxed data loads are not fenced before the final sequence load (the kernel's own `SeqCount::read_retry` uses an acquire fence).

Fix: bound the reader (about 1000 spins, then fall back to `ours_clock_get_monotonic_via_kernel`), take one mutex over `reading()` plus the publish so a stale anchor cannot win, and check the closing store.

### M11. The userboot handle arrays are exactly full

`kernel/lib/userabi/src/userboot.cpp:488` (`handles[15]`) and `kernel/lib/userabi/userboot/userboot.cpp:327` (`handles[14]`). Re-derived 1/1.

Both are exactly full when the debug, crashlog, ACPI and clock-page handles are all present, and `handles[count]` is unchecked, so the next handle added is a stack overflow. The comments "Ours: 14 entries; theirs adds..." are merge residue, and repair item 2 quotes 14 and 13, which are stale. Fix: a named maximum with `DEBUG_ASSERT(count < MAX)`. The same shape exists in `pools.hpp:26`, where `MAX_INSTANCES = 11` is exactly the ten children plus the root with no build-time check against `root.cm` (read-only).

### M12. A CPU that joins late is not ordered against a concurrent removal on weakly ordered hardware

`kernel/include/ours/cpu.hpp:76-88`, `kernel/kernel/task/irq_rcu.cpp:44`, argument at `kernel/include/ours/task/irq_rcu.hpp:94-101`, reader load at `:129`. Re-derived 3/3; latent.

See T1 for the full argument. `unregister_domain` has no caller yet and x86 is safe, so nothing fails today; the comments promise an ordering the code does not give, and the design says nothing is assumed from x86. Fix: one `std::atomic_thread_fence(std::memory_order_seq_cst)` after the successful join compare-exchange. It runs once per join, and the interrupt path stays an acquire load. (The reviewer recalls Linux's `rcutree_report_cpu_starting()` placing an `smp_mb()` after setting the CPU's bit; that was not checked against a Linux tree.)

## Minor and style

Naming fixes first, as the user values them most.

- Naming, mm: `Pinned` means four things (`VmoFlags::Pinned` and `VmCowPages::Flags::Pinned` mean "never reclaimable", `PageKind::Pinned` is a queue, `pinned_count` and `pin_range` are counted DMA pins, and `nr_pinned_pages_locked()` answers 0 for a contiguous VMO whose pages sit on the pinned queue). Rename the node flag to `Wired`, which the tree already says (`create_from_wired_pages`), and keep "pin" for counted pins. `kernel/commands/vma_physical_test.cpp` registers `physvmo.test` beside `vma_phys_test.cpp` (`vmaphys.test`): rename it `physvmo_test.cpp`. `VmaDispatcher::map` tests "is physical" two ways (`vmo.vmo()->type() == Type::Physical` and `vmo.is_physical()`): keep `is_physical()`.
- Naming, irq (re-derived 1/1, but partly overtaken by the eleven newer commits, so check against the tree first): `virq_of_hwirq()` resolves only the root domain while the doc calls it "the inverse" of `hwirq_of_virq()`, which resolves any domain (`kernel/include/ours/irq/mod.hpp:214-219`); suggest `virq_of_root_hwirq()` and fix the doc. `Status::NoMemory` and `Status::OutOfMem` are one value (`system/public/ours/status.hpp:44-45`) but both spellings are used (`domain.cpp:51,72` against `domain_registry.cpp:63,72`). The reviewer's `hirq`/`hwirq` and "directory" findings were fixed or renamed by the newer commits.
- Naming, time: `boot_offset` (`kernel/kernel/time/clock.cpp:131`) returns `REAL_BASE`, the wall-clock epoch, so call it `realtime_offset`. "Clock page" (`CLOCK_PAGE_BASE`, `VmoClockPage`, `CLOCK_PAGE_VMO`) is the HPET MMIO page: call it `hpet_page` or `counter_page`, since "clock" will be wrong once a TSC exists. `AnchorReading`, `Clock::reading()` and `SteadyClock::reading()` return the anchor, not a reading: `AnchorSnapshot` and `anchor()`. `anchor_page.*` also publishes `ticks_hz` and `boot_offset`, and the gni category `time` hosts `kernel_version` and `published_version`: use a category such as `published`. `PROCESS_ASPACE_BASE` duplicates `USER_ASPACE_BASE` with no `static_assert` (`clock_page.hpp`). The records say `HPET_USER_BASE` and `VmoClockPage=0xe1`; the code says `CLOCK_PAGE_BASE` and `0xe2`.
- Naming, ddk and drivers: `ddk_test.cpp:69` `DriverKitTest` to `DdkTest`; `docs/changes/261103/driver-kit.md` to `ddk.md`, and its code sample at lines 21-27 still uses `driver_kit::`; `.agents/tasks/driver-kit/DESIGN.md:34` and `PLAN.md:17` still say `driver_kit` and `system/lib/driver-kit`. `EcamBase`, `ours_acpi::ecam_base` and `EcamBaseReply` return a window (address, size, buses): `EcamWindow`. `ddk::irq::take` consumes the root and clashes with `Resources::take`: `claim_line`. `ddk::Kind` to `ResourceKind`, `Getter` to `ResourceGetter`. `virtio_blk.cpp:114` `SECTOR` duplicates `ours_block::BLOCK_SIZE`: add a `static_assert`.
- Naming, file system and tooling: `ours_block.hpp` documents `MAX_READ_SECTORS` but the constant is `MAX_READ_BLOCKS`. `fsio.hpp:207` and `:234` declare `chdir(path, flags)` twice with the doc paragraph pasted twice. The hand-typed ordinals for `File.Write`, `Directory.Unlink`, `Block.*` and `Admin.Mount` cite a `wire/test/ordinals_test.py` that does not exist (all were recomputed with SHA-256 and match; use `wire::method(...)` and join the golden list). `_vdso_decls_dir` and `--vdso-decls-dir` are not the declarations' directory (files go to `<dir>/vdso_decls/<cat>.inc`). The "Eight targets" header of `component_manifest.gni` does not count `.names.config`.
- Convention, comments that name other systems (new in this window): `system/public/ours/object.hpp:9`, `system/public/public_headers.gni:14,17`, `kernel/include/ours/panic.hpp:12-13`, `system/public/ours/syscalls/process.gni:136`.
- Stale or wrong comments: `vma_dispatcher.hpp:122-123` still says a writable mapping of a physical VMO is InvArgs, which `27833140` removed, and `vma_dispatcher.cpp:211` and `:242` are two blocks that both set Discache with contradictory rationales (re-derived 1/1). The comment on `RIGHTS_DEFAULT_BTI` (`bti_dispatcher.hpp:19`) says "Write" while the expression grants `Io`. `virtio_blk.cpp:39,49,432` still say "polls the used ring". `hpet.cpp` ("nothing else is enrolled yet") and `clock_source.hpp` ("the only source enrolled today") are stale. `time.gni:63` says `OURS_KERNEL_VERSION_STRING` is in `<ours/version.hpp>`; it is in the generated `ours/version_build.hpp`. `kernel/lib/syscall/BUILD.gn` still says "from syscalls.gni". The temporary witnesses at `btreefs.cpp:675-682` and `:913` are still there although `11a59f5d` says they were removed (read-only). `ours_btreefs.hpp:53-59` and `btreefs.cpp:4-41` still say "flat directory" and describe a leaf chain that nothing reads.
- `kernel/mm/memory_reclaimer.cpp:78` against `:60` (re-derived 3/3, low): `run()` calls `take()` before `reclaim()` takes `m_reclaim_lock`, so a synchronous OutOfMemory walk can finish and the thread then walks a stale ask. The effect is over-reclaim of clean pages, which fault back in; the header at `memory_reclaimer.hpp:48-51` overstates the guarantee. Fix: take the ask under the lock (lock, take, walk).
- `~PmtDispatcher() = default` (`pmt_dispatcher.hpp:88`) does not assert `m_pin == nullptr` (re-derived 1/1, not reachable today). A `DEBUG_ASSERT` would catch a future misuse at the cause instead of at `bti_dispatcher.cpp:20`.
- `kernel/kernel/irq/domain.cpp:43-46` (re-derived 1/1): the virq high-water mark (`domain_registry.cpp:87-89`) never goes down and each domain takes a 32-aligned block, so after about 134 million register and unregister cycles registration answers InvArgs for a valid request; NoSpace or OutOfRange would be accurate.
- `fsio.cpp:815` (`list()`, re-derived 1/1): the inner loop sets `done = true` after the first 512-byte page, so a directory of more than about 30 entries is silently truncated; the only caller lists one or two. `fsio.cpp:446` and `:383` refuse legitimate names starting with `.` or `..` (`.config`, `..x`), and `fsio.cpp:722` opens `/` for a bare `unlink("x")` (re-derived 1/1).
- `ours_btreefs.cpp:256` (`erase_child`, Probe plus re-derived 1/1): the `memmove` length is `(count - at) * 24` but should be `(count - at - 1) * 24`; it reads 8 bytes past the block when the parent has 169 keys. Benign inside the cache array, wrong anyway.
- Read-only, not independently re-derived, file system: `btreefs.cpp` and `memfs.cpp` `handle()` drop `request_handles` on every decode error and unknown method, where `ioservice` closes them; `fshost.cpp:205-266` leaks the directory handle on every refused Mount; `cow()` (`:445`) calls `mark_dead(original)` without the range check `free_block` has, so a corrupt child pointer can clear an arbitrary bitmap bit; `BlockAdapter` has no partition bound check; `load_superblock` does not validate `bitmap_first`, `bitmap_blocks`, `blocks` or `height`; `scan` recurses with 4 KiB frames up to 255 deep; `Client::receive` turns an Ok epitaph into `err(Ok)`; `open_below_dir` truncates or creates before reserving the connection row. Per CLAUDE.md the scan performance problem (`ours_btreefs.hpp:826-830`, `btreefs.cpp:631`: the prune never fires, so each `ReadDirents` is O(tree)) belongs in `docs/repairs/`, not in a quiet fix. `btreefs.cpp` and `memfs.cpp` duplicate about 300 lines.
- Read-only, drivers: `virtio_blk.cpp:549-614` trusts what firmware gives it (it assumes the window is bus 0 and ignores `first_bus`, treats `device_offset == 0` as "not found" so slot 0 is skipped, never sets the PCI command register's bus-master and I/O bits, accepts an I/O BAR of 0, and truncates the queue address above 2^44). `ddk.hpp:385-400` with `virtio_blk.cpp:631-638`: `irq::take` closes the root on success while `Resources` still records it, so `drop` closes it twice (harmless only because the stale handle value is BadHandle). `acpi_route.cpp:105` re-implements the client table and pump that `server::ProtocolServer` provides, which would close repair 261031 item 5 for free.
- Tooling (Probe unless noted): `system/component_manifest.py:1752-1765` (`names_header`) writes manifest paths unescaped into C++ string literals, while `check_path` allows any character except NUL and `/`; a path with a quote breaks the header and a newline injects lines. `time_values.hpp` is listed in `public_headers.gni` but needs `<mtl/ratio.hpp>` and `<mtl/scale.hpp>` from `kernel/lib/mtl`, so it fails standalone and a sysroot built from the list would not compile (`ours/types.hpp:11` has the same kind of hidden dependency on the force-included `prelude.hpp`). `build/version/gen_kernel_version.py:43` always rewrites its output, runs `git status --porcelain` without `--no-optional-locks`, and lists `.git/index` in its depfile, so any index refresh by any session retriggers a rebuild of the vDSO and its users; write only when the text changed. `ours_syscall_categories` in `syscalls.gni` is a third hand-kept list beside the imports and the concatenation, and `public_headers.gni` is a hand-kept mirror of the directory; the entry-schema documentation (name, brief, doc, args, attrs, ret, `category`, `vdso`) was dropped by `0b02c363` and survives only in a Chinese 2026-09-23 design document. `scripts/make_gpt_disk.py` claims an alternate header it never writes, uses the local name `super` (a builtin), imports `zlib` twice, and hand-copies the btreefs superblock offsets with no host test tying writer to reader.
- Test gaps, each of which lets a defect above pass: nothing pins a pager VMO or runs pin, write, unpin, reclaim (H1; `docs/repairs/261030/mm-pin.md` item 3 records the gap), and `page_reclaim_test.cpp:238,266,302,346` calls `PageQueues::reclaim()` directly, bypassing `m_reclaim_lock`; `check_idle_follows_the_state` (`interrupt_test.cpp:797`) passes without the second `owed()` read at `interrupt_dispatcher.cpp:228`, and the task plan records that mutation as the survivor (a test hook between the first `owed()` and `update_state()`, like `set_before_park_hook_for_test`, would catch it); `ACompletionBeforeAReconnectedEntryIsDropped` (`block_server_test.cpp:348`) passes with the generation check deleted because the fake kernel never reissues handle values (`fake_kernel.cpp:45`); the btreefs tests use a zeroed device (H2), and `ASuperblockFailureLeavesTheOldTreeWhole` (`ours_btreefs_test.cpp:731`) still passes with the pre-fix dead-bit order (Probe); `gen_published_offsets_test.py` compares the generator's output with the same `st_value` it read and never runs the kernel's `published_offset()` formula; `read_test.cpp:44-60` computes the expected value with the same `mtl::scale` it tests; `gen_syscalls.py` has no test; the virtio ring logic lives in `main`'s file and was found by QEMU twice (`1b3780ed`, `d9abe81f`), so it needs a ring class host-tested against a model device (wrap at 256 and 65536, stale index).

## Teaching opportunities

### T1. Store-then-load needs a full fence on both sides (M12)

`cpu.hpp:76-88`, `irq_rcu.cpp:44`, `irq_rcu.hpp:94-101` and `:129`. The invariant: a table is freed only when no CPU that could still read it holds it. Two parties touch it concurrently: the remover, which retracts the table pointer, issues a `seq_cst` fence and then reads the `ONLINE` mask to decide which CPUs to wait for; and a CPU that joins late, which sets its `ONLINE` bit with a `seq_cst` compare-exchange and then reads the pointer with an acquire load.

The worst interleaving is the classic store buffer (Dekker) shape. The joining CPU's bit is still in its store buffer when it loads the pointer and gets the old table; the remover retracts, fences, and reads `ONLINE` without that bit; it waits for the other CPUs only, frees the table, and the joining CPU then uses it. The C++ rule ([atomics.order], paragraph 4) orders "my store, then my load of a different location" against the other thread's "store, fence, load" only if both sides have a `seq_cst` fence, or the joining CPU's load is itself `seq_cst`. A `seq_cst` read-modify-write followed by an acquire load of another location gets no such ordering. On x86 a `lock cmpxchg` is a full barrier, so the hardware hides the gap. On RISC-V the compare-exchange is `lr.aqrl`/`sc.rl` and the acquire load is `ld; fence r,rw`, so the load can pass the store; Arm with LDAPR (FEAT_LRCPC) is not safe either.

What changes with a different design: a fence after the join compare-exchange costs once per CPU join and leaves the interrupt path unchanged (an acquire load); making the interrupt-path load `seq_cst` would put a fence on every interrupt, which is the wrong end to pay. The lesson is that "the comment says it needs this to be `seq_cst` elsewhere" is a hope, not a guarantee: write the pairing as two named sync points and check each side has its half.

### T2. One fact stored in two places drifts (H1)

The dirty state of a pager page is encoded as the queue kind, and the queue kind is also used for "pinned". When the second meaning overwrote the first, the first was lost. The cure is not a patch at the unpin; it is to give "written" its own bit that no other transition touches, and derive the queue from the pair. Zircon keeps `MoveToNotPinnedLocked` per page for this reason: it asks the page what it was (Clean or Dirty), not what the node type implies. When a refile function takes the node type as its only input, ask what state the page carried into the call.

### T3. A result that must be acted on needs `[[nodiscard]]` (M4)

`close_client()` returns `Close` because only the Loop can free the slot; `auto _ = Base::close_client(...)` throws away the one thing the caller was supposed to do with it. The entry-reuse invariant (`server.hpp:65-73`) is what the dropped value protected. Marking the function `[[nodiscard]]` turns the next such mistake into a compile error, and the bug is a good example of why a callback that cannot return an action must not own an operation whose result is an action.

### T4. The odd window of a seqlock must be short and uninterruptible (M10)

The invariant is: while the sequence is odd, readers must retry; so readers on the same CPU as a preempted writer spin until it is rescheduled. That is why the kernel's `SeqLock` keeps interrupts off through the writer section, and why the user-space reader needs a bound with a fall-back to the system call. Ask, for each writer section: can it block, can it fail halfway, and what does the odd value mean if it never becomes even.

### T5. What a power cut between two writes leaves (M2, M3)

Write out the commit as a list: data blocks, tree blocks, taken bits, the superblock, dead bits. For each adjacent pair, say what the disk looks like if the power goes between them. The in-place data write (M2) is the pair "new bytes, old tree"; the taken bits before the superblock (M3) leak blocks; and with no flush every ordering claim depends on the device being write-through. btrfs answers this with generation numbers in `should_cow_block()` and with flush barriers; this volume has neither yet, and the docs should say which guarantees are real.

### T6. The device owns the memory until the device says it does not (M5, M6)

DMA memory has two owners, and the kernel's unpin is a promise that the device is done. The driver's exit path is where that promise is made, so it must reset the device and observe status 0 first. The crash path is accidentally safer because the kernel quarantines a pin that is closed without an unpin. Likewise a request that exits early leaves the device holding the descriptors: the safe response is to treat the whole queue as poisoned, not to carry on one completion behind.

## Suggested follow-up order

1. Naming fixes: the mm `Pinned` rename and `physvmo_test.cpp`, `boot_offset`, "clock page", `AnchorReading`, the `ddk` leftovers (`DriverKitTest`, `driver-kit.md`, `driver_kit::` in the record), `virq_of_root_hwirq`, `EcamWindow`, `PROCESS_ASPACE_BASE`; then the comments that name Zircon.
2. H5, the skill links (one command, and everything else in this list is easier to do with the skills visible).
3. H2 and H3 together (btreefs node initialisation and the mount path), since both can destroy a volume; then H4, the rights checks and the memfs row.
4. H1, the pager pin and unpin, with the test that fails first; then M1 to M3 as one btreefs copy-on-write round, deciding M2 with the user.
5. M4 to M8, the driver stack: `[[nodiscard]]` on `close_client` and the Close handling, the device-quiesce guard, the poisoned-queue policy, the queue-size layout, and `MappedMmio` with its test.
6. M9 as one change in the order 1, 2, 3, 4, with a boot check that the TSC is selected and a guest test that sleeps 100 ms and compares the fast path with the trap.
7. M10 and M11, then M12's one-line fence.
8. The test gaps, so that each fix above lands with a test that fails without it.
9. Record in `docs/repairs/` rather than fix quietly: the btreefs scan cost, the entry-schema documentation loss, and `acpi_route` re-implementing `ProtocolServer`.
