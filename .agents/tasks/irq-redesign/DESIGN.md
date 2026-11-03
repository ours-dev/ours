# irq-redesign: proposed ownership and interfaces for interrupt lines

**Date**: 2026-11-03
**Status**: Proposed

## Problem

The user asked to review kernel/irq and redesign it before rewriting implementation. The implementation is in `kernel/kernel/irq/`, with interfaces in `kernel/include/ours/irq/`. REVIEW.md records the evidence.

There is real design to preserve: one owner of a level line's Oneshot mask, a callback-lifetime barrier at detach, no reused virqs, root-controller ownership of unmatched EOI, and IRQ RCU reclamation. The structural problem is that Domain means a mapping, DescTable owns storage and forwards controls, DomainRegistry owns the lifetime, and callers can see all three. Short generic names and integer aliases make this harder to understand.

This document is the first design cut, for the module's objects and seams. The user accepted D1 (b) on 2026-11-03: explicit Line, Registration, Domain and ControllerOps, with Domain owning mapping, storage and retirement. The user's subsequent active goal authorizes implementation of the full refactor, and the user suspended teaching-oriented arguments on 2026-11-03. Engineering decisions and their verification are recorded here and in STATUS.md.

## Goals and non-goals

- Give every state transition one owner and give callers a small Interface that hides lookup, line locks, and reclamation.
- Make hardware source identity and kernel line identity distinct types inside the kernel while preserving the current u32 syscall representation.
- Preserve existing userspace interrupt semantics, including virtual interrupts, exclusive physical objects, NeedAck, coalescing, port delivery, revocation, and timestamps.
- Preserve domains registered and removed after boot, globally unique never-reused virqs, domain ids chosen by registrants, root id 0, and the existing x86 GSI domain.
- Honor the user's explicit guard requirement on every dispatch, including Permanent lines: no TA_NO_THREAD_SAFETY_ANALYSIS escape around shared line state. Preserve the proven L1 -> L2 lock order. This reopens the older Permanent bypass under the user's newer requirement; no performance improvement is claimed.
- Do not add MSI, affinity, hierarchical domains, kernel interrupt threads, disable depth, lazy disable, resend, new controllers, or a new userspace ABI.
- Do not remove reserved hooks or initialization scaffolding. Do not modify the user's scheduler.

## Design

### Objects

D1 (b)'s object vocabulary and consolidated domain ownership were accepted on 2026-11-03. The detailed Interfaces below remain subject to D2 and D3.

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

Domain now owns one private Storage aggregate of mapping vectors and boxed Lines; DescTable is removed. The registry remains an internal implementation that locates domains and allocates globally unique virq blocks. It does not become a new public IrqManager. A Domain may internally compose a map and a line store, but those no longer repeat the whole caller control surface.

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

The retained order is L1 (line) -> L2 (interrupt object), with controller register locks and the port interrupt queue taken only in their existing allowed places. Ack and wait must not add L2 -> L1. No handler may recursively enter a line control operation. Permanent registrations remain immutable and now dispatch through the same L1 Guard as every other line. Lifetime policy does not select a synchronization protocol.

Two different lifetimes remain explicit: detach protects the callback and its context; IRQ RCU protects the domain, mapping, and line storage. Neither substitutes for the other. Hiding the table does not remove either guarantee.

The user suspended teaching-oriented arguments and requested development to continue. D2 must be resolved by an implementation argument and deterministic tests for the old-ack/new-dispatch ordering. Compare generation-bearing completion, revalidation under L1 -> L2, and controller-specific exclusion; do not wait for a learning exercise.

### Completion protocol and proof

The selected mechanism is owner-state revalidation under L1 -> L2. Registration::Callbacks contains handle, revoke and ready over one context. Every deferred registration must supply ready. Line completion holds L1, confirms the registration is still attached, queries ready, and only then clears Held and unmasks. The physical object's query holds L2 and returns true only in Idle or Waiting; Destroyed retires through detach.

The invariant is that completion cannot reopen a deferred level line while newer delivered work still awaits service. If a new dispatch precedes the recheck, its owner-state update is visible under L2 and readiness is false. If it follows the recheck, it cannot enter until L1 is released, so its mask/Held update follows the old completion. The handler and the readiness query both take L1 -> L2. Ack and wait release L2 before calling completion, so no reverse edge is introduced. Detach drains handlers; the owner must quiesce its own completion calls before reusing a registration or freeing its context.

| Alternative | State and Interface cost | Decision |
|---|---|---|
| Owner-state revalidation under L1 -> L2 | One readiness pointer; one bounded query of existing owner state; no extra pending-work word | Selected: matches the current guarded-dispatch model |
| Generation-bearing event completion | Generation storage and token propagation through coalescing, wait, port resend and acknowledgement | Rejected: introduces another work-identity protocol for state the owner already knows |
| Exclude the ordering by controller behavior | Depends on each controller's masking/pending guarantees and cannot cover synthetic dispatch | Rejected as a general line Interface |
| Call completion while holding L2 | Reverses L1 -> L2 and can deadlock with dispatch | Rejected |

The unified test hook has BeforeCompletion and BeforePark phases, both outside L2. interruptline.test injects new work before completion for port ack and wait-as-ack. Removing only the readiness gate causes both new assertions to fail; restoring it passes. This is a verified synthetic interleaving, not a claim of a reproduced physical-controller failure.

### Controller endpoint and registry ownership

The public DomainEndpoint is controller-owned and noncopyable. It is registered with the source set, immutable operations and instance context. dispatch(source) covers acquisition and line flow with an IRQ RCU guard; it returns Some when the source was found and ended, otherwise None so the root can end an unmatched interrupt. The owning Domain and directory headers are private. The public API never returns a Domain or Line pointer.

The directory owns Box<Entry>; each entry owns Box<Domain> and borrows its endpoint. Removing slots keep their entry until revocation, retraction and reader grace complete. Ownership is moved out and the slot freed atomically under the writer Guard; destruction runs outside the lock. Another writer never reads an entry already freed. The directory destructor requires quiescent users and does not dereference borrowed endpoints. Context and operations outlive successful unregistration; the endpoint outlives all delivery calls.

### Typed identities and numeric boundaries

HardwareIrq identifies a source only within its domain; IrqId identifies a kernel line and is never reused after withdrawal. Both are scoped u32 enums, mutually non-convertible and not implicitly convertible to/from integers. Each has its own Invalid. Numeric indexing, vector encoding, diagnostics and syscall wire conversion are explicit. Physical interrupt-object creation accepts IrqId; syscalls keep their existing u32 wire format and resource validation. Directed IO-APIC completion takes IrqVec, a third independent type. Compile-time assertions enforce the distinction at real interfaces.

A wrapper with arithmetic/conversion operators was rejected because identity arithmetic should instead be explicit numeric indexing. Aliases with disjoint value ranges were rejected because hardware domains overlap. Fundamental types do not depend on a formatting library; logging serializes them at the boundary.

### Common control resolution

with_line(source, id, operation) owns the IRQ RCU scope, invokes source.find_line(id, guard) and performs the operation before the scope closes. Its structural constraint accepts callable operations on Line; the return contract must be Status or a status Result. Domain and DomainRegistry implement the private resolver shape. The public ordinary controls and tests of private domains use the same mechanism. Domain's duplicate ordinary control facade is deleted. Special detach, completion and revocation semantics remain explicit.

### Controller protocol

Electrical trigger and receiving-CPU handshake are separate. Line caches TriggerMode under L1 to decide whether a deferred level source must stay masked; the duplicate Flow enum and separate Edge/FastEoi handler bodies are removed. ControllerOps declares DispatchProtocol::Eoi or AckEoi. Eoi completes after callbacks; AckEoi acknowledges the known source before callbacks and completes afterward. Claim reads that identify a source remain in the controller entry. AckEoi requires acknowledgment to retain the in-service state until EOI; a mask-before-ack controller or one needing a claim token is not supported by this protocol. A new protocol must define and test that sequence explicitly.

Binding validates the declaration, mandatory mask/EOI, absence of an unused ACK on Eoi and presence of ACK on AckEoi. Synthetic delivery performs neither ACK nor EOI. All real source paths, including unclaimed and held lines, complete on the receiving CPU under L1. Completion of deferred work never acknowledges or ends again.

Masking::Synchronous explicitly promises that delivery is stopped before mask returns. Deferred registration requires this promise and mask/unmask hooks; bare pointer presence is insufficient. Unspecified masking remains valid for ordinary handlers. Unknown protocols/capabilities fail before binding. IO-APIC declares Eoi with synchronous masking; fake adapters verify both handshakes independently over Edge and Level. No GIC, PLIC or MSI support is claimed.

Unused startup, dump and affinity hooks are removed instead of advertising unimplemented behavior. The public affinity stub and its ignored timer call are removed; timer routing remains as established by the platform. The dead IO-APIC EOI-as-ACK adapter is removed and never enters the new AckEoi protocol.

Compared alternatives: adapter-supplied arbitrary flow callbacks move shared masking and lifetime invariants out of Line; a full pending/replay state machine has no demonstrated current requirement; deriving handshake from Edge/Level silently conflates independent facts. A small validated protocol enum keeps these costs bounded and leaves future sequences local to the controller contract.

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
| D1. Object vocabulary and domain ownership | (a) keep genirq names and hide the current machinery, with one internal domain lifetime owner; (b) Line, Registration, Domain and ControllerOps, with Domain owning mapping, storage and retirement | (b), for explicit objects and a smaller caller Interface; retain genirq counterparts in documentation | Not supplied | (b), 2026-11-03: the user answered "B" |
| D2. Deferred completion invariant | Generation-bearing completion; owner-state revalidation; controller-specific exclusion | Owner-state revalidation under L1 -> L2 | Waived: the user suspended teaching exercises | Implemented after delegated engineering analysis, 2026-11-03; deterministic tests reject the old behavior |
| D3. Controller protocol selection | A validated protocol enum; adapter-selected internal flow; a deliberately narrow wired-controller contract | Validated Eoi/AckEoi protocol and explicit synchronous-mask capability | Waived: teaching exercises suspended | Implemented after engineering comparison, 2026-11-03 |

D1 is decided and its object vocabulary is migrated. The full six-part goal and current evidence are tracked in STATUS.md. The user has suspended learning exercises; D2 and D3 are engineering work, not user-input gates.

## Testing and verification

The unchanged baseline builds and boots both dev images in `out/irq-design-review`; 161 host tests pass. All six targeted suites pass: irq.test, irqchip.test, irqrevoke.test, irqregistry.test, interrupt.test, and interruptline.test. These results establish a migration baseline, not acceptance of a future rewrite.

A later concrete design must specify tests for mixed-number rejection, empty-line invalid policies, source lookup, attach/configure atomicity, one receiving-CPU EOI on claimed/unclaimed/unmatched paths, detach versus callbacks, revocation versus destruction, and removal versus readers. The completion window is now forced by BeforeCompletion for both ack forms; the negative old-behavior run fails both checks.

Every implementation increment must run the repository formatter on changed lines, clang-tidy with the real build flags, the relevant kernel suites, and `verify` before its commit. No controller other than the current x86 adapter, no hardware timing, and no performance improvement is claimed by this proposal.

## Open questions

- D3: what is the minimal validated controller protocol, independently of electrical trigger mode?
- Where should trap bookkeeping live so that device-line control no longer exports exception-entry state?
- irq-multi-arch D6, D7 and D20 are not closed by this proposal: non-x86 hold semantics, operation-table selection, and notifying port-bound drivers of domain removal still need their own decisions.
