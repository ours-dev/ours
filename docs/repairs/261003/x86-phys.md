# x86-phys outstanding repairs

## 1. The kernel reads the ACPI RSDP out of the Multiboot info block, which nothing reserves

**Severity**
Low: ACPI initialisation can read memory that an early boot allocation has already overwritten. It is not seen today, because the early allocators place from the top of memory down and GRUB puts the block low. phys uses `allocate_top_down()`, and the kernel's `EarlyMem` defaults to `AllocationDirection::TopDown` (`early_mem.hpp`). But nothing guarantees either.

**Symptom**
Latent, cannot be triggered today. It would show up as an ACPI init failure (no valid RSDP), or as wrong tables. That happens if a boot-memory allocation lands on the pages of the Multiboot2 information block (MBI) and writes them before ACPI reads it. Such an allocation could come from phys, or from the kernel before `PlatformInitLevel::StartUp`.

**Cause**
`parse_acpi()` (`kernel/arch/x86/phys/legacy_multiboot.cpp`) does not copy the RSDP. It records a pointer into the MBI: `acpi_rsdp = tag.bytes().data() + sizeof(MultibootTag)`. physload passes that pointer on unchanged (`HANDOFF.acpi_rsdp = legacy->acpi_rsdp`, `kernel/phys/physload.cpp`). The kernel follows it in its ACPI init hook (`GKTL_INIT_HOOK(AcpiInit, ...)` at `PlatformInitLevel::StartUp`, `kernel/platform/pc/acpi.cpp`, then `table_set.cpp`). That hook runs first in `init_platform_early()` (`kernel/platform/pc/init.cpp`), before `platform_init_pmm()`, and keeps no pointer to the RSDP afterwards.

So the danger is not the page allocator, which starts later, but boot memory before then. Nothing reserves the MBI: the only reservations in phys are the BootPackage, the phys image, the kernel image, page tables, the handoff and kernel data. So the MBI is free as far as bootmem knows:
- for every allocation phys makes (`allocate_top_down()`: physboot's scratch, the kernel's image, page tables);
- for the kernel's own early allocations before StartUp (`EarlyMem`, for example in `CpuLocal::init_early()`).

The boot shim only keeps physload's scratch above the MBI (`shim.cpp`, `find_scratch`'s floor). It does not keep later allocations off it.

Corrected 2026-10-03: this item first said ACPI reads the RSDP after the page allocator starts. A review of the boot-image work found the hook runs before it.

**Change**
One of the following, the first preferred:
- Copy the RSDP (36 bytes for ACPI 2.0+) into the handoff in phys, as Zircon's physboot hands the kernel what it needs instead of pointers into boot-loader memory. `acpi_rsdp` then points into memory reserved as Handoff.
- Reserve the MBI range, rounded out to whole pages, as Handoff in phys.

Ruled out: relying on the order that happens to hold today, ACPI's hook before the page allocator and allocations from the top down. That is a hidden constraint on two subsystems.

**Reference**
- Zircon's `ZBI_TYPE_ACPI_RSDP` item carries the physical address of the firmware's own RSDP. physboot copies that address into `PhysHandoff::acpi_rsdp` (`zircon/kernel/phys/handoff-prep-zbi.cc`). The kernel therefore follows it into firmware memory, never into boot-loader scratch.
- Linux's x86 boot code copies the boot parameters into `boot_params` before anything allocates (`arch/x86/kernel/head64.c`, `copy_bootdata()`).

**Acceptance**
- `acpi_rsdp` in the handoff points into a reserved range, by a debug check in phys or the kernel.
- ACPI init still finds the tables on both images under `ours-verify`.
