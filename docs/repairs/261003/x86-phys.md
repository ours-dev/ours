# x86-phys outstanding repairs

## 1. The kernel reads the ACPI RSDP out of the Multiboot info block, which nothing reserves

**Severity**
Medium: ACPI initialisation can read memory the page allocator has already handed to someone else. It is not seen today, but it depends on where GRUB puts the block and on what the kernel allocates first.

**Symptom**
Latent, cannot be triggered today. It would show up as an ACPI init failure (no valid RSDP), or as wrong tables. That happens if an allocation made between the page allocator's start and `init_platform()` lands on the pages of the Multiboot2 information block (MBI) and writes them.

**Cause**
`parse_acpi()` (`kernel/arch/x86/phys/legacy_multiboot.cpp`) does not copy the RSDP. It records a pointer into the MBI: `acpi_rsdp = tag.bytes().data() + sizeof(MultibootTag)`. physload passes that pointer on unchanged (`HANDOFF.acpi_rsdp = legacy->acpi_rsdp`, `kernel/phys/physload.cpp`). The kernel follows it in `init_platform()` (`kernel/top/main.cpp`, then `acpi.cpp` and `table_set.cpp`). That is after `init_platform_early()` has started the page allocator, and the allocator has by then received every range bootmem calls free. Nothing reserves the MBI: the only reservations in phys are the BootPackage, the phys image, the kernel image, page tables, the handoff and kernel data. The boot shim only keeps physload's scratch above the MBI (`shim.cpp`, `find_scratch`'s floor). It does not keep the MBI from being freed later.

**Change**
One of the following, the first preferred:
- Copy the RSDP (36 bytes for ACPI 2.0+) into the handoff in phys, as Zircon's physboot hands the kernel what it needs instead of pointers into boot-loader memory. `acpi_rsdp` then points into memory reserved as Handoff.
- Reserve the MBI range, rounded out to whole pages, as Handoff in phys.

Ruled out: reading the RSDP before the page allocator starts. That orders two subsystems on a hidden constraint.

**Reference**
- Zircon's `ZBI_TYPE_ACPI_RSDP` item carries the physical address of the firmware's own RSDP. physboot copies that address into `PhysHandoff::acpi_rsdp` (`zircon/kernel/phys/handoff-prep-zbi.cc`). The kernel therefore follows it into firmware memory, never into boot-loader scratch.
- Linux's x86 boot code copies the boot parameters into `boot_params` before anything allocates (`arch/x86/kernel/head64.c`, `copy_bootdata()`).

**Acceptance**
- `acpi_rsdp` in the handoff points into a reserved range, by a debug check in phys or the kernel.
- ACPI init still finds the tables on both images under `ours-verify`.
