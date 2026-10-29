# phys outstanding repairs

## 1. physload reserves its own image to the byte, so the rest of its last page stays Free

**Severity**
Medium: on both architectures, every boot, the memory map physload builds and physboot hands on describes one frame as part image and part free memory; nothing allocates from that part today.

**Symptom**
Latent, cannot be triggered today. `MemoryProvisioner::reserve_boot_image()` (`kernel/phys/memory.cpp`) reserves `[__executable_start, __executable_end)` as `PhysImage`, and `__executable_end` is not a multiple of a page: riscv physload's is 0x4b268, x86's 0x482d0 (`llvm-nm` on `obj/kernel/phys/physload` in each build). On riscv the shim places physload at 0x80060000, so BootMem holds `PhysImage` over [0x80060000, 0x800ab268) and calls everything from 0x800ab268 up to the shim Free: frame 0x800ab000 belongs to both. physboot's `snapshot_memory_map()` passes that Free run to the kernel starting mid-frame. It would do harm if a phys allocation with less than page alignment fitted in the tail and were picked (every phys allocation asks for page alignment and goes top-down today), or if a reader of the handoff map took a Free run's first partial frame as free (the kernel's `free_frames_phys_range()`, which `EarlyMem::do_handoff()` gives every free run, rounds the run's start up, and `stamp_reserved_frames()` rounds a reservation's end up, so today the kernel settles that frame as reserved).

**Cause**
`BootMem::reserve()` is byte-granular: it records exactly the range it is given and rounds nothing (`kernel/phys/lib/bootmem/include/bootmem/physical_memory.hpp`). From the handoff on, memory is owned frame by frame and a frame has one owner. That rule is why `BootPackageInput::reserve()` rounds the package out to whole pages and why `snapshot_memory_map()` allocates its buffer in whole pages; the latter's comment records the bug a sub-page tail sharing a frame with free memory already caused. `reserve_boot_image()` predates the rule being applied to it, and `physload.ld` ends the image at `__executable_end` right after `.bss`, aligned to 8. Both of physload's loaders, the x86 and the riscv boot shims, already give it whole pages at a page boundary (`ustl::round_up(image.size(), PAGE_SIZE)`), so the slack is no one else's claim; only the reservation leaves it out. The riscv shim's comment and the bootshim host test had claimed physload reserves whole pages; both now say it does not (`docs/changes/261028/riscv-boot.md`).

**Change**
In `reserve_boot_image()`, check that `__executable_start` is page-aligned and reserve `[__executable_start, round_up(__executable_end, PAGE_SIZE))`, as `BootPackageInput::reserve()` does for the package. With it: `opensbi_runtime_tree()` in `kernel/phys/lib/bootshim/test/devicetree_smoke.cpp` reserves the whole window (`reserve_image(scratch.value(), WINDOW)`), and the riscv shim's placement comment in `load_physload()` drops its last sentence. Any future loader of physload must give it whole pages too, or the rounded reservation can run into a claim of another type, which BootMem refuses and physload panics on. Ruled out: `. = ALIGN(4096)` before `__executable_end` in `physload.ld`, which gives the same reservation but spells the page size as a number in a script shared by both architectures, where the C++ has `PAGE_SIZE`.

**Reference**
Zircon's `ZbiInitMemory()` (`kernel/phys/zbi-init-memory.cc`) reserves `[PHYS_LOAD_ADDRESS, _end)` as `memalloc::Type::kPhysKernel` to the byte too, with `_end` aligned to 16 (`kernel/phys/phys.ld`); the whole-page reservation is this project's own design, from the frame-ownership rule `BootPackageInput::reserve()` already follows.

**Acceptance**
A host test reserves physload's rounded window and finds it `PhysImage` throughout; the riscv check still reaches `handoff ok`; x86 `ours-verify` passes and the x86 fake image still reaches `[fake-kernel] handoff ok`.

## 2. physboot's handoff log line calls the page-table root `cr3` on every architecture

**Severity**
Low: a log line names x86's register on riscv; nothing reads it but a person.

**Symptom**
The riscv boot prints `[info] handoff: jumping to kernel at 0xFFFFFFD040002000, cr3=0x8FFFF000`. riscv has no CR3; the value is the Sv39 root that `satp` points at.

**Cause**
`physboot_init()` (`kernel/phys/physboot.cpp`) is architecture-neutral, and its last log line was written when x86 was the only architecture: `log::info("handoff: jumping to kernel at 0x{:X}, cr3=0x{:X}", ...)` with `global_aspace()->pgd()`. The value is right on both; only its name belongs to one. A reader of a riscv log who does not know the line's history looks for a register that does not exist.

**Change**
Name the value by what it is in this codebase's terms, the aspace's root, for example `root=0x{:X}`, or the same word the `Aspace::pgd()` accessor uses. Ruled out: an architecture hook that prints `cr3` or `satp`, which buys nothing for one log line.

**Reference**
None, this project's own design: a log line.

**Acceptance**
The riscv and x86 fake-image boots print the handoff line without `cr3`; x86 `ours-verify` passes; the riscv check still reaches `handoff ok`.
