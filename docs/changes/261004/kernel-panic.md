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
