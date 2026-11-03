# irq-redesign: proposed ownership and interfaces for interrupt lines

**Date**: 2026-11-03
**Status**: Proposed

## Problem

The user asked to review kernel/irq and redesign it before rewriting implementation. The implementation is in `kernel/kernel/irq/`, with interfaces in `kernel/include/ours/irq/`. REVIEW.md records the evidence.

There is real design to preserve: one owner of a level line's Oneshot mask, a callback-lifetime barrier at detach, no reused virqs, root-controller ownership of unmatched EOI, and IRQ RCU reclamation. The structural problem is that Domain means a mapping, DescTable owns storage and forwards controls, DomainRegistry owns the lifetime, and callers can see all three. Short generic names and integer aliases make this harder to understand.

This document is the first design cut, for the module's objects and seams. It is a candidate for discussion, not an accepted specification or an implementation roadmap. The completion protocol and controller protocol remain explicitly unresolved.

## Goals and non-goals

- Give every state transition one owner and give callers a small Interface that hides lookup, line locks, and reclamation.
- Make hardware source identity and kernel line identity distinct types inside the kernel while preserving the current u32 syscall representation.
- Preserve existing userspace interrupt semantics, including virtual interrupts, exclusive physical objects, NeedAck, coalescing, port delivery, revocation, and timestamps.
- Preserve domains registered and removed after boot, globally unique never-reused virqs, domain ids chosen by registrants, root id 0, and the existing x86 GSI domain.
- Preserve current locking and the Permanent dispatch path pending any explicit reopening. No performance claim, benchmark-driven optimization, or new lock-granularity decision is part of this cut.
- Do not add MSI, affinity, hierarchical domains, kernel interrupt threads, disable depth, lazy disable, resend, new controllers, or a new userspace ABI.
- Do not remove reserved hooks or initialization scaffolding. Do not modify the user's scheduler.

## Design

### Objects

The recommended vocabulary below is provisional until D1 is decided.

| Object | What it is | Owns | Exposes to |
|---|---|---|---|
| IrqId | Kernel-assigned identity of a line, never reused | A typed value | Kernel callers |
| HardwareIrq | A source number scoped to its domain | A typed value | Controller adapters and mapping |
| Registration | One caller-owned handler binding; replaces Action's role | Callback, context, registration policy, attachment identity | Kernel drivers and InterruptEventDispatcher |
| Line | One interrupt source's dispatch state; replaces Desc's role | L1, flow, registrations, Held and retirement state | IRQ implementation |
| Domain | One published set of sources with one lifetime | Hardware-to-kernel mapping, line storage, publication and retirement | Controller registration Interface; storage stays private |
| ControllerOps | Hardware operations at the controller seam; replaces Chip's role | No line policy; adapter state remains controller-owned | Controller adapters |
| ControllerSource | Immutable binding information handed to ControllerOps | Domain-scoped source identity and controller context | Controller adapters |
| DomainEndpoint | Opaque dispatch access published by a domain; replaces raw-table use through DomainSlot | Publication slot; no ownership of the controller | Root handler |

The registry remains an internal implementation that locates domains and allocates globally unique virq blocks. It does not become a new public IrqManager. A Domain may internally compose a map and a line store, but those no longer repeat the whole caller control surface.

### Interfaces and placement

The Module has two caller roles, expressed through separate headers rather than one omnibus mod.hpp.

| Interface | Caller must know | Behavior hidden behind it |
|---|---|---|
| Line control | IrqId, Registration, requested line mode, start/stop/completion contract | Registry lookup, read guard, line resolution, L1, controller locking |
| Controller integration | DomainId, supported hardware sources, ControllerOps, DomainEndpoint | Storage layout, virq allocation, publication, retirement and reclamation |
| Test support | A controlled dispatch injection and observable outcomes | EOI suppression and direct access to private tables |
| Trap bookkeeping | Entry frame and exit reschedule result | Per-CPU frame and preemption bookkeeping; placement to be decided |

Kernel caller operations remain attach, detach, mask, unmask, and completion of deferred work. Attachment with a mode stays one atomic ownership operation and leaves the line masked until the caller is ready. Existing sharing support stays; user objects remain exclusive. Final spelling and parameter types are a later concrete-interface decision, not permission to add a second compatibility facade.

Registration stays caller-owned, address-stable, and explicitly detached. No destructor may silently acquire L1, wait for a callback, or perform an implicit domain grace period. Invalid policy combinations, including Permanent with Shared or Oneshot, are rejected before publication. Domain storage is noncopyable; storage transfer must be explicit, and freeing a published domain is possible only after retirement and the grace period.

A root handler calls the endpoint with a HardwareIrq. The endpoint hides the table pointer and preserves the existing publication read, hardware-number lookup, and receiving-CPU completion. Its result must distinguish an absent source from an existing source whose callbacks returned None. An absent source is still ended by the root handler. The numeric value of a hardware source is never accepted as an IrqId by implicit conversion.

Retain `interrupt_*`, InterruptDispatcher, and the Irq resource name. They describe the user-visible object and capability. This design changes the internal line vocabulary, not that object model.

### Events

1. Controller discovery produces a source set and adapter binding. Domain construction builds mapping and line storage before publication.
2. Registration resolves an IrqId, checks policy and occupancy, settles the mode, links the registration, and returns with the source masked.
3. The owner starts the line. A deferred level line stays masked whenever the current dispatch still owes its owner service.
4. The root handler decodes the received source, dispatches through its endpoint, and ends an unmatched interrupt itself.
5. The line runs the selected protocol and callbacks. Device notification remains in InterruptEventDispatcher; the IRQ module knows no ports.
6. Completion reports that the deferred work has finished. The old-ack/new-dispatch handshake must be designed under D2 before its final Interface is specified.
7. Detach removes the registration and returns only after its callback can no longer execute.
8. Domain removal rejects unrevocable lines, marks all lines, revokes registrations, unpublishes endpoints, waits for readers, and destroys storage.

### Ownership and concurrency constraints

The retained order is L1 (line) -> L2 (interrupt object), with controller register locks and the port interrupt queue taken only in their existing allowed places. Ack and wait must not add L2 -> L1. No handler may recursively enter a line control operation. Permanent registrations remain immutable and their dispatch remains outside L1.

Two different lifetimes remain explicit: detach protects the callback and its context; IRQ RCU protects the domain, mapping, and line storage. Neither substitutes for the other. Hiding the table does not remove either guarantee.

D2 is a blocking design question, not a selected locking mechanism. The user first writes which invariant completion protects, the concurrent actors, and the worst old-ack/new-dispatch ordering. Only then should we compare a generation-bearing completion, revalidation under the existing lock order, or a proof that supported delivery excludes the ordering. No choice is made here.

### Controller protocol

Current support is the established IO-APIC contract: synchronous masking and receiving-CPU EOI after callbacks, with level Oneshot held masked until completion. Edge/Level describes electrical signaling; it must not imply that every controller has the same acknowledgement or completion sequence.

Before another controller is bound, its dispatch protocol must be stated and checked against the line flow. The design must distinguish electrical mode from the controller's acknowledgement/completion needs. Whether that becomes a small protocol enum, an internal flow selection supplied by the adapter, or a narrower ControllerOps contract is deferred under D3. Reserved hooks remain reserved; their presence is not evidence of support.

### References and deliberate differences

- Linux `include/linux/irqdesc.h:struct irq_desc`, `include/linux/irq.h:struct irq_chip`, and `kernel/irq/chip.c:handle_fasteoi_irq` identify the line, hardware operations, and flow roles. The proposed names describe ours's roles while the design keeps counterpart names here.
- Linux `kernel/irq/chip.c:handle_edge_irq` uses acknowledgement and pending handling beyond ours's current edge flow. This cut does not import that machinery.
- Zircon `kernel/platform/pc/interrupt_manager.rs:InterruptTableEntry::invoke_if_present` holds its entry lock across non-permanent callbacks and uses an immutable permanent path. Ours retains this callback-lifetime approach instead of Linux `kernel/irq/handle.c:handle_irq_event`, which drops the descriptor lock around callbacks.
- Linux `kernel/irq/manage.c:irq_finalize_oneshot` rechecks thread work under its synchronization protocol. D2 must explain ours's corresponding guarantee without copying an incompatible lock algorithm.
- The proposed Domain combines ownership locally; it does not introduce Linux's hierarchical domains or sparse descriptor tree. Existing irq-multi-arch D10-D19 still determine registration, numbering, revocation, and reclamation behavior.

## Alternatives considered

- Keep genirq vocabulary and only make Desc, DescTable, and DomainRegistry private. This has the smallest naming churn and a clear reference-system mapping; it still needs a single owner for storage and retirement. It is a credible alternative to D1, especially if reference familiarity matters more than names describing ours's objects.
- Recommended: use Line, Registration, Domain, and ControllerOps, and consolidate domain ownership while preserving existing behavior. This makes the object model readable without knowing Linux's internal abbreviations, and removes a public forwarding layer. It requires internal caller and test migration.
- Replace the current protocol with full Linux genirq, including callbacks outside L1, in-progress state, synchronization, pending replay, and disable depth. This changes callback lifetime and locking and solves requirements the user has not requested. It is a separate strategy decision, not a prerequisite for better module design.
- Fold the module into platform/pc or return mask ownership to the object. Both reopen explicit decisions and lose the shared line policy that currently serves interrupt objects. No evidence in this review justifies either departure.
- Add a public manager and wrapper objects over every existing type. This would add Interface without deleting caller knowledge. The candidate replaces the shallow seams instead.

## Decisions

| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Object vocabulary and domain ownership | (a) keep genirq names and hide the current machinery, with one internal domain lifetime owner; (b) Line, Registration, Domain and ControllerOps, with Domain owning mapping, storage and retirement | (b), for explicit objects and a smaller caller Interface; retain genirq counterparts in documentation | Pending: one line choosing a or b and predicting what caller knowledge disappears | |
| D2. Deferred completion invariant | Account for the old-ack/new-dispatch window by an event-aware protocol, revalidation, or a controller-specific exclusion proof | No mechanism selected; user writes the invariant and worst interleaving first | Pending after D1 | |
| D3. Controller protocol selection | A validated protocol enum; adapter-selected internal flow; a deliberately narrow wired-controller contract | Compare only when the concrete Interface is reviewed; preserve current x86 behavior now | Pending after D1 | |

Only D1 is presented for a decision in this round. D2 and D3 are recorded so the candidate does not silently treat them as solved.

## Testing and verification

The unchanged baseline builds and boots both dev images in `out/irq-design-review`; 161 host tests pass. All six targeted suites pass: irq.test, irqchip.test, irqrevoke.test, irqregistry.test, interrupt.test, and interruptline.test. These results establish a migration baseline, not acceptance of a future rewrite.

A later concrete design must specify tests for mixed-number rejection, empty-line invalid policies, source lookup, attach/configure atomicity, one receiving-CPU EOI on claimed/unclaimed/unmatched paths, detach versus callbacks, revocation versus destruction, and removal versus readers. It must also force the completion window before finalize, rather than only the existing after-finalize/before-park hook.

Every implementation increment must run the repository formatter on changed lines, clang-tidy with the real build flags, the relevant kernel suites, and `verify` before its commit. No controller other than the current x86 adapter, no hardware timing, and no performance improvement is claimed by this proposal.

## Open questions

- D1: does the user prefer source-system vocabulary or explicit domain-owned objects?
- D2: can a new real delivery be admitted between clearing the object's debt and completing the line on each supported controller, and how must synthetic delivery behave?
- D3: what is the minimal validated controller protocol, independently of electrical trigger mode?
- Where should trap bookkeeping live so that device-line control no longer exports exception-entry state?
- irq-multi-arch D6, D7 and D20 are not closed by this proposal: non-x86 hold semantics, operation-table selection, and notifying port-bound drivers of domain removal still need their own decisions.
