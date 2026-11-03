# irq-redesign: review of the interrupt line module

**Date**: 2026-11-03

## Scope and method

Reviewed all six implementation files in `kernel/kernel/irq/`, all nine headers in `kernel/include/ours/irq/`, and the PC root handler, IO-APIC adapter, physical interrupt object, object ack/wait paths, and relevant kernel tests. The original review preceded implementation. The user later authorized the full refactoring goal and suspended teaching exercises; current implementation evidence is tracked in STATUS.md.

Authorities: `kernel-design` and its `references/task.md`, `modern-cpp-development`, `cpp-code-review`, `codebase-design`, `.clang-format`, `.clang-tidy`, and the accepted decisions in `irq-genirq`, `interrupt-system`, and `irq-multi-arch`. The line layer deliberately follows genirq; the user object protocol follows Zircon. An accepted simplification is not a defect merely because Linux has more machinery.

Read primary reference implementations in the local checkouts: Linux `kernel/irq/chip.c` (`handle_fasteoi_irq`, `handle_edge_irq`), `kernel/irq/manage.c` (`irq_finalize_oneshot`), `kernel/irq/handle.c` (`handle_irq_event`), `include/linux/irqdesc.h`, and Zircon `kernel/platform/pc/interrupt_manager.rs` (`InterruptTableEntry::invoke_if_present`) and `kernel/dev/interrupt/include/dev/interrupt.h`.

Validation used the repository's LLVM 23 binaries and an isolated `out/irq-design-review` build. All 15 IRQ C++ files pass `clang-format --dry-run -Werror`. Clang-tidy results and runtime checks are recorded in RESULT.md. Findings below are source-based unless explicitly described as exercised; the baseline tests do not prove absence of races. Self-review of the naming increment checked all migrated declarations, include paths, GN source entries and caller types; no semantic change or new finding was identified. All 22 touched C++ files ran clang-tidy and the nine targeted suites passed.

## What is done well

- `kernel/kernel/irq/desc.cpp:272`: detach takes the same line lock as dispatch, giving callers a concrete callback-lifetime barrier.
- `kernel/kernel/irq/desc.cpp:518`: the line flow owns the level Oneshot mask; the user object reports deferred work instead of becoming another mask writer.
- `kernel/kernel/irq/desc.cpp:710`: revocation unlinks, calls the owner, and release-publishes detachment last. Clearing the action first would permit the owner to free callback context too early.
- `kernel/kernel/irq/domain_registry.cpp:168`: removal marks and revokes before unpublishing, waits for IRQ RCU readers, then frees storage. Never-reused virqs prevent a stale number from becoming another source.
- `kernel/platform/pc/interrupt.cpp:77`: the root handler ends an interrupt with no line; a line's flow ends one that it owns. The fallback is distinct from an unclaimed interrupt on an existing line.
- `kernel/commands/interrupt_line_test.cpp` and `kernel/commands/irq_registry_test.cpp`: tests exercise real entry, attachment races, revocation, and reclamation rather than only construction.

## High severity

None established on the exercised x86 paths. The fresh build, both boot checks, and all six targeted kernel suites passed. This is not a proof that the completion protocol in M2 is safe under every permitted delivery.

## Medium severity

### M1. Permanent and Shared are accepted together on an empty line

`kernel/kernel/irq/desc.cpp:206`, `:235`; `kernel/commands/irq_test.cpp:284`. The settled contract says a Permanent action cannot be Shared, but `check_oneshot()` validates incompatible flags only when Oneshot is set. On an empty line `attach()` therefore reaches `link_locked()`, which stores both Shared and Permanent. The existing test tries this action only on an occupied line, where the occupancy check hides the gap.

This does not currently allow a second action onto the immutable line, because Permanent takes precedence. It is an observable contract mismatch and leaves a supposedly rejected registration permanently attached. Validate registration policy as a whole, including the empty-line case; do not make Oneshot validation own unrelated policy rules. Resolved by the registration-policy increment: both attach forms now validate immutable registration policy first. Four new runtime checks pass in irq.test, bringing it to 76 checks.

### M2. Completion does not distinguish the deferred work being completed

`kernel/kernel/irq/desc.cpp:330`; `kernel/object/interrupt_dispatcher.cpp:465`, `:495`. The object clears its debt under L2, releases L2, then finalizes under L1. Finalize checks only that the same action is still attached and Oneshot, then unconditionally clears Held and unmasks. If a new dispatch is admitted in that gap, it can establish a new debt that the older completion clears.

This is an existing open question in `irq-genirq/DESIGN.md`, not a newly reproduced hardware failure. The source-level interleaving is: old ack sets Idle; another dispatch sets NeedAck and Held; old finalize clears Held. Whether an ordinary wired level interrupt can enter that window depends on the controller's masking and pending-delivery guarantees. Synthetic dispatch can express it; the current tests do not force the window before finalize.

Resolved by owner-state revalidation under L1 -> L2. Deterministic port-ack and wait-as-ack tests inject a new interrupt before completion; removing the readiness gate fails both assertions and restoring it passes. Merely renaming finalize to complete does not fix it. Preserve L1 -> L2: calling into the line while holding L2 would deadlock against dispatch. Linux's `irq_finalize_oneshot()` rechecks RUNTHREAD; its exact algorithm cannot be copied without accounting for ours holding L1 across callbacks. The user must write the concurrency argument before a replacement protocol is selected.

### M3. Owning storage is represented by copyable views

`kernel/include/ours/irq/domain.hpp:24`, `:84`; `kernel/include/ours/irq/desc_table.hpp:23`; `kernel/kernel/irq/desc_table.cpp:91`. Domain owns two allocations through Span fields, is implicitly copyable, has no owning destructor, and requires explicit reset. DescTable likewise owns pointers through a Span and requires explicit destroy. Table construction intentionally transfers ownership by copying a temporary Domain.

The current call sites manually maintain the convention, so this review does not establish a current double free. The old types allowed two apparent owners to reset the same arrays, or a forgotten reset to leak them. Resolved by the owning-Domain increment: private Vec and Box storage, deleted copy/move, scoped rollback, and destruction only after registry retirement. Refactoring these public types is unusually hazardous because ordinary C++ copying silently changes ownership. Consolidate the lifetime under one noncopyable owner and use existing owning containers or an explicit ownership transfer; keep reader reclamation a prerequisite for storage destruction.

### M4. Trigger mode silently selects a controller protocol

`kernel/kernel/irq/desc.cpp:220`, `:431`; `kernel/include/ours/irq/chip.hpp:25`; `kernel/kernel/irq/desc_table.cpp:50`. Edge selects one flow, Level selects FastEoi, and every published line must have mask and EOI operations. This is a deliberate, working IO-APIC model, but it is not a general consequence of edge versus level: controller acknowledgement, completion ordering, and safe masking are additional facts.

Linux's `handle_edge_irq()` acknowledges before its event loop; `handle_level_irq()` masks and acknowledges; `handle_fasteoi_irq()` uses an EOI-oriented controller protocol. Ours's Edge flow simply runs callbacks then sends EOI. The shared name does not imply Linux's pending/replay guarantees.

Treat current support as wired sources with synchronous masking and end-on-the-receiving-CPU semantics. Before adding another controller, make its supported dispatch protocol explicit and reject incompatible bindings. Do not claim GIC, PLIC, or MSI support from the presence of a function table alone. This is an extension hazard, not evidence that today's IO-APIC sequence is broken; irq-multi-arch D6 and D7 remain open.

### M5. Public interfaces expose three copies of one control surface

`kernel/kernel/irq/mod.cpp:78`, `:120`; `kernel/kernel/irq/desc_table.cpp:199`, `:271`; `kernel/include/ours/irq/desc.hpp:69`. Attach, mask, unmask, configure, and completion are repeated across the facade, table, and line. DescTable mostly resolves a number and forwards the operation, while Domain represents only the numbering maps and DomainRegistry owns the actual domain lifetime.

`kernel/include/ours/irq/domain_registry.hpp:25` also exposes a slot returning a raw DescTable pointer under an interrupt-state convention. This can be used correctly, as the PC handler does, but its interface requires controller code to know storage and reclamation details.

The module is shallow at these seams: several public types expose implementation machinery without removing knowledge from callers. Keep the kernel-caller and controller-facing interfaces separate, make line storage and lookups internal, and let one domain owner contain mapping, storage, and retirement. An opaque dispatch endpoint can keep the existing one-load interrupt path while hiding the table.

## Minor and style

- Naming first: Desc describes representation rather than the object it is; Line says what it owns. Action is familiar genirq vocabulary but Registration better describes the caller-owned binding and its lifetime. Data conceals controller-source identity; ControllerSource would say what the hooks receive. Chip combines operations and a name; ControllerOps identifies its role without implying a hardware instance.
- Keep `interrupt_*`, InterruptDispatcher, and the Irq resource vocabulary: the user explicitly retained them on 2026-11-01. A virtual interrupt object has no hardware line.
- `system/public/ours/types.hpp:14`: HIrqNum and VIrqNum are both aliases of u32, so a hardware number can be passed to a virq-taking function with no diagnostic. Deliberately disjoint numeric ranges catch some mistakes at runtime, but a hardware number of another domain may still equal a valid virq. Distinct kernel types should carry the distinction; the syscall wire format can stay u32.
- `kernel/kernel/irq/mod.cpp:268`: preamble/postamble and current_frame also cover exceptions, so placing them in the same interface as device-line control makes the word irq misleading. Give trap entry bookkeeping its own interface when placement is decided; no scheduler semantic change is needed.
- `kernel/include/ours/irq/mod.hpp:300`: synthetic dispatch and revocation helpers belong at a deliberate test seam. Current handle_irq_for_test already acquires an IrqRcuReadGuard that disables preemption, so the old repair alleging unprotected thread-context dispatch is stale for this facade. Direct calls to Desc::handle_irq still have context requirements.
- Reserved hooks and empty initialization functions are planned scaffolding. No deletion is recommended without reopening the recorded decision.
- Existing edge-finalize MMIO cost is already tracked in `docs/repairs/261007/kernel-irq.md`. No optimization or lock-granularity change belongs in this rewrite without measurement and the user's decision.

## Teaching opportunities

### T1. A line lock protects callback lifetime, not the truth of a late completion

`kernel/kernel/irq/desc.cpp:330`. The line owns Held and the action list under L1; the object owns NeedAck and its packet under L2. Dispatch goes L1 -> L2; ack drops L2 before taking L1. The lock prevents simultaneous line mutation but cannot identify which event an old completion acknowledges. A replacement must account for the old-ack/new-dispatch ordering without adding L2 -> L1. Compare Linux `irq_finalize_oneshot()` and `handle_irq_event()`, then account for their different callback lock scope.

### T2. A domain is useful when it owns a lifetime

`kernel/kernel/irq/domain_registry.cpp:137`. The mapping, descriptors, publication, revocation, and reclamation belong to one lifecycle. This is a natural Module with a deep Interface: registration publishes a complete domain; removal retires the whole domain. Internal maps and line objects may stay separate implementations without each becoming public caller knowledge. Linux's `irq_domain` naming is a reference, not a requirement to duplicate Linux's internal layout.

## Suggested follow-up order

1. Decide the object vocabulary and the domain owner's Interface. Record which existing decisions remain binding.
2. Review the completion invariant with the user's written concurrency argument before selecting its mechanism.
3. Design concrete interfaces and acceptance tests, then implement independently verified commits. The user authorized development after the original review; implementation progress is tracked in STATUS.md.
