# irq-multi-arch: one interrupt line layer for x86, riscv and arm, with cascaded controllers in user space

**Date**: 2026-11-01
**Status**: Proposed

## Problem

`kernel/irq` was built for one machine: x86 with an IO-APIC. On 2026-11-01 the user said that riscv is being introduced on other branches now (`origin/feat/riscv`, tip `9b8e7690`, and five slices of it), and that arm and GPIO controllers follow soon. The same day the user asked whether, if every device driver except the root interrupt controller and MMIO lives in user space, `kernel/irq` still has a value or should be removed. Two read-only reviews by agents (13 in all; one re-checked its claims against Linux's irqchip sources; the transcripts are in the session, not in the tree) found:

- The layer is not a device-driver layer. It is the root controller's line layer: 14 files and 2168 lines (1016 of code), with two production owners of a line, the legacy timer (a Permanent action) and the user-space interrupt object. More drivers in user space make the interrupt object its main customer. The cascaded GPIO mapping the user asked about, a leaf's number behind a root line, is not in it: it has one chip, one linear domain and no parent.
- Removing it would move five duties, not delete them: a per-GSI slot that outlives every object, the detach barrier under the line lock L1, the level hold until the ack, attach with the mode in one step, and hwirq to virq for the user ABI. They would go into `platform/pc` with the same lock order, the roughly 400 hard lines of `desc.cpp`, and a rewrite of 2938 lines of kernel-shell tests, and it would reverse genirq decisions of 2026-10-05 (irq-genirq D1, D2, D8). A thin per-vector slot re-derives the same five duties and leaves the level mask with two writers.
- What the new controllers need is small, and mostly at the edges of the layer: the platform entry seam is x86-shaped, and `set_affinity` is a stub that answers Ok. This is the reviews' reading and the user's to confirm (D0).

The riscv side has its own plan. These documents exist only on `origin/feat/riscv` (read them with `git show origin/feat/riscv:<path>`): `.agents/tasks/riscv-kernel/PLAN.md` step 2 writes the kernel's trap vector and riscv's `IrqFrame`, step 5 "a PLIC `irq::Chip`", step 10 sends IPIs through SBI; `.agents/tasks/arch-platform-seams/DESIGN.md` has the interrupt entry as an open decision (P-irq, refactor item R19, issue I14) and says it must be decided before riscv step 2, because a PLIC gives the number only when the hart claims it. riscv is between its steps 1a (landed) and 1b (not started), so the deadline is near. Those branches have no `irq::Domain` implementation (`domain.hpp` is empty there), no `interrupt_map` and no acpi-route, since they were cut before the interrupt-system work of `.agents/tasks/interrupt-system/`. This design is the contract the two lines of work meet at.

## Goals and non-goals

Goals:
- One line layer that carries the IO-APIC, GIC SPIs and the PLIC with the five duties kept in one place. An APLIC is a design constraint, not a deliverable: no riscv document plans one, and the riscv verify boot is plain `-machine virt`.
- The kernel and user-space split of interrupt controllers stated as a test, so that a controller nobody has met yet is placed by it, and not by a list.
- Cascaded controllers (a GPIO bank, a mux, an expander) need no kernel code beyond one signal on the virtual interrupt.
- The riscv branch writes its PLIC chip and its entry against a stated contract, and the two branches do not edit the same seam twice.

Non-goals:
- MSI controllers (ITS, IMSIC, APLIC in MSI mode). Only: nothing here may make them a rewrite.
- GICv5 and extended SPI numbering. The first arm cut refuses them.
- Per-CPU interrupts inside the line layer (SGI, PPI, the hart's local causes, the timers and IPIs).
- Deleting or reshaping the pieces that serve no new controller: `Shared`, `issue_ack`, `startup`, `dump`, `Data::platform_data` and the x86 local APIC chip are uncalled or test-only and stay as scaffolding under the hands-off rule. `Permanent` is not among them: the legacy timer's line is attached with it.
- Any non-x86 controller code, a device-tree route service, the Driver Manager, performance work.

## Design

### Where the kernel ends and user space begins

Proposed (D1). A controller belongs to the kernel if either of these holds:

- K1. The line's flow or the CPU's own path touches it in interrupt context or while a CPU comes up: claim or acknowledge, end of interrupt, the mask and unmask the flow does, threshold, per-hart enable, the CPU's own registers.
- K2. It allocates or aims a delivery resource that is shared across protection domains: vectors, LPIs, EIIDs, an ITS device table, the routing of a distributor, an APLIC target.

K1 and K2 restate the three reasons of interrupt-system's Responsibilities table: privilege and interrupt context are K1, isolation is K2. By them the IO-APIC, the GIC distributor, redistributor and CPU interface, the ITS, the PLIC, an APLIC and an IMSIC are the kernel's. A controller that is only MMIO behind a root line is user space's: a GPIO bank, an expander on I2C or SPI, and an interrupt router whose output is a root line. Two cautions, both for D1. A router's MMIO can raise any root line it reaches, kernel-owned ones included; that is the power an MMIO resource already grants, bounded by the kernel's MMIO deny list (a table of up to 32 page-granular ranges, a few on a PC, which fails closed beyond that). And lines the kernel itself consumes (the console UART, an SMMU, a timer frame) are reachable by number from firmware tables, so `interrupt_map` has to answer for them (D4b).

The user's own rule of 2026-10-30, recorded on `origin/feat/riscv` in `.claude/skills/ours-kernel-design/references/layering.md`, puts the local APIC and the riscv hart's timer, IPIs and start and stop in `kernel/arch`, every other chip (IO-APIC, HPET, PLIC) in `kernel/driver/<class>/<chip>`, and the board in `kernel/platform/<board>`. K1 and K2 say which of those chips are the kernel's at all; the rule says where they live. A PLIC chip is written in `kernel/driver/<class>/plic`; `io_apic.cpp` is still in `kernel/arch/x86/apic` and is due to move there too, so an edit of it in step 3 should land before that move or be redone after it.

### What stays and what changes

| Piece | Verdict | Why | Where |
|---|---|---|---|
| `Desc`, `Action`, the Oneshot held bit, the detach barrier, L1 across the flow and the EOI | Stays | The same shape as the usual fast-EOI flow. EOI is on the CPU that took the interrupt, inside the handler. | `kernel/kernel/irq/desc.cpp` |
| `Flow` with two members | Stays | A level source runs `FastEoi` and an edge source runs `Edge`, chosen from its trigger mode; both end the interrupt after the actions, and they differ in whether a Oneshot action holds the line masked. | `kernel/include/ours/irq/types.hpp` |
| `Chip` as a runtime table | Stays (D7) | Four implementers are in sight: the IO-APIC, the GIC, the PLIC and, after it, an APLIC. | `kernel/include/ours/irq/chip.hpp` |
| `Domain` and `DescTable`, one table built once | Stays (D10) | Dense spaces fit: GIC SPI 32 to 1019, PLIC 1 to N with 0 reserved. A chip serves all its instances by finding them from `hirqnum`, as the IO-APIC does for several IO-APICs (`io_apic.cpp:72`). Wrong for LPIs (8192 and up) and other sparse spaces, which are MSI's. | `kernel/kernel/irq/desc_table.cpp:39` |
| The platform entry seam | Changes (step 2, D3) | `kernel/platform/include/ours/platform/interrupt.hpp:8` includes the x86 arch header; arch brackets every vector with start and finish (`faults.cpp:438`, `:440`) and passes the raw vector, not the GSI (`faults.cpp:434`); the preamble ignores its number (`kernel/kernel/irq/mod.cpp:112`); arch, not the platform, ends a no-line interrupt (`faults.cpp:261-263`). | see below |
| `Chip::set_affinity` and `irq::set_affinity` | Changes (step 3, D8) | The stub answers Ok and does nothing (`kernel/kernel/irq/mod.cpp:69`). | hands-off |
| `Chip::configure` for a controller with no mode register | Changes (step 3) | PLIC has no trigger register. The review's first reading, that a level PLIC source would storm on the edge flow, does not hold for the callers there are: `attach` with a mode calls `configure` and re-selects the flow (`desc.cpp:157-168`), and `attach` without a mode answers NotSupported when the chip has no `trigger_mode` (`desc.cpp:141-143`). The two-argument `attach(virq, action)` still lets a Oneshot action onto such a chip, on the edge flow; nobody does that today. | see below |
| `Virtual` interrupt | Changes (step 4, D5) | The controller of a cascade cannot learn that a level child was acked: `unmask_interrupt()` is empty (`kernel/object/include/ours/object/virtual_interrupt_dispatcher.hpp:35`) and the header says the signal is "left out: nothing here asks for that yet". | see below |
| `Shared`, `issue_ack`, `startup`, `dump`, `Data::platform_data` | Stay unused | No new controller needs them (`Shared` is used by tests only). | hands-off |
| `Permanent` | Stays, and is used | The legacy timer's line (`time.cpp:64`, `:95`) and its lockless flow (`desc.cpp:383-385`). It is also what marks a line as the kernel's today (`desc.cpp:93-95`). | |

### The interrupt entry

The three architectures find the interrupt differently, and none but x86 does it by arithmetic on a vector:

| Architecture | How the number is found | What ends it |
|---|---|---|
| x86 | The vector in the trap frame, less the platform's base | The local APIC EOI |
| arm GICv2 and GICv3 | A read of the acknowledge register, which also takes the interrupt | A write of the same value back; on v3 the acknowledging CPU interface |
| riscv PLIC | A read of this hart's claim register | A write of the id back to the same register |

Events of the entry as proposed (D3). Objects first: arch's trap entry; the board's decoder, a function of the platform code that calls the controller driver's claim and complete helpers (the platform finds the chips and starts their drivers, per the user's rule above); the line layer; the architecture's own handlers for per-CPU interrupts (timer, IPI).

1. The trap enters arch, which calls `irq::handle_irq_preamble` once for the trap.
2. Arch first separates the CPU's own causes. On x86 that is the vector switch (`faults.cpp:266-312`, the timer, error, spurious and IPI vectors); on riscv `scause`, whose software and timer causes are arch's. Only the external interrupt goes on. On arm the decoder cannot do this earlier, since the acknowledge read also returns SGIs and PPIs (INTID below 32), and it hands those to arch's handler.
3. Arch calls `platform_handle_irq(frame)`, which loses its number and takes `arch::IrqFrame *` only. The platform's decoder identifies the interrupt (the vector, an acknowledge read, a claim). It takes one interrupt per trap in the first cut. Looping while the controller has more is a throughput option, with no bound and a longer preemption-disabled window; it is put to the user with a measurement when it matters (`docs/repairs`).
4. The decoder drops what is not an interrupt (GIC ids 1020 to 1023, a claim of 0).
5. For the rest it calls `irq::handle_irq_generic(hwirq)`, which answers whether a line ended the interrupt.
6. If no line had it, the decoder ends it itself: the GIC deactivates it, a PLIC must complete it. The reference PLIC handler in Linux only warns and leaves the source wedged in the gateway, which is the case this clause is for.
7. Arch calls `irq::handle_irq_postamble` and takes the preemption decision.

Moving the no-line EOI from arch (`faults.cpp:261-263`) to the platform changes a recorded genirq detail (`.claude/skills/ours-kernel-design/references/task.md` lines 100-104), so it is the user's decision. This is P-irq (a) of arch-platform-seams, written there as `platform::handle_irq(frame)`. The user's naming rule of 2026-10-30 (a function every platform defines is `platform_<name>`, a free function in `ours::`, chosen over namespaces) came after that text, and refactor item R7 renames every `platform::` function to the prefix form, so this design keeps the prefix form and the existing name. The frame stays `arch::IrqFrame` through the dispatcher `<arch/interrupt.hpp>`: no second neutral type. `origin/feat/riscv` already made the include edit (riscv-kernel step 1a.1, `c0753c85`), so whichever branch lands second drops it.

Counterparts. Zircon: `platform_irq(frame)` with `int_handler_start` and `int_handler_finish` called by arch, as the seams document records them (`exceptions_c.cc:541`, `dev/pdev/interrupt/interrupt.rs:526`; cited there, not re-read). Linux: `set_handle_irq` and the root handlers of `irq-gic-v3.c` and `irq-sifive-plic.c`, read from Linux master on 2026-11-01 and not kept in the tree; the flow is `handle_fasteoi_irq`, and the rejected cascade is `irq_set_chained_handler_and_data` with hierarchical domains.

### The chip contract

Clauses the chips of all architectures keep (step 3 writes them into `chip.hpp`, where lines 34-40 are x86's today):

- `mask`, `unmask` and `issue_eoi` may run in interrupt context or in thread context with interrupts off. `mask` and `unmask` are called under L1; `issue_eoi` is called in interrupt context, under L1 unless the line is Permanent. None sleeps, and none waits on another CPU. A bounded poll (the GICv3 redistributor's write-pending bit) is fine.
- `issue_eoi` runs on the CPU that took the interrupt, inside `handle_irq`. It is never deferred to `finalize_oneshot` or to a thread.
- A controller whose mask completes on another CPU (an IMSIC) cannot hold a Oneshot line. `check_oneshot` cannot tell today: it only tests that the `mask` and `unmask` pointers exist. Step 3 adds a chip flag or a rule, or drops the case while MSI is a non-goal.
- `configure` refuses with NotSupported a mode the controller cannot do (a GIC SPI takes level-high and edge-rising only; the PLIC cannot invert polarity), and leaves the source masked, as it does for every chip. On a controller with no mode register it stores the requested mode in the chip's own per-source array, atomically, since `trigger_mode` is read without L1. That is a chip with state of its own, which the sentence in `chip.hpp:37-40`, "the chip keeps nothing in the line's data", does not rule out for the chip's own storage. The route service gives the mode that firmware gives, so non-x86 platforms always create an interrupt with an explicit mode.
- A chip with several instances finds its instance from `hirqnum`, as the IO-APIC does.
- `set_affinity` answers NotSupported until it is real; its signature is settled with the real implementation (interrupt-system P4).
- A PLIC chip masks with the source's priority register (0 and 1) and keeps the enable bits set, so a completion for a masked source is still valid.

What a controller driver provides, which a PLIC author needs and the per-line operations do not cover: the initial state of every source (priority 0, so masked; the enable bit for the boot hart's context, since the boot CPU takes every device interrupt; the threshold), a bring-up and bring-down hook per CPU (`kernel/include/ours/cpu_hotplug_hook.hpp`), its MMIO reserved with `platform::reserve_kernel_mmio` (`kernel/platform/include/ours/platform/mmio.hpp:30`), and the use of `hirqnum` alone: `virqnum` never equals it. Until D10 says otherwise, a controller registers all its numbers through one `irq::init_lines` call at boot.

### Numbering

The hardware numbers of a machine are one platform-wide space: a controller owns `[base, base + n)`, as ACPI's `gsi_base` does for a PLIC or an APLIC, and a GIC's number is its INTID (ACPI on arm calls the INTID the GSI). The interrupt path is reached through the decoder, so nothing in the public interface should promise an array indexed by hardware number; that keeps MSI from being a rewrite. One table built once per machine (D10 (a)) keeps what interrupt-system and `task.md` record: the table is read on the interrupt path without a lock, it is indexed by hardware number, and every virq is above every hardware number of the machine. A table per controller, a directory from virq range to table and registration after boot (D10 (b)) would depart from all three and would need the first virq to come from one global allocator, since `Domain::init` starts each domain at the first multiple of 32 past its own highest number, so two domains would overlap in virq space. It is MSI's (P3), unless a machine needs it earlier.

### Cascades in user space

Objects: the parent, a physical interrupt on a root line, level, Oneshot; the GPIO driver G, which holds the parent and the bank's MMIO; one virtual interrupt per pin, made by G; a client K, which holds one pin's interrupt and does not know it is virtual; the root controller's flow in the kernel.

Events for a level child on a level parent:

1. Pin i asserts, so the parent line asserts.
2. The kernel's flow masks the parent, runs the action, and holds the line masked until G acks (the interrupt object's action is Oneshot and always answers WakeThread).
3. G wakes, reads the bank's status, masks pin i in the bank's own register, and calls `interrupt_trigger` on pin i's virtual interrupt.
4. G acks the parent, and the kernel unmasks the line. Pin i is masked in the bank, so the parent deasserts.
5. K wakes and clears the cause in its device.
6. K acks (the next `interrupt_wait`, or `interrupt_ack` on a port). Today G does not see this.
7. With D5, the kernel raises a signal on pin i's virtual interrupt, G sees it, and G unmasks pin i in the bank.

Step 6 is the gap. An edge child is fine, since G clears the latch when it forwards. The proposal (D5) is one signal on the virtual interrupt, raised when its ack clears the debt, which is where the base class already calls `unmask_interrupt()` (from `wait()` and from `ack()`, both with the object's spin lock dropped, in thread context). Facts that step 4 has to settle, all read from the tree:

- `update_state` takes a Mutex and cannot be called with the spin lock held, so the signal is raised from `unmask_interrupt()` outside it; a program cannot clear a bit, so the signal is a level bit that `interrupt_trigger` clears after it drops its lock. The order of the two is what the test pins.
- `unmask_interrupt()` is reached only when an ack clears the debt. If K closes its handle or exits without acking, nothing is destroyed while G holds its own handle, no signal comes, and pin i stays masked in G's bank. The destroy path has to raise the signal too, or G has to be told by another means.
- The bit's position is ABI. Bit positions are documented in `system/public/ours/signals.hpp`, which step 4 edits.

The handle given to K is a `handle_duplicate` of G's with Wait, Read, Write and Transfer, and without Signal and Duplicate: `interrupt_wait` checks Wait, `interrupt_bind` Read, `interrupt_ack` Write, and `interrupt_trigger` Signal. `interrupt_destroy` asks for no right, so K can still destroy its pin's interrupt; that is accepted here, or it is a decision of its own. The alternative to the signal, a message from K to G, makes K tell the two kinds apart.

Nothing else in the kernel changes for GPIO. The three shapes seen in Linux (one parent source per pin, one summary line per bank with a status register, a mux with a fixed parent) all reduce to a parent from `interrupt_map` and `interrupt_create` plus a virtual interrupt per pin. A leaf pin has no kernel number.

### The user ABI

`interrupt_map(resource, source, out virq)` keeps its one number (interrupt-system D11 (a)). Its meaning becomes: the root controller's architectural number in the platform's single numbering. On arm that is the INTID for SPIs (ACPI's GSI as is, device-tree cells with 32 added by the route service). On riscv it is the PLIC source id (plus `gsi_base` under ACPI). SGIs, PPIs, the hart's local causes and the IMSIC's IPI id are never sources. Decoding device-tree cells is keyed by the controller's compatible string and lives in a user-space route service, which holds a read-only copy of the device tree as acpi-route holds the ACPI tables. Its answer must be tagged from the start, `Root{source, trigger, polarity}` or `Leaf{controller, cells}`, since a chain of `interrupts-extended` can end at a GPIO controller. That service is a later task.

Today `interrupt_map` answers Ok for a line the kernel holds (the legacy timer's), and `interrupt_create` answers AlreadyBound, the same code as for a line another program holds. D4b proposes AccessDenied from both for a kernel-owned line, so the route service can tell the kernel's line from one a program holds. That needs a marker for "kernel-owned" in the line, which is Permanent today (`desc.cpp:93-95`); the marker is part of step 5.

interrupt-system D11 recorded the specifier form (b) for "when the first non-x86 platform arrives". riscv is that platform. This design proposes to keep (a) anyway and narrow the trigger to a device-tree platform with two root controllers whose numbers collide and nobody owns the flattening. That departs from D11's wording and is the user's to decide (D4a).

### Hardware risks

- The Oneshot level flow masks a line after the decoder's claim and before the end of interrupt. On a PLIC that is a mask while a claim is open. Linux does it; seL4's PLIC code warns that masking during an open claim misbehaved on some hardware and so completes at the ack instead (both read on 2026-11-01, not kept in the tree). QEMU will not show it, so it is a checklist item for real silicon. If it fails, the hold becomes a chip operation, and the completion has to reach the claiming hart (D6).
- Every GICv3 mask polls the redistributor's write-pending bit in interrupt context. Any change to that goes through a performance report first.

## Alternatives considered

Each is "not recommended" with its decision row; none is the user's rejection.

- **Remove `kernel/irq` and fold it into `platform/pc`** (D0). It is a move, not a deletion (above), it loses what arm and riscv would reuse, and it reverses decisions of 2026-10-05.
- **Shrink it in place now** (D0): Domain to a fixed offset, Flow to a bit, a single action slot; about a quarter of the code. The reviews argued it when riscv and arm looked like placeholders. With the second and third controllers coming, the Chip table, the Domain and the flows have users. `Shared`, `issue_ack`, `startup` and `dump` are left as they are for the hands-off rule, not because they are used.
- **A thin per-vector slot with the mask a property of the object kind** (D0). The five duties come back, and the level mask has two writers again (irq-genirq D1).
- **Per-CPU interrupts as lines in `Desc`** (D2). Every CPU would take one global lock per tick, or the layer would need a lockless permanent path plus a per-CPU enable that `Data`, which carries no CPU, cannot express. They also cannot be held by a user program.
- **Cascaded controllers in the kernel**, as Linux's chained handlers and hierarchical domains (D1). A leaf's mask register is user-owned MMIO that interrupt context under L1 cannot reach, and the leaf needs no kernel number.
- **The ack of a level child as a message from the client to the controller driver** (D5). The client must not tell a virtual interrupt from a physical one.
- **A firmware-neutral specifier in `interrupt_map` now** (D4a). Deferred: one number is enough while a platform has one numbering.
- **The mode of a line owned by `Desc`** rather than by the controller. Not needed: the point above about `configure`. Revisit with the PLIC code.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D0. What happens to `kernel/irq` | keep and extend at its edges; shrink in place; fold into `platform/pc` | Keep and extend | None | |
| D1. The test for the kernel and user split | K1 and K2; the root controller only; by privilege alone | K1 and K2 | None | |
| D2. Per-CPU interrupts | out of the line layer, arch-owned; lines in `Desc` | Out. The user's placement rule of 2026-10-30 supports it for the local APIC and the hart's timer and IPIs; extending it to the GIC's SGIs and PPIs is the open part | None | |
| D3. The interrupt entry (arch-platform-seams P-irq) | (a) `platform_handle_irq(frame)` with no number, the decoder owning spurious ids and the no-line end; (b) two entries; (c) the controller's driver installs the entry. Deadline: before riscv-kernel step 2 | (a) | None | |
| D4a. The user number | keep D11 (a), one number in a platform-wide numbering; the specifier form when the first non-x86 platform arrives (D11's wording); the specifier form when two root controllers collide | One number; the specifier when two root controllers collide | None | |
| D4b. What a kernel-owned source answers | AccessDenied from `interrupt_map` and `interrupt_create`; leave as is | AccessDenied | None | |
| D5. How the controller of a cascade learns a level child was acked | (a) a signal on the virtual interrupt; (b) a message from the client; (c) a new call | (a) | None | |
| D6. Where the level hold lives | in `Desc`, as now; as a chip operation | In `Desc`, with the PLIC check on silicon | None | |
| D7. `Chip` as a runtime table or at build time | runtime; one image per platform with link-time functions (the build-time arch-selection rule of `modern-cpp-development`) | Runtime. It departs from that rule and is justified only if one image must choose PLIC or APLIC at boot (open question 1) | None | |
| D8. The hands-off pieces | change only the `set_affinity` stub to NotSupported; leave the rest | That | None | |
| D9. Merge order | per step, see PLAN.md | `feat/driver-caps` first for what only it can carry; steps 2 and 3 on whichever branch the user names | None | |
| D10. Tables | (a) one table built once per machine, a chip finding its instance from `hirqnum`; (b) a table per controller, a directory and registration after boot | (a) for the wired controllers of the Goals; (b) when a second table is needed (MSI, P3). (b) departs from interrupt-system D3 (ii), its answer to where virqs start, and `task.md` | None | |

## Testing and verification

- Every step that touches `kernel/irq` or the x86 entry: `ours-verify` on both images, and the kernel-shell tests that hold the layer's races. None of them is in the boot gate; run them on the dbg image with `python3 scripts/kshell_run.py --out out/<dir> irq.test interruptline.test pinlines.test irqdomain.test`, and `interrupt.test` for steps 4 and 5, which change the state machine it tests.
- New cases go in an arch-neutral file under `kernel/commands`, like `irq_domain_test.cpp`, which includes no x86 header. On `origin/feat/riscv`, `irq_test.cpp` and `interrupt_line_test.cpp` have moved to `kernel/arch/x86/commands` (refactor R25, `3135d44a`), and `pin_lines_test.cpp`, which includes x86 headers, will have to move there too after the merge.
- Step 4 adds a kernel-shell test of the signal on a virtual interrupt, including the order of unmask against trigger and a handle closed without an ack.
- Cannot be verified here: arm and riscv (nothing builds them in this tree's gate), a PLIC or a GIC on silicon, the mask-during-claim ordering, and any interrupt from a real GPIO.

## Open questions

- The riscv plan targets the PLIC (step 5) and SBI (step 10), and its verify boot is `-machine virt` with no AIA option; that QEMU's default there is the PLIC is from memory. Is an APLIC, which no riscv document plans, in scope for this design? D7 depends on it.
- Whose is the flattening of numbers for a device-tree platform with two or more root controllers: a rule published by the kernel, or one the route service makes up?
- Does the device tree handed to user space need its `/chosen` node scrubbed (boot arguments, random seeds)? From memory, unchecked.
- What shape does the MSI call take: the message returned to the caller, or written by the kernel into a VMO? It needs a device identity for an ITS.
- GICv5 packs the interrupt type into the high bits of the number, which breaks any array indexed by hardware number. Refused as NotSupported in the first arm cut?
- Does a Virtual interrupt's `interrupt_ack` still require a bound port (the wait is the ack when unbound)? Step 4 settles the signal for both paths.
