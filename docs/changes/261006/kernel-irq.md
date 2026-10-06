# kernel-irq changes

## Title: a line's flow ends its interrupt through an EoiGuard held across the flow, in place of an end_irq() call on each return path (2026-10-06)

**Files**
- `kernel/kernel/irq/desc.cpp` — new `EoiGuard` in the file's unnamed namespace; `Desc::run_flow()` holds one across the flow; `handle_edge()`, `handle_fasteoi()` and `handle_oneshot_locked()` lose their `eoi` parameter and their `end_irq()` calls; `Desc::end_irq()` is gone
- `kernel/include/ours/irq/desc.hpp` — the flows' declarations without `eoi`, `end_irq()` removed, comments say who ends the interrupt

**Symptom**
Latent, cannot be triggered today. Every return path of every flow called `end_irq(eoi)` by hand, five calls in all. A new early return in a flow that forgot the call would leave the interrupt in service at the local APIC, which then holds back every interrupt of that priority and below on that CPU: the timer and every device would go quiet on it, with nothing logged.

**Cause**
Ending an interrupt is a paired state change: the controller puts the interrupt in service when it delivers it, and only the end of interrupt takes it out. A pair that has to be closed on every path is what a scope is for, and the codebase already says so for the other pairs of its kind: a lock is a `Guard`, interrupts off is an `AutoInterruptDisabler`, preemption off is an `AutoPreemptDisabler` (the modern-cpp-development rule of 2026-10-04: a paired state change is a scope with an RAII object, never a call and its matching undo).

Before this change the pairing lived in each return path:

- `handle_edge()` had two exits, each with its own `end_irq(eoi)`;
- `handle_fasteoi()` had two of its own, and handed its third path to `handle_oneshot_locked()`, which had a fifth;
- each flow also took `eoi` only to pass it on.

Both failure modes are silent. A forgotten end leaves the CPU deaf to its interrupts at that priority and below. A doubled end is worse in a quieter way: the local APIC's EOI ends the highest-priority interrupt in service, so a second one ends an interrupt nested beneath this one, and its line is treated as serviced while its handler is still running.

The order matters as much as the count. The end must come after everything the flow does to the line: after the mask of a line nobody is on, so a level pin still asserted cannot deliver again in between, and after the unmask of a Oneshot line that is not held. It must also stay inside the dispatch's hold of L1, as the design states. The guard gets both from where it is constructed: first thing in `run_flow()`, which the caller enters already holding L1. Locals are destroyed after the return value is computed, so the end goes out after the flow has returned and before `handle_irq()` drops L1.

**Change**
Before, in each flow:

```cpp
auto Desc::handle_edge(Action::List &actions, Eoi const eoi) -> IrqReturn {
    if (actions.empty()) {
        mask_unclaimed();
        end_irq(eoi);
        return IrqReturn::None;
    }

    auto const result = notify_all(actions);
    end_irq(eoi);
    return result;
}
```

After, once for both flows:

```cpp
auto Desc::run_flow(Flow const flow, Action::List &actions, Eoi const eoi) -> IrqReturn {
    EoiGuard const end_of_interrupt{m_data, eoi};
    switch (flow) {
        case Flow::Edge:
            return handle_edge(actions);
        case Flow::FastEoi:
            return handle_fasteoi(actions);
    }
    unreachable();
}

auto Desc::handle_edge(Action::List &actions) -> IrqReturn {
    if (actions.empty()) {
        mask_unclaimed();
        return IrqReturn::None;
    }
    return notify_all(actions);
}
```

`EoiGuard` holds the line's `Data` and the `Eoi`. Its destructor is the old `end_irq()` body: nothing for `Skip`, otherwise the chip's `issue_eoi`. It is neither copyable nor movable, so the one end cannot be handed to a second owner. Its constructor is `[[nodiscard]]`, so `EoiGuard{m_data, eoi};` written as an unnamed temporary, which would end the interrupt at once, before any action ran, is a compile warning, and so an error under `-Werror`.

Rejected options:

- **The guard in `handle_irq()`, before the lock `Guard`.** It would be destroyed after the lock guard, so the end would go out after L1 is dropped. Nothing breaks today, since the local APIC still holds this priority back and the IO-APIC's Remote IRR still holds a level pin until the end. But the design says the end is inside the hold of L1, and moving it out is a change of protocol, not a refactor.
- **A guard per flow.** That gives three guards, one each in `handle_edge()`, `handle_fasteoi()` and `handle_oneshot_locked()`. `handle_fasteoi()` calls `handle_oneshot_locked()`, so one of them would have to know not to make its own; the duplication the guard removes comes back as a rule.
- **A public class in `types.hpp` or `chip.hpp`.** Only `run_flow()` ends interrupts, and arch's EOI for a vector with no line is a single call with no early return. A type nobody else can use stays in the unnamed namespace.

**Reference**
Linux `handle_fasteoi_irq()` (`kernel/irq/chip.c`) has the same shape written in C: its early exits jump to an `out:` label that calls `chip->irq_eoi()` before `raw_spin_unlock()`, and its main path ends through `cond_unmask_eoi_irq()`, so the end is still written twice, once per kind of exit. A single exit point, reached by `goto`, is C's substitute for a destructor. In this codebase the pattern is `lockdep::Guard` and `task::AutoPreemptDisabler`.

**Verification**
- Build of `out/bringup.x86`: ok, `-Werror` with the thread-safety analysis on.
- `scripts/format_cpp.py --check`: clean on both files.
- `clang-tidy -p out/bringup.x86 kernel/kernel/irq/desc.cpp`: nothing in an irq file.
- `boot_check.py`: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required); rel.log has `pc-ps2: key a`, `pc-ps2: key left-shift` and `pc-ps2: key A`.
- `kshell_run.py --out out/bringup.x86`: irq.test ok (69 checks), interruptline.test ok (58), interrupt.test ok (32), the same counts as before the change.
- The tests already pin the order the guard must keep: irq.test's count of ends sent while the pin was unmasked, for a line nobody is on, is zero only if the mask comes before the end. It still passes.

**Deliberately not done**
- arch's EOI for a vector with no line (`faults.cpp`) stays a plain call: it has one path and no early return, so a guard there would add a type and remove nothing.
- No change of behaviour: same ends, same order, same lock. This is a refactor; the protocol is left to the genirq steps still open.

**How it was found**
None. The user asked for the end of interrupt to become an RAII object.

**The user's prediction**
None.

**Three questions you should be able to answer**
- `return handle_edge(actions);` sits inside the guard's scope. Why does the end of interrupt still go out after `handle_edge()` has run, and not before it?
- What would change if the guard were declared in `handle_irq()` above the lock `Guard`, and which of the two would be destroyed first?
- Why does the constructor carry `[[nodiscard]]`, and what would `EoiGuard{m_data, eoi};` on a line of its own do to the interrupt?
