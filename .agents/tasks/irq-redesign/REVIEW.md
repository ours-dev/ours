# irq-redesign: final review of the generic IRQ rewrite

**Date**: 2026-11-03

## Scope and method

Reviewed the current public IRQ interfaces, private line/domain/directory/entry implementations, physical interrupt-object completion path, controller adapter callers, syscall numeric boundaries and regression coverage. Applied kernel-design, codebase-design, modern-cpp-development, cpp-code-review and cpp-write-and-verify. Read actual Guard/state accesses and ownership transitions; checked compile-time type rejection, bounded multi-CPU stress, ordered fake-controller traces, both boot images and counterfactual regression failures. The accepted design records reference-kernel comparisons and this project's deliberate differences. Fair was not edited.

## What is done well

- kernel/kernel/irq/line.cpp:357 and kernel/kernel/irq/line.hpp:272: every dispatch uses the real Guard; mutable flags, trigger and registrations are checked under L1, including Permanent. Callback invocation is private and notify_locked requires L1.
- kernel/kernel/irq/line.cpp:290 and kernel/object/interrupt_dispatcher.cpp:34: completion revalidates current owner debt under L1 -> L2 before clearing Held. The deterministic ack tests distinguish the fixed protocol from the old behavior.
- kernel/kernel/irq/domain.cpp:26: local Vec/Box storage provides transactional construction and one owner per line without manual destroy/reset paths.
- kernel/kernel/irq/domain_registry.cpp:102: revocation, unpublication and reader grace precede reclamation; transfer to a retired Box occurs under the writer Guard, with destruction outside it. Another writer cannot inspect a freed id.
- kernel/kernel/irq/domain_registry.cpp:199 and kernel/kernel/irq/line_access.hpp:21: public dispatch and ordinary control own their complete reader scopes; no public domain pointer escapes.
- kernel/include/ours/irq/controller.hpp:67: binding validates handshake independently of trigger, and deferred holding requires an explicit synchronous-mask promise. Ordered tests cover both protocols over Edge/Level rather than assuming another controller behaves like IO-APIC.
- system/public/ours/types.hpp:14: HIrqNum and VIrqNum retain the user's spelling as distinct scoped types; real interfaces reject mixing and syscall encoding remains u32.
- kernel/kernel/irq/entry.cpp:18: nested entry saves the outer frame on the existing stack and exit restores it with balanced disable counts. A counterfactual demonstrates the new test catches unconditional clearing.

## High severity

None found in the rewritten generic core after the verified fixes. This conclusion is bounded by the documented contracts and exercised x86 configuration, not a proof of all hardware or all interleavings.

## Medium severity

None introduced or left unresolved in the rewritten core. The existing platform ISA override priority issue is still recorded in docs/repairs/261004/kernel-irq.md item 1; it belongs to firmware route arbitration, whose algorithm this cut does not replace. The shared checkout's independent address-type migration also does not compile yet and is excluded from the isolated IRQ verification.

## Minor and style

- Global named lockdep classes remain unavailable in the underlying library, as recorded in docs/repairs/261004/kernel-irq.md item 2. All IRQ paths use its actual Guard and compiler capabilities; no runtime class-order validation is claimed.
- Existing transitive/header diagnostics and older HPET/page-fault symbol naming diagnostics remain outside changed lines. Every new changed-line diagnostic encountered during this rewrite was fixed.
- Private implementation capacity remains eight domains. Failure is explicit NoSpace and the storage choice is hidden behind the endpoint; future expansion does not require a public manager or pointer API.

## Teaching opportunities

None. The user suspended learning-oriented arguments and requested focused development; engineering invariants and alternatives remain in the accepted design and change records.

## Suggested follow-up order

1. Integrate the independently completed address-type migration separately; do not absorb it into IRQ history.
2. Resolve platform ISA override precedence using its existing repair record and a routing-specific regression.
3. Implement named lockdep classes in their owning library, then exercise deliberate order inversions.
4. Add a new controller only with a concrete handshake, mask-effect contract and adapter-specific tests; measure any performance work separately before optimizing.
