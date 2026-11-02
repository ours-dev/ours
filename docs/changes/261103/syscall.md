# syscall changes

## interrupt_map resolves (domain, source), and the route answer names the domain (2026-11-03)

**Files**
- `system/public/ours/syscalls.gni`, `system/public/ours/interrupt.hpp` — `interrupt_map` takes `resource`, `domain`, `source`, `out`; `ROOT_INTERRUPT_DOMAIN` is the root's name in user space, and `ours/interrupt.hpp` is where a second domain's rule will be written (D12).
- `kernel/lib/syscall/src/interrupt.cpp` — `interrupt_map` resolves through the new `irq::virq_of_domain()`; the resource comment is the policy (D17), no longer the technical reason; the invalid domain answers InvArgs, before the resource.
- `kernel/include/ours/irq/types.hpp` — `DomainId` moves here from the registry header: it is a numbering type, beside the numbers it scopes, and the facade's new signature needs it.
- `kernel/include/ours/irq/mod.hpp`, `kernel/kernel/irq/mod.cpp` — `irq::virq_of_domain(id, hirqnum)`, the resolver, one read guard over the directory; `virq_of_hwirq()` stays as the root-domain form the kernel's own code names lines by, now written as `virq_of_domain(DomainId::Root, ...)`.
- `system/lib/ours.acpi/include/ours_acpi/ours_acpi.hpp`, `system/lib/ours.acpi/src/ours_acpi.cpp` — `IsaIrqReply` carries `domain` first in its body (`u32 domain@20`, then gsi, trigger, polarity; 36 bytes), encoded and decoded.
- `system/service/acpi-route/src/acpi_route.cpp`, `system/service/acpi-route/include/acpi_route/routes.hpp` — the service answers `ROOT_INTERRUPT_DOMAIN`, ACPI's numbering being the root controller's (D12); the route comment names what interrupt_map resolves.
- `system/driver/pc-ps2/pc_ps2.cpp` — the keyboard's line is resolved from `route->domain` and `route->gsi`, the pair the service answered; the driver names no number itself.
- `system/service/init/test/routing_checks/routing_checks.cpp`, `system/app/shell/shell_tests.cpp` — every call site names the root domain, and the new refusals are pinned: a domain no controller registers (7) is NoResource, 0xFFFFFFFF as a domain is InvArgs.
- `system/lib/ours.acpi/test/ours_acpi_test.cpp` — the reply's layout, written out on the test's own side, moves with the field.

**Symptom**
Latent, cannot be triggered today: one controller exists on x86. It would be triggered by the first second domain (D13 keeps x86 to one; a PLIC or a GIC is not): with `interrupt_map(resource, source)` a source number alone cannot say which controller's numbering it belongs to, and two controllers can hold the same number — the ABI would have to grow or misresolve. This is step 12 of `.agents/tasks/irq-multi-arch/PLAN.md`, the user's choice of D4a (2026-11-01): the specifier form now, `interrupt_map(resource, domain, hwirq, out virq)`.

**Cause**
The one-number ABI of interrupt-system D11 (a) named a line by the root controller's number alone. That is a resolution with the namespace left out: a virq is the line's canonical, never-reused number, but the pair that names it is (domain, hardware number), and one half of the pair is not the name. D4a replaced it before a second controller could force the change; this step lands it, and every old caller goes in the same commit, no old call kept.

**Change**
The call is now the resolver shape — a scoped name in, the canonical number out:

```cpp
// before
auto interrupt_map(ours::Uandle const resource, u32 const source,
                   user_copy::UserOutPtr<u32> const out) -> Status;
    auto const virq = irq::virq_of_hwirq(source);

// after
auto interrupt_map(ours::Uandle const resource, u32 const domain, u32 const source,
                   user_copy::UserOutPtr<u32> const out) -> Status;
    if (domain == std::to_underlying(irq::DomainId::Invalid)) {
        return Status::InvArgs;
    }
    if (source == irq::INVALID_H_IRQ_NUM) {
        return Status::InvArgs;
    }
    ...
    auto const virq = irq::virq_of_domain(static_cast<irq::DomainId>(domain), source);
```

and the driver resolves from the route service's answer, the party that reads the tables, not from a number it holds:

```cpp
// before, in pc-ps2
auto const status = ours::interrupt_map(irq, gsi, &number);

// after
auto const status = ours::interrupt_map(irq, domain, gsi, &number);
    // with {domain, gsi} = the service's IsaIrq reply, its first field the
    // domain the GSI is a number of: the root controller's, 0, on ACPI.
```

The reply gained the field because D12 records the answer as "the constant 0 plus the GSI" and the design's route answer is to be tagged from the start — which controller a number belongs to is part of the answer, not knowledge the driver keeps. The kernel-side `virq_of_hwirq()` stays for the kernel's own code (the ISA lines, the HPET): the platform's single numbering is the root's, and a kernel driver that names a root line by number asks the root domain, spelled `virq_of_domain(DomainId::Root, ...)`.

The answers keep their pinned order: invalid domain, then invalid source, each InvArgs and both before the resource; then BadHandle/BadType/AccessDenied for the resource; then NoResource for a domain that names none or a source with no line in it; then AccessDenied for a line the kernel holds (D4b). The resource comment is the policy (D17): naming a line from the platform's numbers is the root's power, and it is not cut into ranges.

**Reference**
Linux's `irq_create_mapping(struct irq_domain *domain, irq_hw_number_t hwirq)` — the same resolution, (domain, hwirq) to a global virq, with the domain a parameter and not a guess. Ours is its syscall form restricted to the root's holder (D17), and the kernel has no tree of domains to walk: one flat directory, resolved under one read guard. The conceptual frame is a scoped name resolved to a canonical id, as a PID namespace's (pidns, pid) resolves to a task and DNS's (zone, name) to a record: the pair is reusable and scoped, the number it resolves to is global and, unlike both of those, never reused. Zircon has no counterpart — its `zx_interrupt_create` takes the resource and the vector, with the root resource granting ranges and no second numbering to name.

**Verification**
`boot_check.py`: dbg 45 passed and 4 skipped, rel 42 passed and 8 skipped with 6 of 6 required lines — `pc-ps2: key a` and `pc-ps2: key A` among them, the keyboard's line resolved through the new pair end to end, and `routing_checks: passed`, its interrupt_map checks now naming the root domain. 154 host tests (the codec test's layout moved with the field, written out on its own side as it was). The kernel-shell battery on four CPUs: `irqchip.test` 18, `irq.test` 72, `interruptline.test` 78, `pinlines.test` 28, `irqdomain.test` 87, `irqrcu.test` 34, `irqrevoke.test` 37, `irqregistry.test` 66, `interrupt.test` 67 — unchanged, the kernel side untouched by the ABI. Mutation: dropping the invalid-domain check is killed by the dbg shell's self-check (`interrupt_map(0xFFFFFFFF, GSI 3)` then answers NoResource where InvArgs is pinned). Not verifiable here: a second domain to resolve into, so cross-domain resolution stays tested on synthetic registries (`irqregistry.test`), as D13 says.

**Deliberately not done**
- The user-facing argument stays `source`, not the option card's `hwirq`: the card decided the shape (a domain and a number), and `source` is the word the user documentation, the route service and ACPI itself already speak. The kernel-internal name is `hirqnum` either way.
- No new status for "no such domain": it answers NoResource, the layer's answer for "no such line", and D4b already rejected a new code for the same reason.
- No domain argument on `interrupt_create`: it takes the virq interrupt_map resolved, the canonical number, and a domain there would let a program re-scope a number it was given.
- MSI's tagged answer: a device-tree route service's `Leaf{controller, cells}` form is its own later task, the design's sentence stands for it.

**How it was found**
None.

**The user's prediction**
None — the shape was decided as D4a on 2026-11-01; the step carried it out.

**Three questions you should be able to answer**
- Why must the route service's reply carry the domain instead of the driver hard-coding 0?
- What can go wrong if a program caches `(domain, source)` across a removal, and why is caching the virq safe?
- Why does an invalid domain answer InvArgs while a domain that names no controller answers NoResource?
