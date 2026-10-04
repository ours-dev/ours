# panic-crashlog: keep a panic's report in RAM across a warm reset, and read it on the next boot

**Date**: 2026-10-04
**Status**: Accepted (2026-10-04)

## Problem
A panic leaves its report only on the serial console. This is repair item 5 in `docs/repairs/261003/kernel-panic.md`. Nothing stores the message or the log tail anywhere a later boot could read them. Today that costs nothing, because `vpanic()` ends in `arch::halt()` for good and nothing ever boots after a panic. It matters once a panic can reboot the machine: then the console of the crashed boot may be gone, and the next boot is the only place left to say what happened.

What the tree has and lacks, from a survey on 2026-10-04:
- **No reset path at all.** There is no ACPI reset register, no port 0xCF9 or 0x64 write, no reboot syscall or command. `scripts/qemu_boot.py` passes `-no-reboot`, so any guest reset exits QEMU.
- **A reservation mechanism exists.** In phys, `BootMem::reserve(Range, MemoryType)` (`kernel/phys/lib/bootmem/include/bootmem/physical_memory.hpp:91`), called before `snapshot_memory_map()` (`kernel/phys/physboot.cpp:168`), keeps a range away from every allocator. Its type reaches the kernel in `Handoff::mem.ranges` and is replayed by `handoff_early_pmm()` (`kernel/mm/init_pmm.cpp:129`). There is no `MemoryType` for a persistent range yet (`kernel/phys/lib/bootmem/include/bootmem/types.hpp:67`).
- **The firmware runs again on a warm reset.** QEMU boots OVMF, then GRUB-EFI, then bootshim. OVMF re-initialises and uses guest RAM on every reset, so whether a given range survives is unknown until measured.
- **Kernel options** come from the OBI Config item (`kernel/include/ours/boot/options.hpp`). Phys has its own options object (`kernel/phys/boot_options.cpp`), which reads the OBI Config and then the GRUB command line.

## Goals and non-goals
Goals:
- When a panic reboots the machine, the next boot finds the panic's message and the tail of its log, and says so.
- A record that was torn by a reset in the middle of being written, or was never written, is recognised as such, never shown as a crash.
- A record is shown once: the boot that reads it clears it.

Non-goals:
- Records surviving a power cycle. RAM does not keep data without power; that needs storage this kernel has no driver for.
- Watchdog or brown-out reboots, which Zircon's crashlog tracks by updating an uptime field periodically. There is no watchdog here.
- A user-space crash reporter, unless decision D3 says otherwise.

## Design
Objects:
- **The region.** A few pages of RAM at a physical address that is the same on every boot. Phys reserves it as a new `MemoryType::Crashlog`, so no allocator in phys or the kernel ever hands it out. The kernel finds it in the memory map by that type, so `Handoff` needs no new field.
- **The record.** A header followed by text:
  - The header holds `magic`, `version`, `payload_len`, `payload_crc32`, `header_crc32` and the panic's TSC time.
  - The text is the panic message and the same ring tail the console report prints.
  - The magic is written last, after both CRCs. A reset in the middle of the write therefore leaves no magic, and the next boot sees no record instead of a torn one. Zircon's `ram-crashlog` (`zircon/system/ulib/ram-crashlog/include/ram-crashlog/ram-crashlog.h`) uses magic and CRC32s the same way, with two header copies because it also updates an uptime field while the system runs.
- **The writer.** `vpanic()`, on the CPU that won `claim_report()` only, after the console report. One writer, interrupts off, every other CPU stopped, so it needs no lock.
- **The reader.** Early boot in the kernel, before anything else touches the region: check the magic, version and CRCs, report what it found (D3), then clear the magic so the next boot does not report it again.

Events, for a panic with reboot on:
1. The panic prints its console report, as today.
2. The winning CPU renders the message and the ring tail into the region, then the CRCs, then the magic.
3. It writes the region back from its caches (`clflush` over the range, or `wbinvd`), because a reset does not write back dirty cache lines on real hardware. QEMU models no cache, so only real hardware would catch a missing flush.
4. It resets the machine (D1).
5. OVMF, GRUB and phys run. Phys reserves the region again before any allocation.
6. The kernel validates the record, reports it, and clears the magic.

Linux's counterpart is ramoops (`fs/pstore/ram.c`). It takes an address and size from `ramoops.mem_address` and `ramoops.mem_size` (or a devicetree `reserved-memory` node), receives `kmsg_dump(KMSG_DUMP_PANIC)`, and shows the record under `/sys/fs/pstore` after the reboot. Zircon's is the crashlog. Phys passes the bootloader's `ZBI_TYPE_NVRAM` range in `PhysHandoff::nvram` (`zircon/kernel/phys/include/phys/handoff.h:401`). `MappedCrashlog` (`zircon/kernel/include/platform/mapped_crashlog.h`) writes into it. `crashlog_to_vmo()` (`zircon/kernel/lib/userabi/userboot.cc:278`) recovers it into a read-only VMO named "crashlog", which userboot receives with its other handles.

## Alternatives considered
- **A UEFI variable**, as Zircon's `EfiCrashlog` (`zircon/kernel/platform/efi_crashlog.cc`) uses on PCs. Rejected: the kernel has no EFI runtime services, and `Handoff::efi_system_table` is always empty.
- **A block device or an emulated NVDIMM** (`-device nvdimm`). Either would survive even a power cycle. Rejected for now: there is no storage driver and no ACPI NFIT parsing, and either is far larger than this item.
- **Phys picking the address itself**, for example the top of the highest RAM range. Rejected: OVMF puts its own working memory at the top of low RAM on every boot, so the obvious choices are the ones most likely to be overwritten. Which ranges are safe is a fact about the platform's firmware, and the platform should declare it (D2).
- **Two header copies with transactional updates**, as Zircon's `ram-crashlog` has. Not needed: the record is written once, at panic, never updated while the system runs.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D0. Build this now? | (a) Now, the minimal slice below. (b) Defer until something needs reboot-on-panic. | (a), for the teaching value: reserving memory across the boot chain, a reset that keeps RAM, cache write-back before a reset, and detecting a torn write. (b) is what "the user-facing interface comes first" argues for, since nothing needs this yet. | | (a), as an MVP (2026-10-04) |
| D1. How a panic reaches the next boot | (a) A kernel option `kernel.panic.reboot`, off by default, as Zircon's `kernel.halt-on-panic` and Linux's `panic=` work. When it is on, the panic resets the machine through port 0xCF9, then the keyboard controller, then a triple fault. (b) Panic keeps halting, and the test resets the guest from the QEMU monitor. | (a). The record exists for the reboot, and the reset is a small, real piece of the platform. (b) needs no kernel change, but it tests a machine that never reboots itself. | | (a) (2026-10-04) |
| D2. Where the region's address comes from | (a) An option the image declares, `kernel.crashlog=<size>@<address>`, which phys reads and reserves (Linux's `ramoops.mem_address`/`mem_size`). (b) A fixed address in phys code. | (a). Which range survives OVMF is a platform fact, so the image states it. Moving it means changing a build argument, not code. | | (a) (2026-10-04) |
| D3. How the next boot reports it | (a) The kernel logs the recovered record at boot under a fixed marker line. (b) Also hand it to userboot as a read-only "crashlog" VMO, a 7th bootstrap handle, as Zircon does. | (a) now. (b) once a user program wants to read it, per "the user-facing interface comes first". | | (b): log it and hand userboot a read-only VMO (2026-10-04) |

## Testing and verification
- **Step 0, a measurement before any design is final.** Under QEMU q35 with OVMF, does a chosen RAM range survive a warm reset? Write a pattern into a candidate range, reset, and compare on the next boot. Try a few addresses (low, middle, near the top of low RAM). This decides D2's default address, and whether the plan works at all on this firmware.
- **Host test.** Record encode and decode in logz4 or a small library: a valid record round-trips; a wrong CRC, a missing magic or a wrong version is rejected.
- **Boot test.** `crash panic` on an image with reboot on: the console shows the report, the guest resets (the harness runs QEMU without `-no-reboot` for this test only), and the second boot logs the recovered message under its marker. A third boot logs nothing, because the second cleared the record.
- **`ours-verify`** on both images, with reboot off by default, so their behaviour is unchanged.
- Cannot be verified here: the cache write-back. QEMU models no cache, so a missing flush passes every test. It rests on the argument in Design, step 3.

## Open questions
- Does OVMF leave any range of guest RAM untouched across a warm reset on q35, and which (Step 0)?
- Does GRUB-EFI load bootshim or `image.obi` into a range that could overlap the region? GRUB picks its load addresses itself.
- Is a range of type `Crashlog` covered by the kernel's physmap, so the panic path can write it without mapping anything at panic time?
