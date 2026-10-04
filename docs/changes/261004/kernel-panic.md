# kernel-panic changes

## Title: phys reserves an image-declared RAM range for the panic crash record (2026-10-04)

**Files**
- `kernel/phys/lib/bootmem/include/bootmem/types.hpp` — `MemoryType::Crashlog` and its name
- `kernel/phys/include/ours/phys/boot_options.hpp`, `kernel/phys/boot_options.cpp` — phys option `kernel.crashlog=<size>@<address>`, numbers decimal or 0x hex
- `kernel/phys/include/ours/phys/memory_provisioner.hpp`, `kernel/phys/memory.cpp` — `MemoryProvisioner::reserve_crashlog()`
- `kernel/phys/physload.cpp` — calls it after the boot image and the package are reserved
- `kernel/products/image/dev/BUILD.gn`, `kernel/products/image/dev.dbg_1/BUILD.gn` — both declare `kernel.crashlog=0x4000@0x4000000`
- `.agents/tasks/panic-crashlog/DESIGN.md`, `PLAN.md` — the user's decisions, and the four steps

**Symptom**
None; this is step 1 of a feature (repair item 5, `docs/repairs/261003/kernel-panic.md`). A panic's report survives only on the console. A record written into RAM by one boot can only be read by the next if every allocator of every boot leaves that RAM alone.

**Cause**
Nothing reserved such a range. Phys gives every usable range to BootMem, physboot and the kernel allocate from it top-down, and the kernel hands whatever is left to the buddy. A record in ordinary RAM would be overwritten by the very next boot, before anything could read it.

**Change**
The image declares the range and phys reserves it, before anything in phys allocates:

```cpp
memory.reserve_boot_image();
memory.reserve_package(input.unwrap());
memory.reserve_crashlog(BOOT_OPTIONS);   // new
memory.install(&aspace);
```

`reserve_crashlog()` reserves the range as `MemoryType::Crashlog`. The type rides the handoff's memory map into the kernel, where `handoff_early_pmm()` replays it and `stamp_reserved_frames()` marks its frames Reserved, so the buddy never gets them. No `Handoff` field is needed: the kernel will find the range by its type.

Why the image declares the address (decision D2, the user's choice): which RAM survives the firmware on a warm reset depends on the platform, not on the kernel. On this QEMU, OVMF keeps 8 to 9 MiB for itself, unpacks its DXE volume from 9 MiB on every boot, and keeps its working memory at the top of low RAM. GRUB placed the shim and the OBI below 4 MiB. 64 MiB is far from all of these. Changing the address is a build-argument change, not a code change.

Three choices in the reservation:
- It runs after the boot image and the package. `BootMem::reserve()` refuses a range that a different type already holds, so a region overlapping what GRUB loaded is refused instead of reserved over live data.
- It is not fatal. A boot without a crash record is still a boot, so a misaligned or refused range logs a warning and the boot goes on.
- It does not check that the range is RAM. A range in a hole would be reserved and then fault when the panic writes it. Declaring a sane range is the image's job, as with Linux's `ramoops.mem_address`.

**Reference**
- Linux ramoops (`fs/pstore/ram.c`) takes `ramoops.mem_address` and `ramoops.mem_size`, or a devicetree `reserved-memory` node. The range is kept from the allocator by `memblock_reserve()`, or by `memmap=` on the command line.
- Zircon's phys passes the bootloader's `ZBI_TYPE_NVRAM` range in `PhysHandoff::nvram` (`zircon/kernel/phys/include/phys/handoff.h:401`), and memalloc marks it `memalloc::Type::kNvram` (`zircon/kernel/phys/lib/memalloc/include/lib/memalloc/range.h:130`).

**Verification**
`boot_check.py --out out/kernel-panic`: dbg PASS (20 passed, 0 error lines), rel PASS (18 passed, 0 error lines). The rel log shows `[info] crashlog: reserved [0x4000000, 0x4004000)` from phys and `Reserved 4 frames (16 KiB) of crashlog` from the kernel. clang-format clean; clang-tidy reports nothing new. Not yet verified: that the range survives a warm reset, which step 3 measures.

**Deliberately not done**
- No check that the declared range lies in RAM (see Change).
- The kernel does not use the range yet; steps 3 and 4 do.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must the range be reserved in phys, rather than by the kernel when it first touches it?
- Why is the reservation made after the boot image and the package, and what does `BootMem::reserve()` do with an overlap?
- Why does the image, not phys code, choose the address?

## Title: kernel/lib/crashlog holds the crash record's format, written magic-last and read back only when whole (2026-10-04)

**Files**
- `kernel/lib/crashlog/` — new library, scaffolded with copier from `tools/templates/cpp_library`: `Header`, `Writer`, `read()`, `clear()`
- `kernel/lib/crashlog/test/crashlog_test.cpp` — nine host tests
- `BUILD.gn` — the test joins the default build on the host toolchain

**Symptom**
None; step 2 of the panic-crashlog feature.

**Cause**
The region from step 1 is plain RAM that survives a warm reset, and nothing else. A reset can land halfway through the panic's write. The RAM can hold garbage from a cold boot, or a record from an older kernel. The reader has to tell a whole record from all of these, and a format that cannot is worse than none: it reports a crash that never happened.

**Change**
A 32-byte header in front of the text:

```cpp
struct Header {
    u64 magic;          // MAGIC only when the record is whole
    u32 version;
    u32 payload_len;
    u32 payload_crc32;
    u32 header_crc32;   // over the header with magic and itself taken as zero
    u64 time;
};
```

How the three ways to go wrong are caught:
- **A torn write.** `Writer`'s constructor clears the old magic before a byte of the new record goes in, and `seal()` writes the lengths and CRCs before the magic. A reset anywhere before the magic lands leaves no record; a reset after it leaves a whole one. The magic is the commit point. The header CRC is computed with the magic taken as zero, which is what lets it be written first.
- **Reordering.** A reset is not another CPU, so no atomic is needed. What must hold is that the magic reaches memory after the bytes it vouches for. x86 makes stores visible in program order, and the kernel will write the cache back before it resets (step 3). That leaves the compiler, and an `atomic_signal_fence(release)` before the magic's store stops it from moving that store up.
- **Garbage, decay and old formats.** These are caught by the magic, the version, a payload length that must fit the region, and both CRCs. `read()` returns `ustl::NONE` for anything else.

The CRC is `obi::crc32()` (`kernel/lib/obi/include/obi/checksum.hpp`), CRC-32/ISO-HDLC, the same checksum as Zircon's ram-crashlog. It is reused rather than written a second time.

The library knows nothing of where the region is or how it is mapped. The kernel passes it in as a byte span, so all of the above is tested on the host.

Rejected: Zircon's two header copies. Zircon updates an uptime field while the system runs and needs that update to be atomic. This record is written once, by a panic, and never updated.

**Reference**
Zircon's ram-crashlog (`zircon/system/ulib/ram-crashlog/include/ram-crashlog/ram-crashlog.h`): a magic number flipped last to commit, and separate CRC32s for the header and the payload. Linux ramoops' `struct persistent_ram_buffer` (`fs/pstore/ram_core.c`) has a signature, a start and a size, and protects its payload with ECC rather than a CRC.

**Verification**
- The host test passes 9 of 9: a round trip; a region never written holds no record; clear() removes the record; a writer that never sealed leaves no record, even over an older whole one; a flipped payload bit, a flipped header bit and another version are each rejected; text longer than the region is cut and reported; a region smaller than a header holds nothing.
- `boot_check.py --out out/kernel-panic`: dbg and rel PASS. The kernel does not link the library yet.
- clang-format clean; clang-tidy reports nothing in the new files.

**Deliberately not done**
- A record whose payload CRC fails is dropped whole. Zircon still delivers the bytes, with a flag saying they may be damaged. Partial delivery can come when someone needs it.
- No compression and no second record. Only the latest panic is kept.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the magic go in last, and why must the constructor clear the old magic first?
- Why is a compiler fence enough, with no atomic, for a write that a reset may interrupt?
- Why is the header CRC computed with the magic taken as zero?

## Title: a panic leaves a crash record and, with kernel.panic.reboot, resets; the next boot reports the record and clears it (2026-10-04)

**Files**
- `kernel/top/crash_record.hpp`, `kernel/top/crash_record.cpp` — find the Crashlog range in the handoff's memory map (PlatformEarly hook); `record_panic()` writes the message and the ring tail; the Platform-level hook logs the previous boot's record and clears it
- `kernel/top/panic.cpp` — after the console report: `record_panic()`, then `platform_reboot()` when `kernel.panic.reboot` is set; the template `vpanic()` renders its message and delegates to the one body
- `kernel/include/ours/boot/options.hpp`, `kernel/top/boot_options.cpp` — `kernel.panic.reboot`, and `boot::try_options()`, which does not panic before the options are parsed
- `kernel/platform/include/ours/platform/power.hpp`, `kernel/platform/pc/power.cpp` — `platform_reboot()`: port 0xCF9, then the i8042 pulse, then a triple fault
- `kernel/lib/arch/include/arch/cache.hpp`, `kernel/arch/x86/lib/arch/include/arch/x86/cache.hpp` — `arch::clean_cache_range()`, CLFLUSH between MFENCEs, filling the x86 header that was a TODO placeholder
- `kernel/products/image/dev.dbg_1/BUILD.gn` — `kernel.panic.reboot=true`
- `scripts/qemu_boot.py` — `REBOOT=1` drops `-no-reboot`
- `scripts/kshell_run.py` — `--reboot`: after a crash's report, the next boot must log the record, its first line the panic message
- BUILD files for the new sources and dependencies

**Symptom**
None; step 3 of the panic-crashlog feature. Before it, a panic halted for good and its report existed only on the console.

**Cause**
Three pieces were missing:
- **A writer.** Nothing put the report anywhere but the console.
- **A way back.** Nothing reset the machine, so no boot ever followed a panic.
- **A reader.** Nothing in early boot looked for what a panic had left.

**Change**
The panic path, after the console report:

```cpp
print_context();

record_panic(message, TAIL_LINES);
if (reboot_requested()) {
    print_raw(REBOOT_MARKER);
    platform_reboot();
}
```

The decisions in it:
- **The record comes after the console.** The console is what someone may be watching now, and a fault while writing the record must not cost the report.
- **The record holds the message first, then the same 32-line ring tail the console shows,** fed through a `logz4::Logger` that appends to the record. It has no banner and no markers. The next boot logs the record, and a banner in it would read to a harness as a new panic.
- **The cache is written back before the reset.** `record_panic()` ends with `arch::clean_cache_range()` over the region. A reset discards dirty lines rather than writing them back. QEMU models no cache, so no test here can catch a missing flush; it rests on that argument.
- **The reset tries three ways, in Zircon's order.** First 0x06 to the chipset's Reset Control Register at 0xCF9, as Zircon's `cf9_reboot_system()` does. Then the i8042 pulse-reset command, as `pc_keyboard_reboot()` does. Last, an empty IDT and an `int3`, which escalates to a triple fault, as Linux's `BOOT_TRIPLE` does. Each method gets 100 M TSC ticks to take effect before the next is tried.
- **The option is read without panicking.** `reboot_requested()` uses `boot::try_options()`. A panic before the options are parsed would otherwise raise a second panic inside `options()`, and that CPU would lose `claim_report()` and halt silently.
- **Reboot is off by default.** A panic stays on screen, and a kernel that panics on every boot does not loop. Only dev.dbg_1 turns it on, and under `qemu_boot.py` its reset still ends QEMU unless `REBOOT=1`.

The reader runs in two hooks:
- **`find_region()`, at PlatformEarly.** It scans `boot::handoff().mem.ranges` for `MemoryType::Crashlog` and keeps a physmap span. It runs early so that a panic during the rest of the boot still leaves a record.
- **`recover_record()`, at Platform on the laborer thread.** It reads the region with `crashlog::read()`. A whole record is logged line by line at Warn under `crashlog: | `, then cleared, and the cleared header is written back from the cache. Warn rather than Error, because this boot is fine; the panic was the previous one's.

The template `vpanic(fmt, args)` now renders into a 512-byte buffer the way `panic()` does and calls `vpanic(std::string_view)`. This removes the second copy of the sequence, which every change in this series had to make twice.

**Reference**
- Zircon: `platform_panic_start()` and `PanicFinish()` lead to `platform_halt(HALT_ACTION_REBOOT)`. `MappedCrashlog::Finalize()` (`zircon/kernel/platform/mapped_crashlog.cc`) writes the record, and `crashlog_to_vmo()` (`zircon/kernel/lib/userabi/userboot.cc:278`) recovers it on the next boot. The pc reset is `reboot()` in `zircon/kernel/platform/pc/power.cc`.
- Linux: `panic()` calls `kmsg_dump(KMSG_DUMP_PANIC)` into ramoops, then `emergency_restart()` once `panic_timeout` is set. `native_machine_emergency_restart()` (`arch/x86/kernel/reboot.c`) walks ACPI, KBD, EFI, CF9 and TRIPLE.

**Verification**
- `kshell_run.py --image dbg --reboot "crash panic"`: ok. The console showed the report, then `--- rebooting: kernel.panic.reboot is set ---`, then OVMF, GRUB and phys again (`crashlog: reserved [0x4000000, 0x4004000)`). The second boot logged `crashlog: the previous boot panicked; its record (1304 bytes) follows`, then `crashlog: | crash: deliberate panic from the kernel shell`, then the 32 tail lines ending with the panic message and its prefix. This is also the measurement the design asked for first: on QEMU q35 with OVMF, 16 KiB at 64 MiB survives a warm reset.
- `kshell_run.py --image dbg --reboot "crash panic-smp"`: ok, and the second boot reported the record.
- `crash panic-locked` on dbg without `--reboot`: ok. The reset ends QEMU after the report.
- `boot_check.py`: dbg PASS (21 passed, 0 error lines), rel PASS (19 passed, 0 error lines). A fresh boot logs `crashlog: no record from the previous boot`.
- clang-format clean; clang-tidy reports nothing in the changed files.
- **Not run:** `crash` on rel. Since commit `ead5d12f` (another session's), the rel image runs the interactive user shell with the kernel shell off, so a kernel command cannot be typed there. The panic path is the same code on both images.
- **Not verified:** that a third boot reports nothing, that is, that the clear sticks. That needs a reset without a panic, and there is no reboot command. `crashlog::clear()` is covered by the host test.

**Deliberately not done**
- No reboot command and no reboot delay, such as Linux's `panic=<seconds>`.
- The tail repeats lines the console already showed, as the console replay does; the record is for a boot that never saw that console.
- The record is not handed to user space yet; that is step 4.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is the cache written back before the reset, and why can no test under QEMU catch it missing?
- Why does the panic path read the reboot option through `try_options()`, and what would an early panic do with `options()` instead?
- Why does the record carry no banner, and why is the recovered record logged at Warn rather than Error?

## Title: the next boot hands the crash record to userboot as a read-only VMO (2026-10-04)

**Files**
- `system/public/ours/process_args.hpp` — `HandleType::VmoCrashlog = 0xe0`
- `kernel/lib/userabi/include/userabi/userboot.hpp`, `kernel/lib/userabi/src/userboot.cpp` — `stash_crashlog()` copies the record into a paged VMO; `write_bootstrap()` adds it as a seventh handle with Basic, Read and Map rights
- `kernel/top/crash_record.cpp` — `recover_record()` stashes the text before it clears the region
- `kernel/lib/userabi/userboot/userboot.cpp` — `report_crashlog()` takes the handle, prints the record's first line, and closes it
- `scripts/kshell_run.py` — `--reboot` also requires userboot's line
- `docs/repairs/261003/kernel-panic.md` — item 5 removed, which empties the file, so it is deleted
- `.agents/tasks/panic-crashlog/` — PLAN steps done, RESULT.md

**Symptom**
None; step 4 of the panic-crashlog feature (decision D3 b). After step 3 the next boot logged the record, but no user program could read it.

**Cause**
The record lived only in the kernel: in the reserved region until it was cleared, and in the boot log as text. A user-space crash reporter needs it as an object it holds.

**Change**
`recover_record()` now calls `userabi::stash_crashlog(text)` before it clears the region. The stash copies the text into a fresh paged VMO, so the region can be cleared at once and the next panic can reuse it. `write_bootstrap()` then hands that VMO to userboot:

```cpp
if (crashlog_vmo.dispatcher()) {
    handles[count] = object::Handle::make(
        object::KernelHandle<object::Dispatcher>(ustl::move(crashlog_vmo)),
        crashlog_rights & CRASHLOG_RIGHTS);   // Basic | Read | Map
    ...
    infos[count++] = handle_info(HandleType::VmoCrashlog, 0);
}
```

The design points:
- **It is a copy, not the region itself.** A VMO over the reserved frames would let user space read memory the next panic writes into. It would also have to be writable or not depending on who asks. Zircon's `crashlog_to_vmo()` copies for the same reasons.
- **Read only.** The record is the kernel's account of the previous boot, so userboot gets Basic, Read and Map, never Write.
- **A type of its own.** Zircon tags this handle `PA_VMO_KERNEL_FILE` (0x1c) and tells kernel files apart by the VMO's name, "crashlog". VMOs have no names here, so the handle gets a type in ours' own range, 0xe0.
- **No lock.** The stash and its one reader are both on the laborer thread: the Platform-level init hook, then `userboot_init()`. They are ordered by program order.

The bootstrap message grows from six handles to seven, in both the arrays and `MESSAGE_BYTES`.

userboot takes the handle if there is one, reads its first 160 bytes, prints the first line, and closes it. Nothing after userboot wants the record yet, so it is not passed on as Zircon's userboot does.

**Reference**
Zircon `crashlog_to_vmo()` and `crashlog_stash()` (`zircon/kernel/lib/userabi/userboot.cc:278`, `zircon/kernel/lib/crashlog/include/lib/crashlog.h`); the handle is pushed into userboot's bootstrap message beside the others.

**Verification**
- `kshell_run.py --image dbg --reboot "crash panic"`: ok. The second boot logged `crashlog: the previous boot panicked; its record (1304 bytes) follows` and then `userboot: the previous boot's crash record: crash: deliberate panic from the kernel shell`.
- `boot_check.py`: dbg PASS (21 passed, 0 error lines), rel PASS (19 passed, 0 error lines). With no record, nothing is passed and nothing is printed.
- clang-format clean; clang-tidy reports nothing in the changed files.

**Deliberately not done**
- userboot does not pass the VMO on to the next program. Nothing reads the record there yet.
- No VMO names, and no general kernel-file handle type.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does userboot get a copy of the record rather than a VMO over the reserved region?
- Why may the stash and `write_bootstrap()` share `crashlog_vmo` without a lock?
- Why is the crash record a handle type of its own here, when Zircon reuses PA_VMO_KERNEL_FILE?
