# phys outstanding repairs

## 1. physload's identity map and physboot's physmap both map the firmware's `no-map` reserved memory, because `DevicetreeTranslator` drops the `no-map` property

**Severity**
Medium: on riscv, every boot, the root the kernel adopts maps OpenSBI's `no-map` memory twice, but it does harm only under one of the three conditions in Symptom, and the QEMU boot meets none of them.

**Symptom**
This is visible today and harmless under QEMU. The riscv boot log prints `[debug] Map identically (0x80000000, 0x10000000)` (`docs/changes/261028/riscv-boot.md`, the log in the 5c9de143 entry). That range starts on OpenSBI's two `/reserved-memory` regions, `mmode_resv1@80000000` [0x80000000, 0x80040000) and `mmode_resv0@80040000` [0x80040000, 0x80060000). Both are marked `no-map` in the tree OpenSBI hands the shim, as decoded from the checked-in copy `kernel/phys/lib/bootshim/test/data/opensbi_virt.dtb`.

The root the kernel adopts maps that memory twice. Booting `out/bringup.riscv` as `riscv_boot.py` does, with a QEMU monitor attached, `info mem` after `[fake-kernel] handoff ok` prints two lines that cover it: `0000000080000000 0000000080000000 0000000010000000 rwx-gad` is physload's identity map, and `ffffffc000000000 0000000000000000 0000001000000000 rw--gad` is physboot's physmap, which puts the same bytes at 0xffffffc080000000.

On this platform an ordinary access still traps. The live PMP (`pmpcfg0` 0x1f1b1b181b1b1818, `pmpaddr0` 0x20013fff, `pmpaddr1` 0x20007fff) has two NAPOT entries with no permissions over exactly those ranges. So an S-mode access through either mapping traps as an access fault rather than a page fault, and OpenSBI's memory is not reached.

The harm is latent and needs one of three conditions:
- A core that speculates or prefetches through a valid cacheable mapping. This is what the devicetree reserved-memory binding's `no-map` forbids.
- Code that walks every mapped page. Linux's riscv hibernation did this once its linear map covered OpenSBI's region, and it panicked on the PMP fault (Linux ed309ce52218).
- A firmware whose `no-map` region is not also fenced by PMP. There a stray store through either mapping lands silently.

QEMU's TCG neither speculates nor prefetches, and nothing in phys or the fake kernel touches the range, so no boot here can show it.

**Cause**
The `no-map` property is lost in the shim. `DevicetreeTranslator::add_reserved_memory()` (`kernel/phys/lib/bootshim/include/bootshim/devicetree.hpp:419-445`) passes every `reg` entry of every `/reserved-memory` child to `add_owned()`, which appends it as `obi::MemoryRangeType::Reserved` (:461). Nothing in the header reads `no-map`. So two different kinds of region leave the shim as the same type: one the OS may map but must not allocate, and one it must not map at all.

physload then maps all of RAM. `register_range()` (`kernel/phys/memory.cpp:52-53`) records Reserved as a `bootmem::MemoryType::FirmwareReserved` reservation, which BootMem keeps beside the usable RAM rather than cutting it out. `Aspace::map_usable_ram()` (`kernel/phys/aspace.cpp:97-139`) walks `MemoryView::Usable`, which visits RAM and ignores every reservation (`kernel/phys/lib/bootmem/include/bootmem/physical_memory.hpp:444`). It maps each run with `MmuFlags::PermMask | MmuFlags::Present` (:102), so the whole memory node, firmware included, becomes read, write and execute.

physboot maps the firmware again, without looking at the memory map at all. `build_kernel_aspace()` (`kernel/phys/physboot.cpp:145-151`) maps the physmap as one range, physical [0, ARCH_PHYSMAP_SIZE), which is 64 GiB on riscv, read-write. It goes into the root the kernel adopts and records as `k:physmap` (`kernel/mm/init_vmm.cpp:67-76`). So fixing the identity map alone still leaves a read-write alias of the firmware at KERNEL_ASPACE_BASE + 0x80000000. Once a range reaches physload, nothing says it must stay unmapped.

The fix cannot be to leave every FirmwareReserved range unmapped, because other ranges of that type must stay mapped:
- The riscv shim passes its own image and the device tree blob as Reserved too (`kernel/arch/riscv/phys/bootshim/shim.cpp:360-366`). physload runs as a nested call on the shim's stack, inside that image (:184, :372), and the kernel walks the tree itself.
- On x86, every non-Available Multiboot2 entry becomes FirmwareReserved (`kernel/phys/lib/multiboot/include/multiboot/memory_map.hpp:66-70`). That includes the ACPI tables, which the kernel reads through the physmap (`kernel/platform/pc/acpi.cpp:22-30`).

The type's own comment, "The firmware said this is not RAM" (`kernel/phys/lib/bootmem/include/bootmem/types.hpp:71`), is already inaccurate for these uses.

The gap was known when the riscv arch layer was written. It is recorded under "Deliberately not done" in `docs/changes/261028/riscv-phys.md:277`, for the identity map only, but no repair item tracked it. The code first ran on riscv with 5c9de143, when the shim began calling physload. This item does not depend on any other.

**Change**
Carry `no-map` as its own range type from the shim to every map built from it:

1. obi. `obi::MemoryRangeType` (`kernel/lib/obi/include/obi/package_format.hpp:62-66`) gains a value for firmware memory that must not be mapped. `MemoryConfigView::range()` (`kernel/lib/obi/include/obi/memory_config_view.hpp:48-51`) must accept it, since it currently rejects any type above Reserved.
2. The translator. `add_reserved_memory()` reads each child's `no-map` property and appends that child's ranges with the new type; children without it stay Reserved.
   - `find_scratch()` already keeps off every range that is not Ram, so where the shim places physload does not change.
   - Update the class comment's list of what becomes what (devicetree.hpp:51-71).
   - A `no-map` region that overlaps a reservation-block entry or a caller's range would put two BootMem types on the same bytes. BootMem refuses that, and physload panics on it. The translator should either refuse it (AlreadyExists, as it does for an overlap with the initrd) or cut the plain range around it.
3. bootmem. `bootmem::MemoryType` gains a matching type, with a `to_string()` case. `register_range()` reserves the new obi type under it. The kernel needs nothing: every type other than Free is a used frame (`MemRange::used()`), so those frames stay reserved, as Linux keeps nomap memory in its memory map as reserved pages.
4. physload's identity map. `map_usable_ram()` maps each usable run minus the reservations of the new type, which `visit_reservations()` yields in address order. Round each hole outward to whole pages, so that no page holding a no-map byte is ever mapped.
5. physboot's physmap. It maps [0, ARCH_PHYSMAP_SIZE) as the pieces between those same reservations. One helper, "for each run of [lo, hi) clear of no-map memory", can serve both maps.

The cost is in page-table size. The paging engine picks the largest leaf each step allows (`kernel/lib/arch/include/arch/paging/controls.hpp:31-32`), and riscv's physmap is 1 GiB leaves today (`docs/changes/261028/riscv-phys.md`, the layout entry). With the hole, the 1 GiB leaf over [0x80000000, 0xC0000000) becomes 2 MiB leaves plus 4 KiB ones for [0x80060000, 0x80200000). That costs two more page tables and less TLB reach in that gigabyte. This is the same trade-off as in Linux's history: its riscv linear map moved to 1 GiB pages relying on PMP alone (3335068f8721), and OpenSBI releases that dropped `no-map` so Linux could use those pages broke hibernation (ed309ce52218).

While there, fix the comments that say what the maps hold: FirmwareReserved's comment and the riscv `arch_setup_aspace()` comment ("identity-mapping RAM, and nothing else").

Whether to go further and, like Zircon, keep every `/reserved-memory` region unmapped, `no-map` or not, is a strategy question for the user. It is not part of this item.

Options ruled out:
- A flag in `obi::MemoryRange::reserved`. The reader rejects any nonzero value there (`memory_config_view.hpp:47`). bootmem's own comment on `MemoryType` (types.hpp:57) also chose one enum over a bool plus a purpose, because two fields can disagree.
- Leaving every FirmwareReserved range unmapped. That unmaps the shim stack physload is running on the moment `satp` is written, the device tree, and on x86 the ACPI tables (see Cause).
- Cutting `no-map` regions out of the Ram ranges in the translator, which is the result Zircon gets. It fixes the identity map with a change to one header. But physboot's physmap would then have no way to tell the hole from the MMIO gaps it maps on purpose, and the frames would stop being RAM to the kernel.
- A RAM-only physmap, as Zircon's is. It breaks x86's ACPI access through the physmap, and it is a separate design change.
- Fixing only `map_usable_ram()`. That leaves the physmap's read-write alias.

**Reference**
Linux honours `no-map`:
- `__reserved_mem_reserve_reg()` (`drivers/of/of_reserved_mem.c:143-179`) reads the property (:160).
- `early_init_dt_reserve_memory()` (:121-136) then calls `memblock_mark_nomap()` instead of `memblock_reserve()`.
- Both run under `early_init_fdt_scan_reserved_mem()` (`drivers/of/fdt.c:495`), which riscv's `setup_bootmem()` calls (`arch/riscv/mm/init.c:290`).
- riscv's `create_linear_mapping_page_table()` (`arch/riscv/mm/init.c:1202-1256`) maps only `for_each_mem_range()` (`include/linux/memblock.h:247-250`). Its flags leave out MEMBLOCK_NOMAP, so `should_skip_region()` (`mm/memblock.c:1231-1233`) skips nomap memory. The same function uses nomap to keep the kernel's text and rodata out of huge leaves (:1215-1216).
- `memblock_mark_nomap()`'s comment (`mm/memblock.c:1107-1116`) gives the semantics this item gives the kernel: not in the direct map, still covered by the memory map, its pages reserved.
- History: 3335068f8721 moved riscv's linear map to 1 GiB leaves, relying on firmware PMP. ed309ce52218 marked riscv hibernation NONPORTABLE because OpenSBI versions without `no-map` let the linear map cover the PMP-protected region.

Zircon reaches the same result differently, without reading the flag:
- `DevicetreeMemoryMatcher::OnNode()` (`kernel/phys/lib/boot-shim/devicetree-memory-matcher.cc:31-50`, with `HandleReservedMemoryNode()` at :86-97) records every `/reserved-memory` child except a ramoops region (kept as NVRAM, :35-44) as `memalloc::Type::kReserved`, and never reads `no-map`. That is the same flattening as ours.
- But `memalloc::Pool::Init()` treats kReserved as a hole in RAM (`kernel/phys/lib/memalloc/pool.cc:124-127`; the precedence rule is in `include/lib/memalloc/range.h:32-35`).
- Both maps then cover RAM only. Phys's identity map (`AddressSpace::IdentityMapRam()`, `kernel/phys/address-space.cc:47-88`, reached through `SetUpIdentityMappings()` from riscv64's `ArchSetUpAddressSpace()`, `kernel/arch/riscv64/phys/address-space.cc:18`) and the kernel's physmap (`HandoffPrep::ConstructKernelAddressSpace()`, `kernel/phys/handoff-prep-vm.cc:238-262`) both map only `memalloc::NormalizeRam()`.
- So Zircon maps no reserved region at all and never needs the flag.

x86 here has no counterpart. Multiboot2's map has no no-map attribute, and its non-Available entries already lie outside RAM.

**Acceptance**
1. Host test. In `kernel/phys/lib/bootshim/test/devicetree_smoke.cpp`, `opensbi_runtime_tree()` (:399) expects both firmware ranges with the new type (today `is_reserved` at :410-411). It also expects BootMem to hold [0x80000000, 0x80060000) under the new bootmem type (today FirmwareReserved at :424). A hand-built tree gets one `/reserved-memory` child without `no-map`, which must stay Reserved; the `reserved_memory()` helper (:214-226) marks every child `no-map` today, so it needs a parameter for this. Host tests pass.
2. riscv boot. The riscv check (`.agents/skills/ours-verify/scripts/boot_check.py --out out/bringup.riscv`) still reaches `[fake-kernel] handoff ok`. Its log prints `Map identically (0x80060000, 0xFFA0000)` instead of `Map identically (0x80000000, 0x10000000)`.
3. Live mappings. Boot `out/bringup.riscv` as `riscv_boot.py` does, adding `-monitor tcp:127.0.0.1:<port>,server,nowait`. After `[fake-kernel] handoff ok`, run `stop` and then `info mem`. No line may map a physical address in [0x80000000, 0x80060000): the identity map starts at 0x80060000, and the physmap has a gap at [0xffffffc080000000, 0xffffffc080060000). Today the output instead has `0000000080000000 0000000080000000 0000000010000000 rwx-gad` and `ffffffc000000000 0000000000000000 0000001000000000 rw--gad`.
4. x86. x86 `ours-verify` passes and the x86 fake image still reaches `[fake-kernel] handoff ok`; nothing on x86 produces the new type.

## 2. Every phys loader places an ELF image at page alignment whatever its PT_LOAD p_align, so `ElfImage::load_at()` refuses an image aligned above a page

**Severity**
Medium: latent; the first PT_LOAD whose p_align exceeds a page, from an over-aligned object or an aarch64 port, stops the boot at whichever loader places that image, and the kernel's virtual base can break that alignment without a word.

**Symptom**
Latent, cannot be triggered today. Every image phys loads has p_align 0x1000. `llvm-readelf -l` shows Align 0x1000 on every PT_LOAD of `kernel.phys_<arch>.phys.dbg_0/obj/kernel/phys/physload`, `kernel.phys_<arch>.phys.dbg_0/obj/kernel/phys/physboot.module` and `kernel_<arch>.dbg.0/obj/kernel/tests/fake-kernel/fake-kernel.image.elf`, in both `out/bringup.riscv` (`<arch>` riscv) and `out/bringup.x86` (`<arch>` x86). physload's largest section alignment is 16, and no BUILD.gn passes `-z max-page-size`, so p_align is LLD's default max page size, 4 KiB on riscv64 and x86-64.

It triggers as soon as one PT_LOAD of physload, physboot or the kernel asks for more than a page. That happens in either of two cases:
- One object is declared with `alignas` above 4 KiB. Linking a scratch `_Alignas(0x200000) char big[16];` with `kernel/phys/physload.ld` gives its RW PT_LOAD Align 0x200000 on both riscv64 and x86-64.
- The tree is ported to aarch64, where the same script gives every PT_LOAD Align 0x10000 with no over-aligned object.

The loader of that image then picks a merely page-aligned base, and `ElfImage::load_at()` refuses it with `Status::InvArgs` unless the base happens to be a multiple of the alignment. The boot stops at whichever loader that is:
- the riscv shim prints "physload could not be loaded";
- the x86 shim panics "Failed to load physload";
- physload panics "Failed to load phys module";
- physboot panics "Failed to load the kernel image at ...".

Today's riscv placement, 0x80060000 (`.agents/tasks/riscv-boot-chain/PLAN.md`, step 5), is a multiple of 128 KiB but not of 256 KiB, so a 2 MiB p_align would be refused there.

The kernel also has a silent half. Its virtual base is `KERNEL_LOAD_BASE` plus a KASLR bias rounded only to a page, and `relocate()` checks no alignment. So an over-aligned kernel object whose physical placement happened to pass would run at a virtual address that breaks its alignment.

**Cause**
`load_at()` refuses a base unless, for every PT_LOAD, `(phys_addr + image_offset) % p_align == p_vaddr % p_align` (`kernel/phys/elf_image.cpp:123-126`). The load plan sets `image_offset = p_vaddr - image_base`, where `image_base` is the lowest PT_LOAD p_vaddr (`system/lib/eftl/include/eftl/elf/load_policy.hpp:225-227, 258`). So the check is `phys_addr ≡ image_base (mod p_align)` for every segment, that is, modulo the largest p_align. `image_base` is 0 in every image here, so the base must be a multiple of the largest p_align.

The check itself is right. ELF only promises `p_offset ≡ p_vaddr (mod p_align)`, not that p_vaddr is a multiple of p_align: the scratch link above puts its segment at p_vaddr 0x2000 with p_align 0x200000. A base that breaks the congruence moves every object the alignment was there for.

What is missing is the placing side. The plan keeps each segment's p_align (`load_policy.hpp:387, 423`), but `ElfImage` exposes no alignment, so every caller asks for `PAGE_SIZE`:
- the riscv shim's `translator.find_scratch(size, PAGE_SIZE)` (`kernel/arch/riscv/phys/bootshim/shim.cpp:227`);
- the x86 shim's `LegacyMemoryTranslator::find_scratch(..., total, PAGE_SIZE)` (`kernel/arch/x86/phys/bootshim/shim.cpp:96-97`);
- `ModuleLoader::load()`'s `allocate_top_down(total, PAGE_SIZE, PhysImage, ...)` (`kernel/phys/module_loader.cpp:123`);
- physboot's `load_kernel_image()` and its `allocate_top_down(image_pages, PAGE_SIZE, KernelImage, ...)` (`kernel/phys/physboot.cpp:59`).

On the kernel's virtual side, `gen_load_bias()` rounds the bias down to `PAGE_SIZE` (`physboot.cpp:47`), and `relocate()` (`elf_image.cpp:158-166`) checks no congruence at all.

Both `find_scratch()` functions and `allocate_top_down()` already honour any power-of-two alignment they are given:
- `DevicetreeTranslator::find_scratch()` steps by `align_up()` (`kernel/phys/lib/bootshim/include/bootshim/devicetree.hpp:172, 184`), and `devicetree_smoke.cpp:914` tests a 2 MiB alignment;
- `LegacyMemoryTranslator::find_scratch()` rounds up (`legacy_memory.hpp:129-130`).

So only the alignment fed to them is wrong. This item does not depend on any other.

**Change**
Recommended: compute the image's load alignment once, in the plan, and have every placer ask for it.
1. `KernelElfLoadPolicy::make_plan()` (`load_policy.hpp`) records the largest PT_LOAD p_align as `LoadPlan::max_alignment()`, at least `MappingPageSize`. It also refuses, with `LoadError::InvalidAlignment`, an image whose `image_base` is not a multiple of that alignment. Then "a base that is a multiple of `max_alignment()`" is the whole contract, and no caller needs to know about `image_base`.
2. `ElfImage` gains an inline `load_alignment()` beside `size()` (`kernel/phys/include/ours/phys/elf_image.hpp:86-89`). It is inline so that physboot, a physload_module that binds `ElfImage`'s out-of-line methods through physload's export table, needs no new `PHYS_EXPORT`.
3. The four placers pass it: `kernel/arch/riscv/phys/bootshim/shim.cpp:227`, `kernel/arch/x86/phys/bootshim/shim.cpp:96-97`, `kernel/phys/module_loader.cpp:123` and `kernel/phys/physboot.cpp:59`. The size stays rounded to `PAGE_SIZE`, because only the base needs the larger alignment.
4. physboot's `gen_load_bias()` rounds to `load_alignment()` instead of `PAGE_SIZE`. `relocate()` refuses a `load_address` that is not congruent to `image_base` modulo it, the same check `load_at()` makes on the physical side, so the kernel's virtual placement cannot break the alignment silently.

Ruled out:
- `load_at()` rounding the base up itself: it does not own the window, and the caller sized that window to the byte from `size()`.
- Weakening `load_at()`'s check to page granularity: this is effectively what Zircon's loader does, and it would load an over-aligned image with its objects misaligned without a word.
- A per-architecture constant, like Zircon's `kArchZbiKernelAlignment` or Linux's `kernel_alignment`: the next image here is an ELF that states its own requirement, and a constant can disagree with it.
- As the only fix, a link-time check in `physload.ld`, the module script and `kernel.ld` (`ASSERT` on `CONSTANT(MAXPAGESIZE)` and on each output section's `ALIGNOF`): it is cheaper, but it turns a legitimate `alignas` into a link error and would force `-z max-page-size=4096` on an aarch64 port. It remains the fallback if forbidding over-alignment is preferred to honouring it.

**Reference**
Zircon has no counterpart that honours p_align, and it behaves differently:
- `ElfImage::Load()` (`kernel/phys/elf-image.cc:164-242`) allocates the image with `Allocation::New(ac, type, load_info_.vaddr_size(), AddressSpace::kPageSize)` (line 190). That is page alignment whatever the segments ask for, with no congruence check, so Zircon would load an over-aligned image without complaint.
- elfldltl's PT_LOAD observer refuses only a p_align below the page size (`src/lib/elfldltl/include/lib/elfldltl/phdr.h:479-482`).
- physboot loads the kernel with `kernel.Load(memalloc::Type::kKernel, kHandoffVirtualAddress)` (`kernel/phys/physboot.cc:96`), at a fixed virtual address.
- Where Zircon's shims place the next stage, a ZBI kernel, the alignment is a per-architecture protocol constant, `kArchZbiKernelAlignment` (`kernel/phys/include/phys/zbi.h:96`). It is 4 KiB on x86 and riscv64 and 64 KiB on arm64 (`kernel/arch/*/phys/include/phys/arch/zbi.h:19`), and `BootZbi::Load()` honours it: `BootZbi::KernelCanLoadInPlace()` checks it (`kernel/phys/boot-zbi.cc:192-196`), and otherwise `Load()` allocates the kernel at that alignment (`boot-zbi.cc:418-423`).

Linux's ELF loader is the counterpart of the recommended fix. `maximum_alignment()` (`fs/binfmt_elf.c:491`) takes the largest power-of-two PT_LOAD p_align, at least a page, and `load_elf_binary()` masks an ET_DYN load bias with it (`fs/binfmt_elf.c:1107, 1144-1145, 1171-1172`).

For the kernel image itself, Linux, like Zircon, uses a protocol value instead of p_align:
- x86 publishes `kernel_alignment = CONFIG_PHYSICAL_ALIGN` in the setup header (`arch/x86/boot/header.S:332`; `Documentation/arch/x86/boot.rst`, field `kernel_alignment`). The decompressor rounds its load address up to it (`arch/x86/boot/compressed/head_64.S:147-153`), and KASLR picks slots in its units (`arch/x86/boot/compressed/kaslr.c:544, 855`).
- riscv expects a PMD boundary, 2 MiB on rv64 (`Documentation/arch/riscv/boot.rst:46-48`). The EFI stub honours it through `efi_get_kimg_min_align()` (`arch/riscv/include/asm/efi.h:32-39`), used by `drivers/firmware/efi/libstub/kaslr.c:111`.

**Acceptance**
A host test in `system/lib/eftl/test/load_policy_test.cpp` covers three cases:
- a plan with PT_LOADs of p_align 0x1000 and 0x200000 reports `max_alignment()` 0x200000;
- a plan of 4 KiB segments reports the mapping page size;
- a plan whose lowest p_vaddr is not a multiple of its largest p_align is refused with `LoadError::InvalidAlignment`.

An experiment, not committed: give physload and the fake kernel one `alignas(0x200000)` global each.
- Before the fix, the riscv check stops at "physload could not be loaded", unless the first fit happens to land on a 2 MiB boundary.
- After the fix, the riscv shim prints a 2 MiB-aligned `[riscv-bootshim] physload at` base and the riscv check still reaches `handoff ok`. x86 `ours-verify` passes, and the x86 fake image reaches `[fake-kernel] handoff ok`, with the `boot-shim: physload at` address a multiple of 0x200000.

Without the experiment, the riscv check and x86 `ours-verify` both pass unchanged.

## 3. `ElfImage::load_at()`'s fence comment gives lld's `-z text` credit for a guarantee `MappedMemory` enforces, misdescribes lld's check, and never names the writable-and-executable segment that would need a second sync

**Severity**
Low: the fence is placed correctly; its comment, and the commit message and change record it came from, explain it with the wrong mechanism and leave out the one case that would need a second sync.

**Symptom**
Latent, cannot be triggered today. The fence itself is placed correctly. The comment above `arch::sync_instruction_fetch_range()` in `ElfImage::load_at()` (`kernel/phys/elf_image.cpp:145-151`) explains why with the wrong mechanism.

The comment says relocations after `load_at()` cannot reach code because "the linker refuses a dynamic relocation against read-only memory unless told -z notext". Read literally, this leads to two wrong conclusions. The first is that `-z notext` would let a relocation silently rewrite code that has already been fenced. It would not: the loader refuses the write and `relocate()` fails (see Cause). The second is that only link flags stand between a relocation and the code. The one case that does need another sync after relocation is a load segment that is both writable and executable, and the comment never names it. Nothing checks for that case either: `KernelElfLoadPolicy::make_segment()` copies `p_flags` unchanged into the region permissions (`system/lib/eftl/include/eftl/elf/load_policy.hpp:412-424`). If such a segment appeared and a relocation rewrote an instruction in it, the failure would show only on hardware whose instruction fetch does not see the CPU's own stores, as a fetch of the stale bytes.

No image phys loads today has such a segment. `prebuilt/clang/llvm-23/bin/llvm-readelf -lW` finds no `RWE` PT_LOAD in riscv or x86 `physload`, `physboot.module` and `fake-kernel.image.elf`, nor in x86 `vmours.image.elf` and `vmours.with-tests.image.elf` (the riscv real kernel is not built). None of these images has a `TEXTREL` tag, and no generated ninja file in `out/bringup.riscv` or `out/bringup.x86` passes `-z notext` or `--omagic`.

The same wrong explanation is in commit bccdca9e's message and in its change record: `docs/changes/261028/riscv-phys.md`, entry "ElfImage::load_at() ends by making the loaded image visible to instruction fetch", fourth Cause paragraph. That record's first "Three questions" item asks what change to the build would make the placement unsafe, and the record's own Cause paragraph points the reader at `-z notext`, which is the wrong answer.

**Cause**
Two different checks keep a relocation away from the image's code. The comment names the one the placement does not depend on, and describes it wrongly.

The check the placement depends on is the loader's own. Every store that `relocate()` and `relocate_and_bind()` make goes through `eftl::elf::MappedMemory::write()` (`system/lib/eftl/include/eftl/elf/mapped_memory.hpp:56-60`), which calls `translate(address, sizeof(T), true)`. With `requires_write` set, `translate()` returns `INVALID_OFFSET` for any address whose `LoadRegion` lacks `SegPerms::Writable` (lines 106-109). A region's permissions are its PT_LOAD's `p_flags` (`load_policy.hpp:418`, `details/segment.hpp:84-86`).

Both functions reach that check:
- `relocate()` goes through `apply_kernel_relative_relocations()` (`kernel/phys/elf_image.cpp:173`), then `eftl::elf::relocate_relative()`, then `apply_relative_record()`, then `memory.write()` (`system/lib/eftl/include/eftl/elf/link.hpp:47`).
- `relocate_and_bind()` calls `relocate()` first (`elf_image.cpp:314`). `SymbolicRelocationPass::bind()` then writes through its own `MappedMemory` (`elf_image.cpp:273-274`).
- `decode_dynamic()` only reads.

A refused write makes `relocate()` return `InvArgs` and `bind()` return `OutOfRange`. Every caller then stops: the riscv shim refuses, the x86 shim and physboot panic, and `ModuleLoader` fails the load before running anything. So whatever the link flags, a relocation can change bytes only in a PF_W segment. Every link here puts code in PF_X segments, so those bytes can be code only if the segment is also PF_X. `test_rejects_writes_to_readonly_regions` (`system/lib/eftl/test/mapped_memory_test.cpp:178`) tests the refusal for an R+X region.

lld's `-z text`, its default, is only why a well-formed image never hits that refusal. It checks where a relocation writes, not what it refers to. In ELF usage, "a relocation against X" names the symbol X, but lld's check is on the section that holds `r_offset`. The tree's `ld.lld -pie` on riscv64 shows this:
- A word in `.text` pointing at writable data is refused, and so is a word in `.rodata` pointing at code. The error is "relocation R_RISCV_64 cannot be used against symbol ...", reported at `.text+0x8` and `.rodata+0x0`.
- Words in `.data` pointing at code and at `.rodata` link cleanly as two `R_RISCV_RELATIVE` relocations. Those are exactly the comment's "dynamic relocation against read-only memory", and lld accepts them.

With `-z notext`, the `.text` word is emitted as a `TEXTREL` relocation, but `.text` stays in an `R E` segment. `MappedMemory` would refuse that write, so `-z notext` produces a failed relocation, not a stale fetch. `physload.ld` and `phys.ld` fix `:code` at `FLAGS(5)` (R|X), and `physload.ld`'s `.phys.exports` comment already states the loader-side rule ("A relocation target has to sit in a writable segment for the fixup to land").

What would make the fence too early is a PT_LOAD flagged both PF_W and PF_X: a `PHDRS` entry with `FLAGS(7)`, or lld's `--omagic`, which emits a single `RWE` segment. `MappedMemory` allows a relocation write into that segment. lld's per-section check does not look at segments. If the write lands on an instruction, `load_at()`'s fence has already run.

The riscv handoff's comment (`kernel/arch/riscv/phys/handoff.cpp:31-35`) states the loader-side fact correctly ("the relocations physboot applied after it wrote only the kernel's writable segments") and does not repeat the `-z notext` claim. It has the same gap: writable means "not code" only while no segment is also executable.

This item does not depend on any other.

**Change**
Rewrite the comment in `load_at()` so it names three things: the loader's refusal as the guarantee, `-z text` as why a well-formed image never hits that refusal, and the writable-and-executable segment as the exception. Suggested text:

```cpp
// The image's code was written with ordinary stores (the zero fill, the
// segment copies), and a CPU may not fetch what it stored until it is told
// to, so this is where every image phys loads becomes safe to jump into,
// whoever loaded it. relocate() and relocate_and_bind() store into the image
// later, but only through MappedMemory, which refuses any store outside a
// segment flagged writable; lld's default -z text refuses a dynamic
// relocation that would write into a section not marked writable, so a
// well-formed image never meets that refusal. So no instruction they could
// change is left, unless a segment is both writable and executable: an image
// with one must sync again after relocating.
```

In `kernel/arch/riscv/phys/handoff.cpp`, extend "the relocations physboot applied after it wrote only the kernel's writable segments" to say that none of those segments is also executable, so both comments state the same condition.

Change records are append-only, so the entry in `docs/changes/261028/riscv-phys.md` is not edited. The fixing change's own record corrects that entry's reasoning and answers its first "Three questions" item correctly: `-z notext` makes `relocate()` fail, while a writable and executable segment, or code patching, makes the fence too early.

Ruled out:
- Calling `sync_instruction_fetch_range()` again at the end of `relocate()` and `relocate_and_bind()`. It costs only one `fence.i`, but it splits ownership of "safe to jump into" across functions again, which bccdca9e removed. It also guards against a segment that no image has.
- Making `KernelElfLoadPolicy::make_segment()` refuse a PT_LOAD with both PF_W and PF_X. That would turn the last assumption into a check, just as `MappedMemory` already checks writes. But it changes eftl's behaviour for every eftl user, and Zircon's loader has no such check. That makes it the user's decision and a separate item, not part of a comment fix.
- Keeping `-z notext` as the reason given. It is not what stops a text relocation in this loader, and it points the reader at the linker for a guarantee the loader already gives.

**Reference**
Zircon places the sync the same way, so the placement is right and only the comment is wrong:
- `ElfImage::Load()` (`zircon/kernel/phys/elf-image.cc:233-239`) ends with an `arch::GlobalCacheConsistencyContext` and `SyncRange()` over the whole image, before `ElfImage::Relocate()` runs.
- `ElfImage::Relocate()` (lines 244-268) issues only `ktl::atomic_signal_fence()` after `elfldltl::RelocateRelative()`.
- Load's comment states the condition in terms of executable segments ("Later relocation won't touch executable segments"), which is the condition this item wants the comment to name.
- Zircon has no loader-side check behind that statement: `elfldltl::DirectMemory::Store()` (`src/lib/elfldltl/include/lib/elfldltl/memory.h:393-400`) checks only bounds. Here, `MappedMemory` is stricter.
- Code written into the image after loading syncs itself: `ElfImage::ForEachPatch()` (`zircon/kernel/phys/include/phys/elf-image.h:141-164`) calls `SyncRange()` on each patched range. This is the rule `handoff.cpp` states for future code patching.

Linux syncs differently because its relocations do write code:
- `load_module()` (`kernel/module/main.c:3515-3523`) calls `flush_module_icache()` after `apply_relocations()` and `post_relocation()`. A module is an ET_REL object, and its riscv relocations rewrite instructions: `riscv_insn_rmw()` (`arch/riscv/kernel/module.c:55`), used by `apply_r_riscv_hi20_rela()` (line 190) and the others.
- On riscv, `flush_icache_range()` calls `flush_icache_all()` (`arch/riscv/include/asm/cacheflush.h:82-86`), which runs `local_flush_icache_all()`, that is `fence.i` (lines 11-14), on this hart and, on SMP, has the other harts do the same (`arch/riscv/mm/cacheflush.c:21-44`).
- Linux's riscv kernel self-relocation has no counterpart of this invariant. Under `CONFIG_RELOCATABLE`, vmlinux links with `-shared -Bsymbolic -z notext` (`arch/riscv/Makefile:10-11`), and `relocate_kernel()` (`arch/riscv/mm/init.c:307-339`, called from `setup_vm()` at line 1105) writes `R_RISCV_RELATIVE` words with no `fence.i` of its own.

**Acceptance**
- The comment above `arch::sync_instruction_fetch_range()` in `kernel/phys/elf_image.cpp` names three things: `MappedMemory`'s refusal of stores outside writable segments as the guarantee, lld's `-z text` as the reason a well-formed image never hits that refusal, and a segment both writable and executable as the case that needs another sync after relocation. The words "against read-only memory" no longer appear.
- The comment in `kernel/arch/riscv/phys/handoff.cpp` states the same condition.
- For each image phys loads, `prebuilt/clang/llvm-23/bin/llvm-readelf -lW <image> | grep LOAD | grep -cE ' [R ]WE '` prints 0. The images are, in `out/bringup.riscv` and `out/bringup.x86` alike, `kernel.phys_<arch>.phys.dbg_0/obj/kernel/phys/physload`, `kernel.phys_<arch>.phys.dbg_0/obj/kernel/phys/physboot.module` and `kernel_<arch>.dbg.0/obj/kernel/tests/fake-kernel/fake-kernel.image.elf` (`<arch>` riscv or x86), plus x86 `kernel_x86.dbg.1/obj/kernel/products/bin/vmours.image.elf` and `kernel_x86.dbg.0/obj/kernel/products/bin/vmours.with-tests.image.elf`.
- The fixing change's record answers the 261028 entry's question correctly: `-z notext` makes `relocate()` fail, and a writable and executable segment makes the fence too early.
- x86 `ours-verify` passes, and the riscv fake image boots to `[fake-kernel] handoff ok`. The change is comment-only, but every commit is verified.
