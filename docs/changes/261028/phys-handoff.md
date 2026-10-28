# phys-handoff changes

## Title: Handoff and LegacyPlatformInfo carry the device tree's address (2026-10-28)

**Files**
- `kernel/phys/include/ours/phys/legacy_platform_info.hpp` — `device_tree`, the address a shim found, with why it can never become an OBI item
- `kernel/phys/include/ours/phys/handoff.hpp` — `device_tree` beside `acpi_rsdp`
- `kernel/phys/physload.cpp` — forwards it from the shim's struct into `HANDOFF`
- `kernel/arch/x86/phys/bootshim/shim.cpp` — says None explicitly: a Multiboot2 platform describes itself through ACPI
- `kernel/tests/fake-kernel/fake_kernel.cpp` — prints it with the other handoff fields

**Symptom**
None, step 0 of the riscv boot chain. The riscv shim receives a device tree address in `a1` and had no field to pass it on in, and the kernel would have had no way to learn it.

**Cause**
None, an addition. The user decided on 2026-10-27 to treat the device tree the way ACPI is treated: phys forwards only where the tree is, and the kernel's riscv platform layer walks it itself. That needs one field at each hop, the same two hops `acpi_rsdp` already takes: the shim's `LegacyPlatformInfo`, and `Handoff`.

**Change**
- `LegacyPlatformInfo::device_tree` and `Handoff::device_tree`, both `ustl::Option<PhysAddr>`, None where there is no tree.
- physload copies one into the other next to `acpi_rsdp`.
- The x86 shim names the field with `ustl::NONE` rather than relying on a default member initializer. The struct is built with designated initializers, and a field left out there is a warning this build treats as an error; spelling it out also says on the page that x86 has no tree.
- `PhysLoadHandoff` does not get the field. Its `acpi_rsdp` is written by physload and read by nothing, because physboot reads the shared `HANDOFF` directly; a second unread copy would only be something to keep in step.
- Both comments promise that the blob's own range is reserved in the memory map. On riscv that promise is kept by the shim's device-tree reading, which is the next change; nothing fills the field yet.

**Reference**
Zircon hands the kernel a device tree as a ZBI item (`ZBI_TYPE_DEVICETREE`) and its riscv64 boot shim reads the tree in phys; this tree cannot add an item at run time (OBI's tables are baked at build time), so the address travels in the side-channel struct instead, as the command line does. In Linux the riscv kernel receives the tree's physical address in `a1` and keeps it in `_dtb_early_pa` before `setup_arch()` unflattens it. Neither was re-read for this change.

**Verification**
- `ours-verify`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), 148 host tests pass.
- `ours-verify` does not boot the fake image; the new fake-kernel line is compiled by the default build, not observed running.

**Deliberately not done**
- Filling the field on riscv. The shim reads the tree in the next step.
- Reserving the blob's range. Same step.

**How it was found**
None, a new feature.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does the address travel in `LegacyPlatformInfo` and not in an OBI item, when the memory map is meant to become one?
- What breaks if the shim forwards the address but forgets to reserve the blob's range?
- Why does `PhysLoadHandoff` not get the field, although it carries `acpi_rsdp`?
