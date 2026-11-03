# kernel-irq changes

## Every control call resolves through the directory, and the root handler holds its table (2026-11-03)

**Files**
- `kernel/include/ours/irq/mod.hpp`, `kernel/kernel/irq/mod.cpp` — the facade over `DomainRegistry`: `init_lines()` registers the root domain and publishes its table into a `DomainSlot` the caller owns; `handle_irq_generic()` takes the handler's table; every control call opens a `task::IrqRcuReadGuard` and resolves its number through the directory; `root_table_for_test()` for tests that play an interrupt by hardware number.
- `kernel/platform/pc/interrupt.cpp` — the PC owns the root domain's slot (static, as the registry's contract asks) and its handler loads the table from it, one acquire load; `init_lines()` is handed the slot.
- `kernel/commands/pin_lines_test.cpp` — the no-line dispatch check passes `irq::root_table_for_test()`, the one test that needs the table itself.
- `.agents/tasks/irq-multi-arch/PLAN.md`, `DESIGN.md` — step 11 recorded.

**Symptom**
Latent, cannot be triggered today: only x86 exists. It would be triggered by the first second controller (a PLIC on riscv, a GIC on arm): with one machine-wide `DescTable` there is nowhere to register a second controller's lines, so `kernel/irq` could not carry them. This is step 11 of `.agents/tasks/irq-multi-arch/PLAN.md`; steps 8 to 10 built the directory, the read guard and the removal, and nothing in the tree used them.

**Cause**
Not a mistake on x86, a shape that did not survive a second controller. The machine's lines lived in one `DescTable`, built once by `init_lines()` and reached directly: a control call indexed it by virq and the interrupt path by GSI. That shape assumes what D10 (b), decided 2026-11-01, retired — that every line of the machine is made at boot and lives in one table — and D13 keeps it true on x86 only by making the IO-APIC's table the one real entry of the directory. The registry, its RCU readers and the removal machinery of steps 8 to 10 stood beside that table with no caller.

**Change**
The directory becomes the only path to a line. `init_lines()` is now the root controller's registration:

```cpp
// before
auto init_lines(ustl::views::Span<HIrqNum const> const hirqnums, Chip const &chip) -> Status {
    return get_desc_table()->init(hirqnums, chip);
}

// after
auto init_lines(ustl::views::Span<HIrqNum const> const hirqnums, Chip const &chip,
                DomainSlot &slot) -> Status {
    return get_domain_registry()->register_domain(DomainId::Root, hirqnums, chip, slot);
}
```

and a control call is one read guard around the directory load and the whole use of the line, as the design's read side has it — `find_table()` is `DescTable::find_line()`'s contract one directory up:

```cpp
// before
auto mask(VIrqNum const virqnum) -> Status {
    return get_desc_table()->mask(virqnum);
}

// after
auto mask(VIrqNum const virqnum) -> Status {
    task::IrqRcuReadGuard const guard;
    auto const table = find_table(virqnum, guard);   // InvArgs for INVALID_V_IRQ_NUM,
    if (table.is_err()) {                            // NoResource for a number no
        return table.unwrap_err();                   // registered domain owns
    }
    return (*table)->mask(virqnum);
}
```

The answers are unchanged, the pinned ones included: `InvArgs` for `INVALID_V_IRQ_NUM` (which `find_table()` keeps ahead of the lookup), `NoResource` for a number in no domain's block, `NotFound` from `detach()` for an action whose `virqnum()` is `INVALID` (it is in no block, and the removal's last touch of an action is storing exactly that), and a stale virq of a removed domain answers "no line", never another's, because the high-water mark never goes down.

The interrupt path does not read the directory. The handler owns its slot and loads the table from it; before and after, on the PC, for every device interrupt:

```cpp
// before: one static table, reached directly
if (!irq::handle_irq_generic(gsi)) {
    apic_issue_eoi();
}

// after: one acquire load of the slot the directory published
auto *const table = ROOT_SLOT.table_with_irqs_off();
if (!irq::handle_irq_generic(table, gsi)) {
    apic_issue_eoi();
}
```

so the hot path pays one acquire load of an atomic pointer and nothing else — no scan, no guard (the gate has interrupts off already, which is what makes the load a read of the directory's RCU scheme). A null table answers false and the handler ends the interrupt itself, the shape of a domain being taken away. `virq_of_hwirq()` resolves in the root domain, whose numbers are the platform's single numbering; a second domain's lines are asked for by domain and hardware number together, which is step 12's `interrupt_map(resource, domain, hwirq, out virq)`.

The three questions the step carried were put to the user with the introduction (2026-11-03) and decided as recommended: the registry keeps its eight places and its linear scan (both revisited when MSI, whose per-device domains could exceed eight, is built — the container change is contained in `domain_registry.cpp`); and the interrupt-path sample above is the before and after D15 asked to show.

**Reference**
Linux's `irq_domain` directory: `irq_domain_lookup`-shaped resolution of a controller's own numbers through a registered domain, with the controller's driver holding what its handler needs rather than asking a global (`struct irq_domain`, `irq_domain_add_linear()` in `kernel/irq/irqdomain.c`); ours' difference is that the handlers hold an `IrqRcuPointer` to the table instead of walking a hierarchy, because a cascade here belongs to user space (D1). The read guard and the rendezvous grace period are the project's own (D18), the kernel having no RCU; Zircon has no counterpart for the directory — its interrupt manager is one platform-owned table.

**Verification**
`boot_check.py`: dbg 45 passed and 4 skipped, rel 42 passed and 8 skipped with 6 of 6 required lines (the keyboard's interrupt reaching the user driver through the new slot path among them), 154 host tests. `clang-tidy` on the touched files reports nothing on the changed lines. The kernel-shell battery on the dbg image, four CPUs: `irqchip.test` 18, `irq.test` 72, `interruptline.test` 78, `pinlines.test` 28, `irqdomain.test` 87, `irqrcu.test` 34, `irqrevoke.test` 37, `irqregistry.test` 66, `interrupt.test` 67 checks — the counts the records of steps 3 to 10 pin. Mutations, each rebuilt and run on the dbg image: removing the `INVALID_V_IRQ_NUM` check of `find_table()` is killed by `pinlines.test` (its `line_status(INVALID_V_IRQ_NUM)` pins `InvArgs`); removing the empty-table check of `mask()` is killed by `irq.test`, which panics on the null dereference of a number in no domain. Not verifiable here: a second real controller, so the cross-domain resolution of a control call stays tested on synthetic registries (`irqregistry.test`), as D13 says.

**Deliberately not done**
- `set_affinity()` resolves nothing: it is the NotSupported stub whose contract, signature included, is settled by interrupt-system P4, and resolving first would change its answer for a number with no line.
- A guard-taking variant of the facade for `interrupt_map` to hold one read over `virq_of()` and `is_kernel_owned()`: each facade call is one guard, and two consecutive ones are correct — the virq that escapes is a plain number, safe because virqs are never reused, and `is_kernel_owned()`'s answer chooses only which refusal a program gets, `attach()` being the authority.
- Registering a second, synthetic domain in the machine's registry for the tests: it would consume one of the eight places for the life of the boot, for coverage D13 says belongs on a local registry, where `irqregistry.test` already has it.
- The capacity and lookup-structure revisit: put to the user, decided for now as keep (above).

**How it was found**
None.

**The user's prediction**
None — the step applied decisions already made (D10 (b), D13, D15); the user approved the introduction with its three open points ("可以") before it started.

**Three questions you should be able to answer**
- Why does the interrupt path take the table from a `DomainSlot` instead of the directory, and what exactly does that cost per interrupt?
- Why is it safe for a control call to keep a virq after its read guard closes, but not the `Desc` pointer it found?
- What keeps a stale virq of a removed domain from ever naming another domain's line?

## Name the IRQ objects by their owned roles (2026-11-03)

**Files**
- `kernel/include/ours/irq/line.hpp`, `registration.hpp`, `controller.hpp` — replace Desc, Action, Chip and Data with Line, Registration, ControllerOps and ControllerSource.
- `kernel/kernel/irq/line.cpp` and both IRQ and command GN targets — migrate filenames without retaining compatibility aliases.
- IRQ callers, interrupt objects and controller adapters — use the new types; rename the controller contract suite to irqcontroller.test.

**Symptom**
The names exposed storage representation or unspecified data rather than the objects' responsibilities. A reader had to know external implementation vocabulary to identify a line, a callback registration and controller operations.

**Cause**
The old names concealed ownership: Desc contained the actual line state, Action represented a caller-owned registration, and Chip was an operations table rather than a controller instance. Data carried one source's binding information. This obscured the distinction between controller state, line state and callback lifetime and made the later ownership refactor harder to review.

**Change**
Migrate the four object identities and their filenames atomically across declarations, definitions, callers and tests. For example:

```cpp
// Before:
irq::Action m_action;
// After:
irq::Registration m_action;
```

The implementation file is line.cpp, the callbacks are registrations, and the hardware hook parameter is ControllerSource. No aliases or forwarding headers preserve a second vocabulary. This increment changes names only; it is not completion of the subsystem redesign.

**Reference**
None, this project's own design: irq-redesign D1 (b), accepted on 2026-11-03. Names describe each object's role and ownership rather than inheriting another kernel's abbreviations.

**Verification**
Both dev images boot: dbg has 46 passed, 5 skipped and 0 failed; rel has 44 passed, 8 skipped and 0 failed, including 7/7 required markers. All 161 host tests pass. Nine kernel suites pass: irq.test (72 checks), irqcontroller.test (18), irqrevoke.test (37), irqregistry.test (66), irqdomain.test (87), irqrcu.test (34), interrupt.test (67), interruptline.test (78), and pinlines.test (28). All 22 touched C++ files ran through the repository formatter and clang-tidy against out/irq-design-review; no new diagnostics were found on changed lines. Existing naming diagnostics in untouched APIC headers remain. Performance was not measured.

**Deliberately not done**
Domain ownership, numeric identities, registration validation, guarded dispatch and the completion handshake remain separate increments. Their unresolved state is tracked in the task's STATUS.md.

**How it was found**
The design review traced facade-to-table-to-line operations and compared public names with the state and lifetime each type actually owns.

**The user's prediction**
None. D1 (b) was explicitly selected, and the user then suspended teaching exercises and asked for development to proceed.

**Three questions you should be able to answer**
None. Teaching exercises are suspended for this task at the user's request.

## Validate registration policy before either attachment path changes state (2026-11-03)

**Files**
- `kernel/include/ours/irq/registration.hpp` — keep callback configuration immutable and validate its policy in one constexpr predicate.
- `kernel/kernel/irq/line.cpp`, `kernel/include/ours/irq/line.hpp`, `kernel/include/ours/irq/controller.hpp` — share registration validation before controller checks or mutations.
- `kernel/commands/irq_test.cpp` — cover invalid policy on empty and occupied lines, both attach forms, unknown option bits, and unchanged controller state.

**Symptom**
Permanent | Shared was accepted on an empty line and could never be detached. The mode-setting attach could also program the controller before accepting this invalid registration. Occupied-line coverage previously hid the defect.

**Cause**
The old validator returned immediately for every non-Oneshot registration. Permanent and Shared are incompatible independently of deferred handling, so the registration's policy must be validated before occupancy and hardware decisions. Mutable callback configuration also represented an immutability promise only in comments.

**Change**
Registration now owns one private constexpr validity predicate, and its callback, context, policy, name and revocation hook are const after construction. Both attach forms enter through check_registration() before changing state:

```cpp
if (!action.is_valid()) {
    return Status::InvArgs;
}
```

Validation rejects unknown bits and every incompatible Permanent/Shared/Oneshot combination. Controller capability checks remain the line's responsibility. No public validation wrapper or duplicate policy word was introduced.

**Reference**
None, this project's own registration contract and ownership rule. The defect was recorded by this task in docs/repairs/261103/kernel-irq.md; that fixed item has been removed.

**Verification**
Both images boot: dbg 46 passed, 5 skipped, 0 failed; rel 44 passed, 8 skipped, 0 failed and 7/7 required markers. All 161 host tests pass. irq.test passes 76 checks, including four new policy checks; irqcontroller.test passes 18 and interruptline.test passes 78. All five touched C++ files pass changed-line formatting and run clang-tidy without new diagnostics on changed lines. Self-review confirmed errors are returned before mode writes, linking or publication. Performance was not measured.

**Deliberately not done**
The Permanent dispatch bypass and domain ownership are the next independent changes. This fix preserves callback lifetime and current masking behavior.

**How it was found**
The earlier review traced check_oneshot()'s early return for non-Oneshot policy and found the empty-line path into link_locked(). New tests check actual attachment and controller state, not just a returned error.

**The user's prediction**
None. The user suspended teaching exercises and requested focused development.

**Three questions you should be able to answer**
None. Teaching exercises are suspended for this task.

## Guard every line dispatch and remove the Permanent synchronization exception (2026-11-03)

**Files**
- `kernel/include/ours/irq/line.hpp` — guard the line flags and every flow helper with L1; delete the two permanent-dispatch analysis escapes.
- `kernel/kernel/irq/line.cpp` — take lockdep::Guard with interrupts and preemption disabled for every dispatch; query kernel ownership under L1.
- `kernel/kernel/irq/desc_table.cpp`, `kernel/include/ours/irq/controller.hpp`, `kernel/include/ours/irq/mod.hpp` — follow the unified guard contract.
- `kernel/commands/irq_test.cpp` — observe a held spin lock and disabled preemption inside the Permanent callback.
- `docs/repairs/261004/kernel-irq.md` — remove the fixed thread-context dispatch/preemption repair.

**Symptom**
Permanent lines bypassed the required Guard and reached the protected flow and callback list through TA_NO_THREAD_SAFETY_ANALYSIS helpers. Direct thread-context dispatch also depended on an external preemption scope if its handler woke a thread.

**Cause**
A lifetime policy had acquired a second synchronization protocol. Keeping the list immutable could justify a lockless read in isolation, but required atomic line flags, an extra publication argument and suppressed capability checks. This contradicted the user's explicit requirement that the subsystem use its normal safety guards. Callback lifetime, line state and thread-context rescheduling are simpler to verify under one guarded dispatch protocol.

**Change**
All line dispatch now begins with the same checked scope:

```cpp
Guard<sync::SpinLock, IrqSaveAndPreemptDisable> const guard{m_lock};
auto const result = run_flow(m_flow, m_registrations, eoi);
```

The line flags are plain LineFlags annotated TA_GUARDED(m_lock); flow helpers require m_lock. permanent_registrations(), permanent_flow() and their analysis suppressions are deleted. Permanent still means exclusive ownership for the kernel's lifetime; it no longer changes synchronization. Guard releases L1 before restoring preemption, so a callback's wake cannot switch away holding the line.

**Reference**
None, this project's own invariant and the user's explicit guard requirement. This intentionally supersedes the older Permanent bypass recorded by irq-genirq. No alternative locking implementation or Guard wrapper was introduced.

**Verification**
All six changed C++ files ran the repository formatter and clang-tidy without new diagnostics on changed lines. The compiler's thread-safety checks cover every flow helper and line flag access. Both images boot and all 161 host tests pass. Nine kernel suites pass: irq.test 77 checks, irqcontroller.test 18, irqrevoke.test 37, irqregistry.test 66, irqdomain.test 87, irqrcu.test 34, interrupt.test 67, interruptline.test 78, and pinlines.test 28. The new Permanent callback check observes a held spin lock and preemption disabled. IRQ sources contain no TA_NO_THREAD_SAFETY_ANALYSIS. Existing tests also exercise detach, revocation, mask/unmask and concurrent domain readers. Performance was not measured; this is guard compliance, not a performance optimization.

**Deliberately not done**
The shared lockdep library's coarse fallback lock class remains tracked as kernel-irq repair item 2; this change uses the existing Guard and does not claim runtime lock-class validation is complete. Domain ownership and completion revalidation remain separate work.

**How it was found**
The guard acceptance audit identified the Permanent branch and two suppressed readers in Line. Reviewing the thread-context dispatch path showed that the line itself should own its callback preemption guarantee.

**The user's prediction**
None. The user suspended teaching exercises and requested development.

**Three questions you should be able to answer**
None. Teaching exercises are suspended for this task.

## Make Domain the single owner of source mappings and line storage (2026-11-03)

**Files**
- `kernel/include/ours/irq/domain.hpp`, `kernel/kernel/irq/domain.cpp` — implement a noncopyable, address-stable Domain with one private Storage aggregate.
- `kernel/include/ours/irq/desc_table.hpp`, `kernel/kernel/irq/desc_table.cpp` — removed.
- Registry, facade, kernel tests and GN sources — use the owning Domain directly.

**Symptom**
Domain owned allocation-backed Span fields while DescTable owned another allocation of Line pointers. The map was implicitly copied into the table, and failure or removal depended on manual discard, reset and destroy calls. The public names split one source set's lifetime across unrelated representations.

**Cause**
A Span describes a borrowed range and does not express allocation ownership. Copying it preserved pointers rather than transferring ownership; an ordinary C++ copy could silently create two apparent owners. Separate map and table objects also repeated lifetime operations and left test scopes responsible for manual cleanup.

**Change**
Domain now owns a private Storage aggregate:

```cpp
struct Storage {
    ktl::Vec<VIrqNum> virqs;
    ktl::Vec<HIrqNum> hwirqs;
    ktl::Vec<ustl::Box<Line>> lines;
};
```

Initialization constructs a local Storage, validates source identities, builds the bijection and binds every Line, then moves the complete aggregate into Domain. Refusal destroys the local aggregate automatically. Domain cannot be copied or moved; its destructor checks that no registrations remain, then its existing owning containers reclaim the lines and arrays. Registry removal still waits for the RCU grace period before deleting Domain. No destructor performs an implicit grace period or silently detaches a live registration.

The separate DescTable type, manual discard lambda, map reset and table destroy interface are removed. The old post-destructor byte-poison loop is also removed: scoped ownership and the existing destruction sentinel replace manual destruction and deallocation. This does not replace the reader grace-period guarantee.

**Reference**
None, this project's own D1 (b) ownership model, implemented with existing ktl::Vec and ustl::Box rather than a new ownership utility.

**Verification**
All 14 touched C++ files ran formatting and clang-tidy with no new changed-line diagnostics. Both dev images boot: dbg 46 passed, 5 skipped, 0 failed; rel 44 passed, 8 skipped, 0 failed, 7/7 required. All 161 host tests pass. All nine IRQ suites pass, including irqdomain.test's 87 layout, refusal and repeated-initialization checks through the actual owning Domain; compile-time assertions reject Domain copy and move. IRQ removal and reader suites also pass: irqrevoke.test 37, irqregistry.test 66 and irqrcu.test 34. The remaining suites pass: irq.test 77, irqcontroller.test 18, interrupt.test 67, interruptline.test 78, pinlines.test 28. Performance was not measured.

**Deliberately not done**
Raw domain pointer exposure, registry entry ownership and repeated facade controls remain for the next interface increment. Numeric identity separation and completion revalidation remain in the acceptance ledger.

**How it was found**
Review M3 identified ownership represented by copyable views; following the construction and retirement paths showed that mapping and line storage have exactly the same lifetime.

**The user's prediction**
None. The user chose D1 (b) and suspended teaching exercises.

**Three questions you should be able to answer**
None. Teaching exercises are suspended for this task.

## Revalidate deferred completion under the line-to-owner lock order (2026-11-03)

**Files**
- `kernel/include/ours/irq/registration.hpp`, `types.hpp` — group handle, revoke and readiness callbacks into one immutable protocol; deferred registrations require readiness.
- `kernel/kernel/irq/line.cpp` — revalidate the owner's current work before clearing Held or unmasking.
- `kernel/object/interrupt_dispatcher.cpp` and interrupt-object headers/implementation — query Idle/Waiting under L2 and provide one test hook with completion and park phases.
- Kernel registration callers and tests — use named callback records; inject a new interrupt before both ack and wait-as-ack complete.

**Symptom**
An old ack could clear NeedAck, drop L2, admit a new dispatch that set NeedAck and Held, then clear the new Held while finalizing under L1. The existing after-finalize/before-park hook did not exercise this window.

**Cause**
L1 serialized individual line mutations but did not establish whether the caller's earlier observation still described the owner's work. Held belongs to the line and NeedAck belongs to the delivery object; a stale observation of NeedAck cannot authorize a later unmask. Holding L2 while acquiring L1 would reverse the dispatch order and deadlock.

**Change**
Every deferred registration provides a bounded readiness callback that reads its actual owner state under the same lock the handler uses. Completion holds L1, validates attachment, then invokes readiness:

```cpp
if (!action.ready_to_complete()) {
    return;
}
set_held_locked(false);
m_source.chip->unmask(m_source);
```

The physical object's callback takes L2 and permits completion only in Idle or Waiting. Triggered, NeedAck and Destroyed do not reopen the line. Dispatch cannot establish new work between this recheck and unmask because L1 remains held. Work admitted before the recheck is seen under L2; work admitted after completion masks and holds the line afterward. No path takes L2 then L1.

Registration::Callbacks groups the three operations over one immutable context and replaces positional handler/cancel arguments. No alternate legacy constructor remains. The extra readiness pointer is per registration; no event counter, acknowledgement token or second pending-work word is introduced. Detach still drains IRQ callbacks; owners must quiesce their own completion calls before reusing a registration or freeing its context.

**Reference**
None, this project's own state and lock-order argument. Compared alternatives: generation-bearing completion needs token propagation through coalesced wait/port delivery and creates another work-identity protocol; controller-specific exclusion cannot cover synthetic dispatch or another controller. Revalidation uses the owner state already serialized by L2 and the existing L1-held dispatch.

**Verification**
All 14 changed C++ files ran formatting and clang-tidy without new changed-line diagnostics. Both images boot and all 161 host tests pass. All nine suites pass: irq.test 78, irqcontroller.test 18, irqrevoke.test 37, irqregistry.test 66, irqdomain.test 87, irqrcu.test 34, interrupt.test 67, interruptline.test 83, pinlines.test 28. The policy suite rejects deferred registrations lacking readiness.

Counterfactual verification temporarily removed only the readiness gate, built it, and ran interruptline.test. Both new assertions failed: "an old ack cannot unmask newer work admitted before completion" and "wait-as-ack cannot unmask an interrupt injected before completion". The exact fixed source was restored in a finally scope, rebuilt, and passed the final positive checks. Negative output is in /tmp/irq-completion-negative.log; positive suite output is in /tmp/irq-completion-tests.log. No failing variant was committed. Performance was not measured.

**Deliberately not done**
No claim is made that a real IO-APIC delivery reproduces the synthetic ordering. The invariant now holds for every admitted dispatch regardless. Controller capabilities, typed numeric identities and the facade/endpoint cleanup remain in STATUS.md.

**How it was found**
The review's open completion question identified the L2-drop/L1-acquire window. A phase-specific hook makes that ordering deterministic in the real interrupt object; the old-behavior run demonstrates that the test catches the missing synchronization property.

**The user's prediction**
None. The user suspended teaching-oriented arguments and requested focused development.

**Three questions you should be able to answer**
None. Teaching exercises are suspended for this task.

## Hide domain storage behind an owning registry and controller endpoint (2026-11-03)

**Files**
- `kernel/include/ours/irq/domain.hpp` — the public DomainEndpoint and registration API.
- `kernel/kernel/irq/domain.hpp`, `domain_registry.hpp`, `domain_registry.cpp` — private owning domain and registry with Box ownership.
- `kernel/platform/pc/interrupt.cpp`, IRQ facade and kernel tests — dispatch through endpoints and pass per-controller context.

**Symptom**
Latent: an adapter could obtain a raw domain pointer without keeping its reader scope alive. The registry manually paired allocation and deletion, copied mapping bounds into entries, and exposed its implementation as public API. Controller instances sharing operations could not supply instance context at registration.

**Cause**
The storage interface made the controller responsible for both interrupt identification and another module's reclamation rules. A separate slot getter and generic dispatch call left the lifetime requirement between calls. Manual delete paths duplicated ownership instead of expressing who retained a retiring domain.

**Change**
Before, the root performed `handle_irq_generic(slot.table_with_irqs_off(), source)`. Now it calls `endpoint.dispatch(source)`. The endpoint opens the IRQ RCU scope and closes it only after lookup and flow complete. Some means the source's receiving-CPU interrupt was ended; None leaves fallback completion to the root. Domain and DomainRegistry headers moved out of the public include tree. No old-name forwarding API remains.

Each registry slot owns Box<Entry>, and each immutable published entry owns Box<Domain>. Mapping bounds are queried from Domain instead of copied. Removal marks the slot Removing, revokes registrations, retracts both publications, waits for readers, then moves ownership out while freeing the slot under Guard. Destruction occurs after dropping the writer lock. The id is read only while an entry is still owned; no separate id array or post-free inspection is necessary. Controller context is bound before publication and reaches every operation through ControllerSource.

Keeping a public borrowed-pointer getter was rejected because it preserves the caller's lifetime burden. Reference-counted domain handles were rejected because dispatch already has a reader protocol and ownership is unique. A general dynamic snapshot directory was not introduced: the eight-domain bound remains a private implementation limit, replaceable without changing controller callers.

**Reference**
None, this project's own design. The boundary follows its actual IRQ RCU and unique storage ownership rather than importing a reference kernel's domain hierarchy.

**Verification**
All 19 changed C++ files ran LLVM 23 formatting and clang-tidy with no new changed-line diagnostics. Both images pass: dbg 46 passed, 5 skipped, 0 failed; rel 44 passed, 8 skipped, 0 failed and 7/7 required. All 161 host tests pass. Nine kernel suites pass: irq.test 78, irqcontroller.test 18, irqrevoke.test 37, irqregistry.test 85, irqdomain.test 87, irqrcu.test 34, interrupt.test 67, interruptline.test 83 and pinlines.test 28. The 19 new registry checks use the public global API with two controllers sharing source zero and an ops table, but different contexts; they exercise dispatch, EOI, mask, unmask and independent withdrawal. Existing multi-CPU reader/removal and two-writer tests pass. Performance was not measured.

**Deliberately not done**
Typed numeric identities, common control resolution and explicit controller dispatch protocols are subsequent increments. Local test registries retain a private raw-pointer test seam under an explicit IRQ RCU guard. Their destruction requires users to have quiesced; no destructor blocks on a grace period.

**How it was found**
The API audit traced the root handler's raw-table acquisition and every registry new/delete path. Existing removal tests cover a retiring entry held in a grace period while another writer inspects ids; those tests remain green after ownership transfer replaces duplicate writer arrays.

**The user's prediction**
None. Teaching exercises are suspended for this task.

**Three questions you should be able to answer**
None. Teaching exercises are suspended for this task.
