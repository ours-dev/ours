# riscv-phys changes

## Title: the riscv phys headers that Handoff includes, so code that reads a Handoff compiles for riscv (2026-10-28)

**Files**
- `kernel/arch/riscv/phys/include/ours/phys/arch_bootmem.hpp` — `BootMem` is `bootmem::PhysicalMemory`, as on x86
- `kernel/arch/riscv/phys/include/ours/phys/arch_handoff.hpp` — an empty `ArchHandOff` and the declaration of `arch_handoff_to_kernel()`
- `kernel/arch/riscv/phys/BUILD.gn` — `:headers` publishes both and depends on bootmem's headers

**Symptom**
Every riscv translation unit that includes `ours/phys/handoff.hpp` stopped at `'ours/phys/arch_bootmem.hpp' file not found`: physload's sources, physboot, and the fake kernel. Four pieces of riscv work about to run in parallel all include it.

**Cause**
`handoff.hpp` includes two headers each architecture supplies, and riscv's `:headers` target was an empty placeholder.

**Change**
The two headers, with the same content as x86's apart from the comment on `arch_handoff_to_kernel()`, which states the riscv obligation: install `pgd` in `satp` from code that stays mapped across the switch, then jump with the handoff in `a0`. The definition is not written; nothing calls it on riscv yet. `arch_paging.hpp`, the third header x86 has, is left for the change that adds riscv's page-table format, since its content is that format.

Landed on its own, before the parallel work starts, so that each piece branches from a tree where the shared header compiles instead of each adding its own copy.

**Reference**
None. These are this tree's own seams; x86's versions are in `kernel/arch/x86/phys/include/ours/phys/`.

**Verification**
- Compiling `fake_kernel.cpp` for riscv now gets past the includes and stops at its x86 port I/O inline assembly, which is the fake kernel's own riscv work.
- Compiling physload's and physboot's sources for riscv now fails only on `ours/phys/arch_paging.hpp` (4 files), `ours/arch/aspace_layout.hpp` (2 files) and `arch::boot_marker` in ccrt (1 file).
- `ours-verify`: build ok, dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), 148 host tests pass; x86 does not read these files.

**Deliberately not done**
- `arch_paging.hpp` and the address-space layout: they wait for the Sv39 page-table format.
- `arch_handoff_to_kernel()`'s definition.

**How it was found**
Compiling the riscv physload, physboot and fake kernel objects one by one with `-Wno-unused-command-line-argument` added, to see past `-fno-plt`, which riscv clang ignores and `-Werror` turns into a failure in every file.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does an architecture-neutral header like `handoff.hpp` include per-architecture headers at all, rather than the architecture including it?
- Why must the code that writes `satp` stay mapped under both the old and the new root?
- Why land two copied headers as their own commit instead of inside the first piece of work that needs them?
