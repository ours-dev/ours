# irq-multi-arch: the steps that make kernel/irq carry x86, riscv and arm, one at a time

**Date**: 2026-11-01
**Status**: Proposed

## Request

The user said "开始refactor中断系统" on 2026-11-01, after the discussion recorded in `DESIGN.md`: riscv is being introduced on other branches now, arm and GPIO controllers follow, and cascaded controllers belong in user space. Scope ends at the changes to `kernel/irq`, the platform entry seam, the chip contract, the line tables, the virtual interrupt's ack signal and the answer for a kernel-owned source. Not in scope: any non-x86 controller, MSI, a device-tree route service, the Driver Manager, performance work.

The user approved starting, not the individual decisions below. Step 1 only writes them down as proposals. A step that needs a decision waits for it.

## Decisions

The full table, with options and reasons, is in `DESIGN.md`. Those that gate a step:

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D3. The interrupt entry (gates step 2) | a numberless `platform::handle_irq(frame)`; two entries; the controller's driver installs the entry | a numberless entry, the decoder ending what no line has | None | |
| D9. Merge order and the branch that carries D3 (gates step 2) | `feat/driver-caps` first; the riscv slices first | `feat/driver-caps` first, D3 once, on the branch the user names | None | |
| D8. The hands-off pieces (gates step 3) | change only the `set_affinity` stub to NotSupported; leave the rest | that | None | |
| D4. The user number and a kernel-owned source's answer (gates steps 4 and 6) | one number in a platform-wide numbering; a specifier. AccessDenied or NoResource | one number; AccessDenied | None | |
| D5. The ack of a level child (gates step 5) | a signal on the virtual interrupt; a message; a new call | a signal | None | |

## Steps

Each step is one commit and is introduced to the user before it starts, one at a time (CLAUDE.md). Only step 1 is under way.

1. **Design and plan.** `DESIGN.md` and this file, and a pointer from `.agents/tasks/interrupt-system/DESIGN.md`, whose non-goal "any port to ARM or RISC-V" this design takes up. Verified by: `scripts/lint_docs.py --changed`, and an independent check of every `file:line` against the tree and of the riscv-side facts against `origin/feat/riscv`. Lands as `docs(irq-multi-arch): ...`.
2. **The interrupt entry (D3, D9).** The platform header stops including the x86 arch header and takes a neutral frame type. `platform::handle_irq(frame)` replaces `platform_handle_irq`, `platform_start_handling_irq` and `platform_finish_handling_irq`; arch calls the preamble and postamble once per trap; pc's decoder maps the vector to the GSI and ends what no line has; the dead vector parameter goes. This is item R19 of `.agents/tasks/arch-platform-seams/DESIGN.md`; it is done once, on the branch the user names. No behaviour change on x86. Verified by: `ours-verify` on both images (dbg 44 passed and 4 skipped, rel 41 passed and 8 skipped before), and the four kernel-shell tests of the layer.
3. **The chip contract (D8).** The clauses of `DESIGN.md` go into `chip.hpp`, replacing the x86-only reading at lines 34-40; `configure` says what it does on a controller with no mode register; `irq::set_affinity` and the IO-APIC's stub answer NotSupported (the tick device discards the result, `tick_device.cpp:218`). Verified by: the same boot and tests, and a kernel-shell case that a chip without a mode register refuses `ModeDefault` and takes an explicit mode.
4. **Per-controller tables and the numbering (D4).** A `DescTable` and a `Domain` per controller, a directory from virq range to table, registration after boot in place of the one-shot `init_lines`, and the platform-wide numbering rule written down. Verified by: a kernel-shell test with two tables on one machine, and the existing tests unchanged.
5. **The ack signal on a virtual interrupt (D5).** One signal raised from `unmask_interrupt()`, the position fixed as ABI, documented in `syscalls.gni`, with a test of the order of unmask against trigger and of both ack paths. Verified by: the test, `ours-verify`, and a mutation that moves the raise before the debt clears.
6. **A kernel-owned source answers AccessDenied (D4).** `interrupt_map` and `interrupt_create` for a line the kernel owns, with `routing_checks` and the shell tests moved. Verified by: `routing_checks` on the rel image.

Later, each its own plan when its trigger appears: a device-tree route service with a tagged answer; MSI (a dynamic virq lookup, message composition, a device identity for an ITS); a real `set_affinity`.

## Files

- Step 1: `.agents/tasks/irq-multi-arch/DESIGN.md` and `PLAN.md`, new; `.agents/tasks/interrupt-system/DESIGN.md`, one line in the non-goals.
- Step 2: `kernel/platform/include/ours/platform/interrupt.hpp`, `kernel/platform/pc/interrupt.cpp`, `kernel/arch/x86/kernel/faults.cpp`, `kernel/kernel/irq/mod.cpp`, `kernel/include/ours/irq/mod.hpp`, the `task.md` rule at lines 101-104 if D3 changes it.
- Step 3: `kernel/include/ours/irq/chip.hpp`, `kernel/kernel/irq/mod.cpp`, `kernel/arch/x86/apic/io_apic.cpp`, `kernel/commands/irq_test.cpp`.
- Step 4: `kernel/kernel/irq/desc_table.cpp`, `domain.cpp`, `mod.cpp`, `kernel/include/ours/irq/{desc_table,domain,mod}.hpp`, `kernel/platform/pc/interrupt.cpp`, the kernel-shell tests.
- Step 5: `kernel/object/virtual_interrupt_dispatcher.{hpp,cpp}`, `kernel/object/interrupt_dispatcher.{hpp,cpp}`, `system/public/ours/syscalls.gni`, the tests.
- Step 6: `kernel/lib/syscall/src/interrupt.cpp`, `kernel/object/interrupt_event_dispatcher.cpp`, `system/service/init/test/routing_checks/routing_checks.cpp`, `system/app/shell/shell_tests.cpp`.
- Every step: a change record in `docs/changes/<YYMMDD>/<module>.md`.

## Test plan

- `ours-verify` after every step with code: build, dbg and rel images boot, `routing_checks` 6 of 6 required on rel, `pc-ps2: key a/A`.
- The kernel-shell tests of the layer by hand, since `boot_check.py` does not run them: `irq.test`, `interruptline.test`, `pinlines.test`, `irqdomain.test`.
- `scripts/lint_docs.py --changed` for every document.
- Not verifiable here: arm and riscv builds of any of this, and the PLIC's mask during a claim on silicon.
