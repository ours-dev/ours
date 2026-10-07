# kernel-irq changes

## Title: interrupt objects attach Oneshot and leave a level line's mask to its flow: the handler always answers WakeThread, mask_interrupt() is empty, and unmask_interrupt() is the finalize, called outside L2 only once the debt is cleared (2026-10-07)

**Files**
- `kernel/object/interrupt_event_dispatcher.cpp` — the action is built with `IrqFlags::Oneshot`; `on_interrupt()` always answers `WakeThread`; `create()` no longer reads the trigger mode attach answers; `unmask_interrupt()` calls `irq::finalize_oneshot(m_line, m_action)`; the out-of-line `mask_interrupt()` is gone
- `kernel/object/include/ours/object/interrupt_event_dispatcher.hpp` — `mask_interrupt()` is an empty override; the class and `create()` comments say the line's flow masks a level line, not the object
- `kernel/object/interrupt_dispatcher.cpp` — `set_level_triggered()` is gone; `deliver_locked()` and `send_packet_locked()` no longer mask; `begin_wait()` returns a `WaitStart` that says whether the wait cleared the debt, and `wait()` then calls `unmask_interrupt()` after the lock is dropped, before the test hook and the park; `ack()` calls `unmask_interrupt()` after the lock is dropped, only on the path that moves to Idle; `trigger()` reads the flags before taking the lock
- `kernel/object/include/ours/object/interrupt_dispatcher.hpp` — `Flags` loses `UnmaskPrewait` and `MaskPostwait`; `m_flags` is `Flags const`, no longer `TA_GUARDED`; new private `Debt` and `WaitStart`; the class comment says what the object owes its line and that L2 is never held across `unmask_interrupt()`; the four virtuals' contract is rewritten, and the review's second fixup marks them `TA_EXCLUDES(m_spinlock)`
- `kernel/include/ours/irq/desc.hpp`, `kernel/kernel/irq/desc.cpp` — two comments that gave an interrupt object as the example of a handler masking its own line now say no handler does
- `kernel/commands/interrupt_line_test.cpp` — the first dispatch on an object's line is expected to answer `WakeThread`, not `Handled`
- `.agents/skills/ours-kernel-design/references/task.md` — the genirq rule says the objects write no mask since step 7, and that the handler answers WakeThread even once Destroyed
- `.agents/tasks/irq-genirq/PLAN.md` — step 7
- `docs/repairs/261007/kernel-irq.md` — new file: item 1, the finalize an edge object's every ack now costs

**Symptom**
None visible, by design: step 7 of the irq-genirq DESIGN's Staging changes who masks a level line, not what user space sees. A level object's line is still masked from each interrupt until the ack that leaves nothing more to deliver, and an edge object's line is still never masked. Every `interruptline.test` check that step 5 wrote against the old objects passes unchanged against the new ones.

What was wrong before: a level line's mask had two writers under no common lock. kernel/irq wrote it under the line's lock L1 (the flow, for a line nobody is on; detach, for the line it empties; and since step 6, the Oneshot flow). The interrupt object wrote it too, through `irq::mask()` and `irq::unmask()`, which take only the IO-APIC's register lock LI: from its handler, and from `ack()` and `begin_wait()` under its own lock L2. With two writers, kernel/irq could keep no state about the line that depends on the mask, such as step 6's held bit: an unmask from the object could land at any moment relative to the flow's decision "leave it masked, a thread still owes the device".

**Cause**
The objects:
- The interrupt object (`InterruptEventDispatcher`): a state machine under L2 (Idle, Waiting, Triggered, NeedAck, Destroyed). NeedAck means "an interrupt was handed to the driver and the object waits for its ack". The ack is `interrupt_ack` when bound to a port, or the next `interrupt_wait` when not.
- The object's action on the line: its handler is `on_interrupt()`, called by the line's flow in interrupt context under L1. Its flags now say Oneshot.
- The line's flow: `handle_fasteoi` on a level line, `handle_edge` on an edge line, run under L1. On a Oneshot level line it masks the line before the handler, sets held if the handler answers WakeThread, and unmasks before the end of interrupt only if the line is not held (step 6).
- held: one bit of the line's flag word, under L1. Set, it means the line's one Oneshot action still owes its device a service.
- `irq::finalize_oneshot(line, action)`: takes L1; if the action is still on that line and the line is Oneshot, clears held and unmasks the line.
- The pin's IO-APIC entry: the only record of whether the line is masked.

The events of a level interrupt on a bound object, before and after:

| Step | Before (Zircon's flags) | After (Oneshot) |
|---|---|---|
| Interrupt, object Idle | handler sends the packet, then MaskPostwait masks the line, under L2 | flow masks the line under L1, handler sends the packet, answers WakeThread, flow sets held and leaves the line masked |
| Interrupt, object in NeedAck | handler folds the time; the line is masked already | flow masks again (idempotent), handler folds the time, WakeThread keeps held set |
| `ack()` with an interrupt folded since | UnmaskPrewait unmasks under L2, then sending the next packet masks again under L2 | sends the next packet and stays in NeedAck; nothing touches the line |
| `ack()` with nothing since | UnmaskPrewait unmasks under L2, moves to Idle | moves to Idle, drops L2, then finalizes: held cleared, line unmasked, under L1 |
| `destroy()` | masks the line through `irq::mask()`, then detaches | `mask_interrupt()` does nothing; the detach clears held and masks the line it empties, under L1 |

The unbound object follows the same table with the wait in place of the ack. A wait in NeedAck that takes an interrupt folded since is the "ack that sends the next one"; a wait in NeedAck that finds nothing pending is the "ack with nothing since", and it finalizes before it parks.

Why each piece is the way it is:

Why the handler always answers WakeThread:
- WakeThread is how a Oneshot action tells the line "my handler is done, but the device is not serviced yet". For an interrupt object that is true of every interrupt. A delivered one went to a waiter or a port reader, and the driver behind them services the device later. A coalesced one, which arrived in NeedAck, is owed the same ack as the one before.
- Answering Handled for a coalesced interrupt would be wrong in a quiet way. The flow would unmask the line unless held was set, and held is set, so nothing visible would change today. But it would make the line's state depend on an earlier interrupt's answer, which is exactly the coupling held exists to remove.

Why WakeThread even once Destroyed:
- `destroy()` sets Destroyed under L2 and only later detaches under L1. A level interrupt that arrives in between finds Destroyed and delivers nothing.
- If the handler answered Handled there, the flow would unmask the line before the end of interrupt. The device still asserts the pin, nobody will service it, and the pin raises the same interrupt again as soon as it is ended: a storm on that CPU until the detach.
- With WakeThread the flow leaves the line held and masked, and the detach clears held and keeps the line masked, as it does for any line it empties. No test plays an interrupt in that window: the `interruptline.test` mutant below answers Handled on every interrupt, which shows that the level mask depends on the answer, but nothing holds a destroy between Destroyed and its detach, so a handler that answered Handled only once Destroyed would pass every check.

Why `mask_interrupt()` is empty but kept:
- D1 (b) keeps the four virtuals. MSI may later want Zircon's model, an object that masks inside its device, and the virtual is where that goes.
- For a line object there is nothing left for it to do: the flow masks a level line from each interrupt, and the detach masks the line it lets go. Both run under L1, so the mask has one writer.

Why `unmask_interrupt()` is called only when the debt is cleared:
- The finalize says "nothing is owed". After an ack that sends the next packet, something is owed again: the driver has not seen that packet yet, so its device has not been serviced for it. Unmasking there would let a level pin that is still asserted fire again before the driver acts.
- The old code unmasked and then masked again inside one hold of L2 in that case. That was harmless only because both writes happened under the same lock as the state change.
- So `ack()` returns from inside its locked scope on every path except the one that moves to Idle, and only that path reaches the finalize. `begin_wait()` reports `Debt::Cleared` only for a wait in NeedAck that finds nothing pending.

Why outside L2:
- A dispatch holds L1 and the handler takes L2: the order is L1 -> L2.
- The finalize takes L1. Called with L2 held, it would take L2 -> L1. The worst interleaving: CPU0 dispatches the line, holds L1, and spins on L2 in the handler. CPU1 is in `ack()`, holds L2, and spins on L1 in the finalize. Neither can proceed: an ABBA deadlock, with interrupts off on both CPUs.
- So both callers drop L2 first. `ack()` closes its locked scope before the call, and `begin_wait()` returns a `WaitStart` so that `wait()` makes the call after the guard is gone.

Why the deferred unmask can be combined with a port here, when Zircon refuses that combination:
- Zircon's `INTERRUPT_UNMASK_PREWAIT_UNLOCKED` also unmasks after dropping the object's lock. But its `ack` decides to defer the unmask before it looks for a pending interrupt, and then may send the next packet, which `INTERRUPT_MASK_POSTWAIT` masks under the lock. The deferred unmask then undoes that mask.
- So Zircon's `InterruptDispatcher::Bind` refuses an object that has both flags (`interrupt_dispatcher.cc`, "a mask operation immediately followed by the deferred unmask operation").
- Ours decides to finalize only after it knows nothing more is to be sent, so the problem does not arise, and a level object can still be bound.

Why `wait()` finalizes before the test hook and the park:
- It must happen after L2 is dropped, for the reason above, and before the park, because a parked thread can do nothing.
- Between the two it goes before `BEFORE_PARK_HOOK`, as DESIGN Events 5 says. `interrupt.test` uses that hook to play another thread's trigger while a waiter has claimed the wait but not parked yet. That is DESIGN Open questions' "interleaving B", which stays the user's to argue, so the hook must still sit after every step a real waiter takes.

What this leaves open, on purpose:
- Between dropping L2 and the finalize taking L1, the object is already Idle (or Waiting) but the line is still held.
- An interrupt dispatched in that gap is delivered (or sent as a packet), and the object goes back into NeedAck. The finalize then clears held and unmasks a line whose object is owed an ack again.
- Whether a real level interrupt can land there, what a hand-raised one does, and why ours needs neither of Linux's guards (`irq_finalize_oneshot()`'s INPROGRESS spin and RUNTHREAD re-check) is the DESIGN's "finalize exercise", left to the user. The code comments point at it rather than answer it.

Why `m_flags` became `Flags const`:
- It was guarded by L2 only because `set_level_triggered()` added the level bits after the object was already on its line, while a handler might be reading them.
- With those bits gone, Virtual is the only bit, and it is fixed at construction. A word that never changes after construction needs no lock, and `trigger()` now reads it before taking L2.

Why `create()` no longer looks at the mode attach answers:
- The object used it to set its own masking flags.
- Now the line's flow follows the mode, chosen under L1 in the same attach. So the object behaves the same on either kind of line, and the line decides.
- `attach()` with a mode is still one step under L1 for the same reason as before. As two steps, the owner that took a free line could hold it in the mode the other racer set, and so with the wrong flow.

**Change**
The handler, before and after:

```cpp
// before
auto InterruptEventDispatcher::on_interrupt(VIrqNum, void *const context) -> irq::IrqReturn {
    static_cast<Self *>(context)->handle_interrupt();
    return irq::IrqReturn::Handled;
}
```

```cpp
// after
auto InterruptEventDispatcher::on_interrupt(VIrqNum, void *const context) -> irq::IrqReturn {
    static_cast<Self *>(context)->handle_interrupt();
    return irq::IrqReturn::WakeThread;
}

auto mask_interrupt() -> void override {}

auto InterruptEventDispatcher::unmask_interrupt() -> void {
    irq::finalize_oneshot(m_line, m_action);
}
```

The ack, before and after:

```cpp
// before
Guard<sync::SpinLock, IrqSaveAndPreemptDisable> const guard{m_spinlock};
// ... BadState, Cancelled, not NeedAck ...
if (!!(m_flags & Flags::UnmaskPrewait)) {
    unmask_interrupt();                 // irq::unmask(), under L2
}
if (m_timestamp.is_some()) {
    if (!send_packet_locked()) {        // masks again under MaskPostwait
        return Status::BadState;
    }
    return Status::Ok;
}
m_state = State::Idle;
return Status::Ok;
```

```cpp
// after
{
    Guard<sync::SpinLock, IrqSaveAndPreemptDisable> const guard{m_spinlock};
    // ... BadState, Cancelled, not NeedAck ...
    if (m_timestamp.is_some()) {
        if (!send_packet_locked()) {
            return Status::BadState;
        }
        return Status::Ok;              // still owed an ack: no finalize
    }
    m_state = State::Idle;
}
unmask_interrupt();                     // the finalize, under L1, with L2 dropped
return Status::Ok;
```

The wait:

```cpp
// after
for (;;) {
    auto const start = begin_wait(out_timestamp);
    if (start.answer.is_some()) {
        return *start.answer;
    }
    if (start.debt == Debt::Cleared) {
        unmask_interrupt();
    }
    // BEFORE_PARK_HOOK, then park()
}
```

`begin_wait()` used to return `ustl::Option<Status>`, where "none" meant "park". It now returns `WaitStart { ustl::Option<Status> answer; Debt debt; }`, because the caller needs a third outcome: park after telling the line the debt is cleared. Rejected shapes:
- A `bool acked` in the struct. The codebase's rule puts yes/no facts in named enums or Flags words, and `Debt::Cleared` reads as what it means at the call site.
- A three-valued enum with the answer in an out-parameter. That gives `begin_wait()` two out-parameters for one result.
- Doing the finalize inside `begin_wait()` after an inner scope. That splits "wait()'s first half, under m_spinlock" into a locked part and an unlocked part, and hides from `wait()` a lock-order step that belongs beside the hook and the park.

The review's second fixup has the compiler hold the callers to that order: the four virtuals are declared `TA_EXCLUDES(m_spinlock)`, so the thread-safety analysis refuses a call made under L2. No lock checker sees L1 (DESIGN, Testing and verification: lockdep for L1 and LI is not available), so before this "never called with L2 held" rested on reading every caller, and a later edit that moved `ack()`'s finalize back inside its locked scope would have built, booted and passed every test until two CPUs met in the ABBA interleaving above.

The test: `check_take_line()`'s first dispatch on an object's line now expects `WakeThread`, the handler's new answer, where it expected `Handled`.

**Reference**
- Linux: the ONESHOT protocol this switches the objects to. `handle_fasteoi_irq()` masks a ONESHOT line through `mask_irq()` and leaves it masked through `cond_unmask_eoi_irq()` while `desc->threads_oneshot` is set (`kernel/irq/chip.c`). The threaded half is `irq_thread_fn()`, which calls the thread function and then `irq_finalize_oneshot()` (`kernel/irq/manage.c`). Here the threaded half is the interrupt object together with its user thread, and the finalize is `unmask_interrupt()`. The difference: `irq_thread_fn()` finalizes after every thread run, while ours finalizes only when the object's NeedAck debt is cleared, because one user-space ack may stand for several coalesced interrupts.
- Zircon: `InterruptDispatcher::WaitForInterrupt()` and `InterruptDispatcher::Ack()` with `INTERRUPT_UNMASK_PREWAIT_UNLOCKED`, which defer the unmask until the dispatcher's spinlock is dropped (`kernel/object/interrupt_dispatcher.cc`), and `InterruptDispatcher::Bind()`, which refuses that flag together with `INTERRUPT_MASK_POSTWAIT`. The flags ours removes are the counterparts of `INTERRUPT_UNMASK_PREWAIT` and `INTERRUPT_MASK_POSTWAIT`, which `InterruptEventDispatcher::Create()` sets for a level line (`kernel/object/interrupt_event_dispatcher.cc`).

**Verification**
- Build of `out/bringup.x86` in `.worktrees/irq-genirq`: ok, `-Werror` with the thread-safety analysis on.
- `scripts/format_cpp.py --check`: clean on all seven changed C++ files.
- `clang-tidy -p out/bringup.x86` on `interrupt_dispatcher.cpp`, `interrupt_event_dispatcher.cpp`, `interrupt_line_test.cpp` and `desc.cpp`: nothing reported in any of the four or in the headers they include from this change.
- `boot_check.py`: dbg PASS (33 passed, 3 skipped, 0 failed), rel PASS (31 passed, 9 skipped, 6/6 required); rel.log has `pc-ps2: key a`, `pc-ps2: key left-shift` and `pc-ps2: key A`. The keyboard's ISA 1 line, taken in the default mode, is edge on q35, so its object's every ack now finalizes the line, which unmasks an already unmasked pin.
- `kshell_run.py --out out/bringup.x86 irq.test interruptline.test interrupt.test`: irq.test ok (69 checks), interruptline.test ok (58), interrupt.test ok (32), the same counts as before the change. The seven level mask checks step 5 wrote against the old objects, the edge check and both destroy checks all pass.
- One mutant run: `on_interrupt()` answering `Handled` instead of `WakeThread`. `interruptline.test` failed 8 of 58: the changed WakeThread check and the seven level mask checks (masked by an interrupt, kept masked through the wait that takes it, through the wait that takes the next one, after the parked wait's interrupt, bound as the packet goes out, after an ack that sends the next one, and after an interrupt raised through the interrupt entry). The edge and destroy checks passed. So the level mask now comes from the flow alone, and only through the handler's answer.
- Boots run for this step: two boot_checks (dbg and rel each; the second after a comment-only edit, on the tree as committed, dbg PASS at 45.0s and rel PASS with 6/6 required and both `pc-ps2: key` lines), one kshell_run of the three tests, and one kshell_run of `interruptline.test` against the mutant.
- Review: a second mutant, two mutations in one build, aimed at the two places the state machine now decides the debt is cleared: `ack()` also finalizing on the path that sends the next packet, and `wait()` ignoring `Debt::Cleared`. `interruptline.test` failed exactly 2 of 58: "an ack that sends the interrupt since leaves the line masked" and "the next wait acks it: it unmasks the line and parks". The review's fixup changes comments only (two that named the design after its model, a stale reason in `create()`'s comment, and a stale order in a test comment); after it, one boot_check (dbg PASS, rel PASS with 6/6 required and both `pc-ps2: key` lines) and one kshell_run of the three tests, with the same counts.
- Review, second fixup: a compile-only mutant that moves `ack()`'s `unmask_interrupt()` inside its locked scope now fails the build with `error: cannot call function 'unmask_interrupt' while SpinLock 'm_spinlock' is held [-Werror,-Wthread-safety-analysis]`; without the annotation it compiled. clang-tidy on `interrupt_dispatcher.cpp`, `interrupt_event_dispatcher.cpp` and `virtual_interrupt_dispatcher.cpp`: nothing in an interrupt object's file. Then one boot_check (dbg PASS, rel PASS with 6/6 required and both `pc-ps2: key` lines) and one kshell_run of the three tests (69, 58 and 32 checks, all ok). In all the review ran three boot_checks of both images and four kshell_runs of dbg, one of them against the mutant.

**Deliberately not done**
- `irq::mask()` and `irq::unmask()` still take no L1, and `irq::unmask()` still ignores held. That is step 8. Until then an interrupt object's one call to `irq::unmask()`, in `create()` to start the line, is the only unmask it makes outside the finalize. The line is masked from the attach until that call, so only an interrupt raised by hand, or one already on its way before the mask, can set held in between; the unmask then leaves a held line unmasked, and a level pin still asserted raises one more interrupt, which the flow masks and holds again. Step 8's unmask, which leaves a held line masked, closes that window.
- No test plays an interrupt between `destroy()` setting Destroyed and its detach, so the handler's WakeThread answer once Destroyed rests on the argument in Cause, not on a check. Playing it needs a test hook inside `destroy()`, as `BEFORE_PARK_HOOK` is inside `wait()`, which this step does not add.
- No finalize skipped on edge lines. An edge object's every ack now takes L1 and rewrites the pin's IO-APIC entry, where it did nothing before. The DESIGN lists skipping that as performance work for the user; it is recorded as item 1 of `docs/repairs/261007/kernel-irq.md`.
- The gap between dropping L2 and the finalize taking L1 is neither closed nor argued in the code: it is the DESIGN's finalize exercise, the user's.
- `deactivate_interrupt()` and `unregister_interrupt_handler()` are untouched; `destroy()` still calls `mask_interrupt()`, now a no-op for a line object, so that a kind that masks inside its device keeps its place in the order.

**How it was found**
None. This is step 7 of the irq-genirq DESIGN's Staging (D1 (b)), with every decision taken as recommended on 2026-10-05.

**The user's prediction**
None: the user skipped the prediction for D1 on 2026-10-05.

**Three questions you should be able to answer**
- An ack finds an interrupt folded in since the last packet and sends it. Why must it not call `unmask_interrupt()`, and what would a level device still asserting its pin do if it did?
- Why does `on_interrupt()` answer WakeThread even when `handle_interrupt()` found the object Destroyed and delivered nothing? What happens on a level line between the destroy setting Destroyed and its detach if it answered Handled?
- `ack()` used to unmask under L2. Name the two locks a finalize under L2 would take in the wrong order, and the interleaving of two CPUs that deadlocks.
