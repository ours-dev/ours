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
