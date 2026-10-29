# phys-bootshim outstanding repairs

## 1. `DevicetreeTranslator::convert()` accepts an initrd that starts mid-page, and physload then panics on it

**Severity**
Medium: latent under the stock boot, but a loader that starts the initrd mid-page gets past the shim, which promises to refuse what it cannot use, and the boot ends in a physload panic that names only a number.

**Symptom**
Latent, cannot be triggered today by the stock boot (`riscv_boot.py`). It is triggered by any loader that writes a /chosen `linux,initrd-start` that is not a multiple of 4096. QEMU does this for an ELF `-kernel` whose first loadable segment starts mid-page: one at 0x80200800 got `linux,initrd-start = 0x88200800`.

Reproduced by hand with the prebuilt QEMU 11.1.1 as follows. Dump the tree QEMU builds for `-m 256M`, rewrite `linux,initrd-start`/`-end` to 0x88200800/0x8823cf78, boot with `-dtb` on that tree, and put `fake.obi` at 0x88200800 with `-device loader,file=fake.obi,addr=0x88200800,force-raw=on`. The shim prints the map, then `[riscv-bootshim] initrd [0x88200800, 0x8823cf78), 247672 bytes` and `[riscv-bootshim] physload at [0x80060000, 0x800ac000), entry 0x8007aba0`. physload then prints `[error] Invalid OBI package range: 10` and spins in its panic loop. The same run at 0x88200000 reaches `[fake-kernel] handoff ok`.

The shim's contract (`kernel/arch/riscv/phys/bootshim/shim.cpp:12-14`) is to refuse a package it cannot use and say why, but here it never refuses. Instead it loads and relocates physload and calls it, and the line that ends the boot names only a number. 10 is `Status::InvArgs` (`system/public/ours/status.hpp:62`), which `from()` also returns for a zero base or size, so nothing on the console says "alignment".

**Cause**
physload's `BootPackageInput::from()` (`kernel/phys/include/ours/phys/boot_package_input.hpp:40-42`) refuses a package base that is not a multiple of `PAGE_SIZE` with `InvArgs`, and `obi_main()` panics on that (`kernel/phys/physload.cpp:61-64`). The requirement is deliberate. phys reserves the package in whole frames starting at its first byte (`BootPackageInput::reserve()`, `boot_package_input.hpp:61-67`). The kernel then adopts those frames as userboot's boot-image VMO (`make_boot_image_handle()`, `kernel/lib/userabi/src/userboot.cpp:283-305`, through `VmoDispatcher::create_from_wired_pages()`). So the package's first byte must start a frame. The boot-image-pages design chose a panic over a copy for an unaligned base (`.agents/tasks/boot-image-pages/DESIGN.md:49`, D2, 2026-10-03).

On x86 the refusal happens before physload runs: `multiboot::View::module()` refuses a module whose start is not a multiple of `MULTIBOOT_MOD_ALIGN` (`kernel/phys/lib/multiboot/include/multiboot/view.hpp:215`). The riscv path has no equivalent check. `DevicetreeTranslator::read_chosen()` (`kernel/phys/lib/bootshim/include/bootshim/devicetree.hpp:283-305`) takes the start as it is. `initrd_pages()` (329-345) rounds it down to a page (`base & ~mask`, line 344), so `convert()` succeeds and `find_scratch()` steps around the rounded pages. `load_physload()` (`shim.cpp:192-252`) checks only that an initrd exists and parses, then passes `package.base` on.

The rounding also models nothing real. For 0x88200800 the translator keeps [0x88200000, 0x8823d000) clear. Had `from()` let that base through, `reserve()` would have claimed [0x88200800, 0x8823d800). The comment "The pages physload will reserve for the initrd: its range rounded out" (329-330) is therefore true only for an aligned start.

The host test missed this for two reasons. Its model of physload, `Physload::take()` (`kernel/phys/lib/bootshim/test/devicetree_smoke.cpp:295-314`), copies `reserve_package()` but not `from()`'s base check. And no case uses a start that is not page-aligned. So the file's promise that every map is taken "without a refusal -- and so without a panic" (lines 15-17) does not cover this refusal.

Nothing guarantees alignment from the loader. The /chosen properties are an address and an end: Linux's `Documentation/devicetree/usage-model.rst:199-202` says only that, and the dt-schema binding is not on this machine to check. QEMU 11.1.1 puts the initrd at the `-kernel` image's load address plus half of RAM, capped at 512 MiB. Measured at `-m` 100M, 129M, 256M and 2G, that gives 0x83400000, 0x84280000, 0x88200000 and 0xa0200000. These are page-aligned only because this shim loads at 0x80200000.

This item does not depend on any other.

**Change**
Refuse in `DevicetreeTranslator::convert()`. `read_chosen()` or `initrd_pages()` fails when the initrd's start is not a multiple of `page_size`, leaving nothing converted, as every other failure does. Add the failure to `convert()`'s list (`devicetree.hpp:100-110`) and to the class comment (77-84), with the reason: physload takes the package's frames whole, starting at its first byte. `initrd_pages()` then no longer needs to round the start down.

Use a status that no other refusal of `convert()` returns, so that `conversion_failure()` in `shim.cpp` (109-124) can name the cause. `InvArgs` there means "a property is not in the shape its binding gives it", and an unaligned start is in that shape. `BadState` ("a precondition of the operation is not satisfied", `status.hpp:79-81`) is not returned by `convert()` today. A matching message would be "the initrd does not start on a page boundary; physload takes it in whole pages".

Make the test's model honest as well. `Physload::take()` should check the base the way `from()` does, and a new case should convert a tree whose initrd starts at 0x8820'0800 and expect the refusal. Also update the sentence in `BootPackageInput::from()` (`boot_package_input.hpp:36-39`) that calls Multiboot "the one boot path there is".

The translator is the right place for the check:
- It is where this library already keeps physload's rules for the initrd: whole pages, BootPackage, and refusal of overlapping claims. Its `page_size` is documented as "the granule physload reserves the initrd in".
- It is the code that rounds the unaligned start today.
- It is the layer the x86 refusal lives in (a boot-protocol reader).
- It is host-testable, while the shim is not.

Ruled out:
- A check in `load_physload()` alone, before `ContainerView::from()`. It would give the right message, but the translator would still round a start nobody accepts, under a comment saying physload reserves those pages, and the case would stay out of reach of the host test. The shim runs only under QEMU.
- Copying the package to an aligned window from `find_scratch()`. D2 already rejected a copy fallback for exactly this case. It copies the whole image in the shim (1210624 bytes on rel, per that design). And `find_scratch()` keeps no state, so it would hand out the same window for the copy and for physload unless it learned about windows already given out. Neither Linux nor Zircon copies.
- Rounding physload's reservation outward to accept any start, as Linux's `reserve_initrd_mem()` does. The bytes before the package in its first frame would become page 0 of userboot's boot-image VMO. That shifts every OBI item away from where userboot reads it and exposes whatever the loader left there. This is the departure D2 rejected.

**Reference**
Linux accepts any start. `early_init_dt_check_for_initrd()` (`drivers/of/fdt.c:806-834`) checks only that start <= end. `reserve_initrd_mem()` (`init/initramfs.c:616-659`), called from `setup_bootmem()` (`arch/riscv/mm/init.c:283`), rounds the start down and the size up to whole pages (632-634) and `memblock_reserve()`s them. If the range is not memory, or overlaps a reservation, it logs and disables the initrd instead of panicking. Linux can afford any alignment because it reads the initramfs (a cpio archive) in place and gives the pages back afterwards (`free_initrd_mem()`). It never adopts the initrd's frames as an object, and that adoption is what forces alignment here.

Zircon has the same requirement as this project, enforced later and as an assertion. `DevicetreeChosenNodeMatcherBase::OnNode()` (`zircon/kernel/phys/lib/boot-shim/devicetree-chosen-matcher.cc:233-256`) takes `linux,initrd-start` as it is. `InitMemory()` (`zircon/kernel/phys/boot-shim/devicetree.cc:126-131`) records it to the byte as `kDataZbi`. `BootShimHelper::Boot()` (`zircon/kernel/phys/boot-shim/include/phys/boot-shim/devicetree.h:242-247`) hands it to `BootZbi::Init()`, whose `InitZbi()` `ZX_ASSERT`s that it is page-aligned (`zircon/kernel/phys/boot-zbi.cc:503-506`), after memory is already set up. `BootZbi::Load()` realigns only the data items inside an already page-aligned ZBI (`boot-zbi.cc:334-365`: a `ZBI_TYPE_DISCARD` pad, or a copy at 433-436), never the container's start.

On x86 here, the counterpart is `multiboot::View::module()` (`kernel/phys/lib/multiboot/include/multiboot/view.hpp:215`). The recommended fix refuses earlier than Zircon does and says why. It does not change the requirement.

**Acceptance**
1. `out/bringup.x86/host/obj/kernel/phys/lib/bootshim/devicetree-smoke` passes. It includes a case where `convert()` refuses a tree whose `linux,initrd-start` is 0x8820'0800, with the new status, and `Physload::take()` checks the base the way `BootPackageInput::from()` does.
2. The hand reproduction stops in the shim, with the shim's own line naming the page-boundary requirement, and physload prints nothing. To reproduce: dump the tree with `-machine virt,dumpdtb=<file>` under the same `-m 256M -kernel bootshim.bin -initrd fake.obi`. Rewrite /chosen's `linux,initrd-start`/`-end` in place to 0x88200800/0x8823cf78 (both are 8-byte cells). Then boot `qemu-system-riscv64 -machine virt -m 256M -smp 1 -nographic -bios default -kernel bootshim.bin -dtb <patched> -device loader,file=fake.obi,addr=0x88200800,force-raw=on`.
3. riscv `ours-verify` on `out/bringup.riscv` still reaches `[fake-kernel] handoff ok`.
4. x86 `ours-verify` passes.
