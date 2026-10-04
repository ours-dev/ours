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
