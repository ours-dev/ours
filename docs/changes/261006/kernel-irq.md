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

## Title: a line's held state is a bit of its flag word, beside a line-private LineFlags, and the line's flow moves into the bytes after that word, taking Desc from 104 bytes to 88 (2026-10-06)

**Files**
- `kernel/include/ours/irq/desc.hpp` — new private `Desc::LineFlags` (Shared, Permanent, Oneshot, Held); `m_flags` is `std::atomic<LineFlags>`; `bool m_held` is gone; new `is_held_locked()` and `set_held_locked()`, both `TA_REQUIRES(m_lock)`; `m_flow` declared right after `m_flags`
- `kernel/kernel/irq/desc.cpp` — new `Desc::line_flags()` translating an `IrqFlags` into the line's word; every read and write of the line's facts uses `LineFlags`; held is read and written through the two accessors; detach() clears Held in the same store that clears Shared and Oneshot

**Symptom**
`Desc` was 104 bytes, 22 of them padding. `bool m_held` took one byte at offset 80 and left seven bytes of padding before the 8-byte `m_spurious`. `Flow m_flow` (one byte) did the same at offset 48 before the 8-byte list header. Measured with clang's `-fdump-record-layouts` on `desc.cpp` as the build compiles it.

**Cause**
Step 6 added held as a separate `bool`. That breaks the codebase's rule for yes/no state (modern-cpp-development, the user's rule of 2026-10-03): the facts about one object go in one Flags word, not in separate `bool` members. Two `bool`s are two memory locations, each padded to the alignment of whatever follows it; one word holds 32 facts in four bytes.

The rule's condition is that every bit of a word is written under the same protection, because a store of the word rewrites all of its bits. Held qualifies: it was `TA_GUARDED(m_lock)`, and every bit of `m_flags` is already written under `m_lock` (L1). Putting it in the word therefore loses no write. Neither the step-6 author nor its review checked the rule.

Two more things had to be decided.

- **Which type the word holds.** `m_flags` was an `IrqFlags`, which is also what an action asks for when it is built. Adding Held to `IrqFlags` would let a driver write `IrqFlags::Held` into an action, a bit that means nothing there and that `link_locked()` would silently drop. The word gets its own type, private to `Desc`, and the constructor translates `IrqFlags` bit by bit rather than by reinterpreting the integer.
- **What the thread-safety analysis still checks.** As a `bool`, held was `TA_GUARDED(m_lock)`, so the compiler rejected any access without L1. A bit of an atomic word that is also read without the lock (Permanent, by `handle_irq()`) cannot carry that annotation. So held is reached only through `is_held_locked()` and `set_held_locked()`, and both are `TA_REQUIRES(m_lock)`: the check moves from the member to its only two doors.

Memory order: a store of held is a release, like every other store to the word. `is_permanent()` reads the word with an acquire load, without L1. An acquire load that reads a value synchronizes only with the store that wrote it, and in C++20 a later plain store does not continue the release sequence of an earlier one. Were held stored relaxed, an acquire that read it would order nothing. On x86 a release store is a plain `mov`, so this costs nothing. On a Permanent line held is never written anyway, since Oneshot is refused there.

The member order: `m_flags` is 4 bytes at offset 16 and the next member, `Data`, needs 8-byte alignment, so bytes 20-23 were padding. `m_flow`, one byte, now sits there instead of padding its own 8 bytes at offset 48.

**Change**
Before:

```cpp
    std::atomic<IrqFlags> m_flags;
    Data m_data;
    Flow m_flow TA_GUARDED(m_lock) = Flow::Edge;
    Action::List m_actions TA_GUARDED(m_lock);
    bool m_held TA_GUARDED(m_lock) = false;
    std::atomic<u64> m_spurious{0};
```

After:

```cpp
    enum class LineFlags : u32 { None, Shared = 0_bit, Permanent = 1_bit, Oneshot = 2_bit, Held = 3_bit };
    USTL_ENABLE_INNER_ENUM_BITMASK(LineFlags);
    ...
    std::atomic<LineFlags> m_flags;
    Flow m_flow TA_GUARDED(m_lock) = Flow::Edge;
    Data m_data;
    Action::List m_actions TA_GUARDED(m_lock);
    std::atomic<u64> m_spurious{0};
```

Layout, measured:

| | sizeof(Desc) | padding |
|---|---|---|
| before | 104 | 22 bytes: 4 after the canary, 4 after `m_flags`, 7 after `m_flow`, 7 after `m_held` |
| after | 88 | 7 bytes: 4 after the canary, 3 after `m_flow` |

88 is the floor for these members: their sizes sum to 81, and the struct's 8-byte alignment rounds that up to 88.

Rejected options:

- **Held in `IrqFlags`.** Smaller change, but a line's state would become an option an action could ask for.
- **Held as a bit of a new word beside `m_flow`.** Still one word per protection, but with a single bit it is a `bool` by another name, and it would still need its own padding.
- **`fetch_or` and `fetch_and` for held.** They would also keep the release sequence going, but every writer holds L1 already, so a locked read-modify-write buys nothing over load-then-store-release. It would add a `lock`-prefixed instruction to every Oneshot interrupt.

**Reference**
None, this project's own rule. Linux keeps the same kind of line state in `irq_desc->istate` and `irq_data->state_use_accessors`, words of bits written under `desc->lock` and read through accessors (`irqd_irq_masked()` and the like), and its oneshot debt is `desc->threads_oneshot`, a bitmap word, one bit per action.

**Verification**
- Build: ok, `-Werror` with the thread-safety analysis on.
- `scripts/format_cpp.py --check`: clean on both files.
- `clang-tidy -p out/bringup.x86 kernel/kernel/irq/desc.cpp`: nothing in an irq file.
- Layout: `-fdump-record-layouts` on `desc.cpp`, 104 bytes before and 88 after, as in the table above.
- `boot_check.py` and `kshell_run.py`: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required), rel.log has `pc-ps2: key a`, `key left-shift` and `key A`; irq.test ok (69 checks), interruptline.test ok (58), interrupt.test ok (32), the same counts as before. irq.test's held checks (WakeThread leaves the line masked, finalize unmasks, detach clears held) now run through the bit.

**Deliberately not done**
- The 4 bytes after the canary stay. Moving `m_flags` there would not shrink `Desc` below 88, and it would separate the canary from the lock, which by convention come first.
- No other `bool` member exists in kernel/irq or in the interrupt objects. Searched with `grep 'bool m_'` over `kernel/include/ours/irq`, `kernel/kernel/irq` and `kernel/object/interrupt*`.

**How it was found**
The user read the `m_held` declaration and pointed at the rule.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What condition must every bit of a flags word meet, and why does held meet it while Permanent is read without the lock?
- Held lost its `TA_GUARDED`. What still makes the compiler reject a read of held without L1?
- Why is a store of held a release, when no lockless reader ever looks at the Held bit?
