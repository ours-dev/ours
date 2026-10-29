# phys outstanding repairs

## 1. physboot's handoff log line calls the page-table root `cr3` on every architecture

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
