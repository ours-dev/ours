# irq-multi-arch: one interrupt line layer for x86, riscv and arm, with cascaded controllers in user space

**Date**: 2026-11-01
**Status**: Proposed

## Problem

`kernel/irq` was built for one machine: x86 with an IO-APIC. On 2026-11-01 the user said that riscv is being introduced on other branches now (`origin/feat/riscv` and five slices of it, read on 2026-11-01), and that arm and GPIO controllers follow soon. The same day the user asked whether, if every device driver except the root interrupt controller and MMIO lives in user space, `kernel/irq` still has a value or should be removed. Two read-only reviews (13 agents, one of them re-checking claims against Linux's irqchip sources) answered:

- The layer is not a device-driver layer. It is the root controller's line layer: 14 files and 2168 lines (1016 of code), with two production owners of a line, the legacy timer and the user-space interrupt object. More drivers in user space make the interrupt object its main customer. The cascaded GPIO mapping the user asked about (a leaf's number behind a root line) is not in it: it has one chip, one linear domain, no parent.
- Removing it would move five duties, not delete them: a per-GSI slot that outlives every object, the detach barrier under the line lock L1, the level hold until the ack, attach with the mode in one step, and hwirq to virq for the user ABI. They would go into `platform/pc` with the same lock order, the roughly 400 hard lines of `desc.cpp`, and a rewrite of 2938 lines of kernel-shell tests, and it would reverse the genirq decisions of 2026-10-05 (irq-genirq D1, D2, D6). A thin per-vector slot re-derives the same five duties and leaves the level mask with two writers.
- What the new controllers do need is small, and it is mostly at the edges of the layer: the platform entry seam is x86-shaped, the table has one chip and is built once, and `set_affinity` is a stub that answers Ok.

The riscv side has its own plan. `.agents/tasks/riscv-kernel/PLAN.md` step 5 writes "a PLIC `irq::Chip`" against `kernel/irq` as it is, step 10 sends IPIs through SBI, and `.agents/tasks/arch-platform-seams/DESIGN.md` has the interrupt entry as an open decision (P-irq, refactor item R19) because a PLIC gives the number only when the hart claims it. Those branches have no `irq::Domain` implementation, no `interrupt_map` and no acpi-route, since they were cut before the interrupt-system work of `.agents/tasks/interrupt-system/`. This design is the contract the two lines of work meet at.

## Goals and non-goals

Goals:
- One line layer that carries the IO-APIC, GIC SPIs (v2 and v3), the PLIC and a wired APLIC with the five duties kept in one place.
- The kernel and user-space split of interrupt controllers stated as a test, so that a controller nobody has met yet is placed by it, and not by a list.
- Cascaded controllers (a GPIO bank, a mux, an expander) need no kernel code beyond one signal on the virtual interrupt.
- The riscv branch writes its PLIC chip and its entry against a stated contract, and the two branches do not edit the same seam twice.

Non-goals:
- MSI controllers (ITS, IMSIC, APLIC in MSI mode). Only: nothing here may make them a rewrite.
- GICv5 and extended SPI numbering. The first arm cut refuses them.
- Per-CPU interrupts inside the layer (SGI, PPI, the hart's local causes, the timers and IPIs).
- Deleting or reshaping the pieces that serve no new controller (`Shared`, `Permanent`, `issue_ack`, `startup`, `dump`, the x86 local APIC chip). They are scaffolding under the hands-off rule.
- Any non-x86 controller code, a device-tree route service, the Driver Manager, performance work.

## Design

### Where the kernel ends and user space begins

A controller belongs to the kernel if any of these holds:

- K1. It is touched on the trap path or while a CPU comes up: claim or acknowledge, end of interrupt, threshold, per-hart enable, the CPU's own registers.
- K2. It allocates or aims a delivery resource that is shared across protection domains: vectors, LPIs, EIIDs, an ITS device table, an APLIC target.
- K3. It gives a root line its identity, its trigger form and its hold until the ack.

By these tests the IO-APIC, the GIC distributor, redistributor and CPU interface, the ITS, the PLIC, an APLIC and an IMSIC are the kernel's. K3 is what keeps the GIC distributor and a wired APLIC there although neither is on the trap path. A controller that is only MMIO behind a root line is user space's: a GPIO bank, an expander on I2C or SPI, and an interrupt router whose output is a root line. Two carve-outs. A user-owned router that can re-aim root lines has root-class trust, and the kernel's MMIO deny list (32 page-granular ranges) is what bounds it. Lines the kernel itself consumes (the console UART, an SMMU, a timer frame) are reachable by number from firmware tables, so `interrupt_map` has to answer for them (D4).

The user's own rule of 2026-10-30, recorded on `origin/feat/riscv` in `references/layering.md`, puts the local APIC and the riscv hart's timer, IPIs and start and stop in `kernel/arch`, every other chip (IO-APIC, HPET, PLIC) in `kernel/driver/<class>/<chip>`, and the board in `kernel/platform/<board>`. K1 to K3 say which of those chips are the kernel's at all; the rule says where they live.

### What stays and what changes

| Piece | Verdict | Why | Where |
|---|---|---|---|
| `Desc`, `Action`, the Oneshot held bit, the detach barrier, L1 across the flow and the EOI | Stays | The same shape as the usual fast-EOI flow. EOI is on the CPU that took the interrupt, inside the handler, and each source has one effective target CPU, so L1 stays uncontended (inference). | `kernel/kernel/irq/desc.cpp` |
| `Flow` with two members | Stays | `FastEoi` really means level. GIC SPIs and PLIC sources of both modes run it; the difference is only whether Oneshot holds the line. | `kernel/include/ours/irq/types.hpp` |
| `Chip` as a runtime table | Stays | Four implementers are in sight: the IO-APIC, the GIC, the PLIC and, after it, an APLIC. Whether one image chooses at boot is D7. | `kernel/include/ours/irq/chip.hpp` |
| `Domain`, linear | Stays, per controller | Dense spaces fit: GIC SPI 32 to 1019, PLIC and APLIC 1 to N with 0 reserved. It is wrong for LPIs (8192 and up) and other sparse spaces, which are MSI's. | `kernel/include/ours/irq/domain.hpp` |
| The platform entry seam | Changes (step 2) | `kernel/platform/include/ours/platform/interrupt.hpp:8` includes the x86 arch header; arch brackets every vector with start and finish (`faults.cpp:438`, `:440`); the preamble ignores its number (`kernel/kernel/irq/mod.cpp:112`); the platform ends a no-line interrupt. | see below |
| `DescTable` with one chip, built once | Changes (step 4) | `DescTable::init` (`desc_table.cpp:39`) binds one chip to every line and refuses a second call. | see below |
| `Chip::set_affinity` and `irq::set_affinity` | Changes (step 3, needs the user) | The stub answers Ok and does nothing (`kernel/kernel/irq/mod.cpp:69`); every target controller picks one effective CPU. | hands-off |
| `Chip::configure` for a controller with no mode register | Changes (step 3) | PLIC has no trigger register. The fact-check refuted the first reading that a level PLIC source would storm on the edge flow: `attach` with a mode calls `configure` and re-selects the flow (`desc.cpp:157-168`), and `attach` without a mode answers NotSupported when the chip has no `trigger_mode` (`desc.cpp:141-143`). What is missing is only the statement of what `configure` does on such a chip. | see below |
| `Virtual` interrupt | Changes (step 5) | The controller of a cascade cannot learn that a level child was acked: `unmask_interrupt()` is empty (`virtual_interrupt_dispatcher.hpp:35`) and the header says the signal is "left out: nothing here asks for that yet". | see below |
| `Data::platform_data`, `startup`, `issue_ack`, `dump`, `Shared`, `Permanent` | Stay unused | No new controller needs them. x86 already serves several IO-APICs by finding the instance from the GSI (`io_apic.cpp:72`). | hands-off |

### The interrupt entry

The three architectures find the interrupt differently, and none but x86 does it by arithmetic on a vector:

| Architecture | How the number is found | What ends it |
|---|---|---|
| x86 | The vector in the trap frame, less the platform's base | The local APIC EOI |
| arm GICv2 and GICv3 | A read of the acknowledge register, which also takes the interrupt | A write of the same value back; on v3 the acknowledging CPU interface |
| riscv PLIC | A read of this hart's claim register | A write of the id back to the same register |

Events of the entry as proposed (D3). Objects first: the trap entry of the architecture; the platform's decoder, which belongs to the interrupt controller's driver; the line layer; the per-CPU handlers of the architecture (timer, IPI).

1. The trap enters arch, which calls `irq::handle_irq_preamble` once for the trap.
2. Arch calls a numberless `platform::handle_irq(frame)`, with a frame type that is neutral.
3. The decoder identifies the interrupt (the vector, an acknowledge read, a claim). It loops while the controller has more, since one trap can carry several interrupts on arm and riscv.
4. It drops what is not an interrupt (GIC ids 1020 to 1023, a claim of 0), and sends a per-CPU interrupt (INTID below 32, the hart's local causes) to the architecture's own handler. Those never reach the line layer, as x86's timer and IPI vectors do not (`faults.cpp:253-306`).
5. For the rest it calls `irq::handle_irq_generic(hwirq)`, which answers whether a line ended the interrupt.
6. If no line had it, the decoder ends it itself: the GIC deactivates it, a PLIC must complete it. The reference PLIC handler only warns and leaves the source wedged in the gateway, which is the case this clause is for.
7. Arch calls `irq::handle_irq_postamble` and takes the preemption decision.

Moving the no-line EOI from arch (`faults.cpp:261-263`) to the platform changes a recorded genirq detail (`.claude/skills/ours-kernel-design/references/task.md` lines 101-104), so it is the user's decision. This is P-irq (a) of arch-platform-seams, and its refactor item R19.

### The chip contract

Clauses the chips of all architectures keep (step 3 writes them into `chip.hpp`, where lines 34-40 are x86's today):

- `mask`, `unmask` and `issue_eoi` are called under L1 in interrupt context and do no synchronous work on another CPU. A bounded poll (the GICv3 redistributor's write-pending bit) is fine. A controller whose mask is deferred to the target CPU (IMSIC) has edge lines only, and `check_oneshot` already refuses a Oneshot action on a chip without a synchronous mask.
- `issue_eoi` runs on the CPU that took the interrupt, inside `handle_irq`. It is never deferred to `finalize_oneshot` or to a thread.
- `configure` refuses with NotSupported a mode the controller cannot do (a GIC SPI takes level-high and edge-rising only). On a controller with no mode register it records and checks. The route service gives the mode that firmware gives, so non-x86 platforms always create an interrupt with an explicit mode.
- A chip with several instances finds its instance from `hirqnum`, as the IO-APIC does.
- `set_affinity` answers NotSupported until it is real. When it is, it returns the CPU it chose.
- A PLIC chip masks with the source's priority register (0 and 1) and keeps the enable bits set, so a completion for a masked source is still valid.

### Tables and numbering

One `DescTable` per controller, a `Domain` per table, and a small global directory from virq range to table, so that two controllers (a second PLIC, a GIC beside its ITS later) are expressible. Registration replaces the one-shot `init_lines` and may come after boot. The numbering of hardware sources is the platform's single space: a controller owns `[base, base + n)`, as ACPI's `gsi_base` does for a PLIC or an APLIC, and a GIC's number is its INTID (ACPI on arm calls the INTID the GSI). The interrupt path is reached through the decoder, which resolves a `Desc` or a controller and a local id, so that nothing in the public interface promises an array indexed by hardware number. That is what keeps MSI from being a rewrite. The details are written when step 4 is introduced.

### Cascades in user space

Objects: the parent, a physical interrupt on a root line, level, Oneshot; the GPIO driver G, which holds the parent and the bank's MMIO; one virtual interrupt per pin, made by G; a client K, which holds one pin's interrupt and does not know it is virtual; the root controller's flow in the kernel.

Events for a level child on a level parent:

1. Pin i asserts, so the parent line asserts.
2. The kernel's flow masks the parent, runs the action, and holds the line masked until G acks.
3. G wakes, reads the bank's status, and calls `interrupt_trigger` on pin i's virtual interrupt.
4. K wakes and clears the cause in its own device.
5. K acks (the next `interrupt_wait`, or `interrupt_ack` on a port). G does not see this.
6. G acks the parent, and the kernel unmasks the line.

Step 5 is the gap. An edge child is fine, since G clears the latch when it forwards. A level child has to stay masked in G's own register until K's ack, and G cannot know when that is. The proposal (D5) is one signal on the virtual interrupt, raised when its ack clears the debt, which is where the base class already calls `unmask_interrupt()`. G waits for it, then unmasks the pin. K then holds an ordinary interrupt and calls the same wait, bind and ack as for a physical one. The handle given to K has Wait, Read and Transfer but not Signal, or K could raise its own interrupts. The position of the signal bit is ABI, so it is fixed with the step. The alternative, a message from K to G, makes K tell the two kinds apart.

Nothing else in the kernel changes for GPIO. The three shapes seen in Linux (one parent source per pin, one summary line per bank with a status register, a mux with a fixed parent) all reduce to a parent from `interrupt_map` and `interrupt_create` plus a virtual interrupt per pin. A leaf pin has no kernel number.

### The user ABI

`interrupt_map(resource, source, out virq)` keeps its one number. Its meaning becomes: the root controller's architectural number in the platform's single numbering. On arm that is the INTID for SPIs (ACPI's GSI as is, device-tree cells with 32 added by the route service). On riscv it is the PLIC or APLIC source id (plus `gsi_base` under ACPI). SGIs, PPIs, the hart's local causes and the IMSIC's IPI id are never sources. Decoding device-tree cells is keyed by the controller's compatible string and lives in a user-space route service, which holds a read-only copy of the device tree as acpi-route holds the ACPI tables. Its answer must be tagged from the start, `Root{source, trigger, polarity}` or `Leaf{controller, cells}`, since a chain of `interrupts-extended` can end at a GPIO controller. That service is a later task. A kernel-owned source answers AccessDenied, where today it answers NoResource or AlreadyBound, so the route service can tell "exists, the kernel's" from "no such pin" (D4).

### Hardware risks

- The Oneshot level flow masks a line after the decoder's claim and before the end of interrupt. On a PLIC that is a mask while a claim is open. Linux does it; seL4 reports it unreliable on some hardware and so completes at the ack instead. QEMU will not show it, so it is a checklist item for real silicon. If it fails, the hold becomes a chip operation, and the completion has to reach the claiming hart.
- Every GICv3 mask polls the redistributor's write-pending bit in interrupt context. Any change to that goes through a performance report first.

## Alternatives considered

- **Remove `kernel/irq` and fold it into `platform/pc`.** Rejected: it is a move, not a deletion (above), it loses what arm and riscv would reuse, and it reverses decisions of 2026-10-05.
- **Shrink it in place now** (Domain to a fixed offset, Flow to a bit, the Chip table to link-time functions, a single action slot; about a quarter of the code). Rejected on 2026-11-01: it was argued when riscv and arm were placeholders. With the second and third controllers coming, the Chip table, the Domain and the flows have users. `Shared`, `Permanent`, `issue_ack`, `startup` and `dump` are left as they are for the hands-off rule, not because they are used.
- **A thin per-vector slot with the mask a property of the object kind.** Rejected: the five duties come back, and the level mask has two writers again (irq-genirq D1).
- **Per-CPU interrupts as lines in `Desc`.** Rejected: every CPU would take one global lock per tick, or the layer would need a lockless permanent path plus a per-CPU enable that `Data`, which carries no CPU, cannot express. They also cannot be held by a user program.
- **Cascaded controllers in the kernel**, as Linux's chained handlers and hierarchical domains. Rejected: a leaf's mask register is user-owned MMIO that interrupt context under L1 cannot reach, and the leaf needs no kernel number.
- **The ack of a level child as a message from the client to the controller driver.** Rejected: the client must not tell a virtual interrupt from a physical one.
- **A firmware-neutral specifier in `interrupt_map` now** (interrupt-system D11 (b)). Deferred: one number is enough while a platform has one numbering. It returns if a device-tree platform has two root controllers whose numbers collide and nobody owns the flattening.
- **The mode of a line owned by `Desc`** rather than by the controller. Not needed: the refutation above. Revisit with the PLIC code.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. The test for the kernel and user split | K1 to K3; the root controller only; by privilege alone | K1 to K3 | None | |
| D2. Per-CPU interrupts | out of the line layer, arch-owned; lines in `Desc` | Out | None | In part: the user's rule of 2026-10-30 puts the local APIC and the riscv hart timer, IPIs and start and stop in arch. Reading it for the GIC's SGIs and PPIs is open |
| D3. The interrupt entry (arch-platform-seams P-irq) | (a) a numberless `platform::handle_irq(frame)` with the decoder owning spurious ids and the no-line end; (b) two entries; (c) the controller's driver installs the entry | (a) | None | |
| D4. The user number, and what a kernel-owned source answers | one number in a platform-wide numbering (`gsi_base` style) with a tagged route answer; a firmware-neutral specifier now. AccessDenied or NoResource | One number; AccessDenied | None | |
| D5. How the controller of a cascade learns a level child was acked | (a) a signal on the virtual interrupt; (b) a message from the client; (c) a new call | (a) | None | |
| D6. Where the level hold lives | in `Desc`, as now; as a chip operation | In `Desc`, with the PLIC check on silicon | None | |
| D7. `Chip` runtime table or build time | runtime; one image per platform and link-time functions | Runtime | None | |
| D8. The hands-off pieces | change only the `set_affinity` stub to NotSupported; leave the rest | That | None | |
| D9. Merge order, and which branch carries D3 | `feat/driver-caps` first, then the riscv slices rebase; the other way | driver-caps first; D3 once, on the branch the user names | None | |

## Testing and verification

- Every step that touches `kernel/irq` or the x86 entry: `ours-verify` on both images, and the kernel-shell tests that hold the layer's races (`irq.test`, `interruptline.test`, `pinlines.test`, `irqdomain.test`). `pinlines.test` is not in the boot gate, so it is run by hand.
- Step 4 adds a test with two tables, so that a second controller is exercised without a second piece of hardware. Step 5 adds a host or kernel-shell test of the signal on a virtual interrupt, including the order of unmask against trigger.
- Cannot be verified here: arm and riscv (nothing builds them in this tree's gate), a PLIC or a GIC on silicon, the mask-during-claim ordering, and any interrupt from a real GPIO.

## Open questions

- Does the riscv branch target the PLIC and SBI (its plan says so) or AIA with an APLIC and IMSIC on the same image? D7 depends on it.
- Whose is the flattening of numbers for a device-tree platform with two or more root controllers: a rule published by the kernel, or one the route service makes up?
- Does the device tree handed to user space need its `/chosen` node scrubbed (boot arguments, random seeds)? From memory, unchecked.
- What shape does the MSI call take: the message returned to the caller, or written by the kernel into a VMO? It needs a device identity for an ITS.
- Does a Virtual interrupt's ack require a bound port today (`interrupt_ack` does; the wait is the ack when unbound)? Step 5 settles the signal for both paths.
- GICv5 packs the interrupt type into the high bits of the number, which breaks any array indexed by hardware number. Refused as NotSupported in the first arm cut?
