# kernel-object changes

## A virtual interrupt raises Idle while it is owed no ack, so the driver of a controller can wait for a level child's ack (2026-11-01)

**Files**
- `system/public/ours/signals.hpp` — `Signals::Idle`, bit 6: the ABI, with its meaning.
- `kernel/object/include/ours/object/interrupt_dispatcher.hpp`, `kernel/object/interrupt_dispatcher.cpp` — two hooks for a kind's signal, `on_trigger()` and `on_destroy()`; `trigger()` calls them (the drop first, the raise again if it finds the object destroyed) and `destroy()` calls the second.
- `kernel/object/include/ours/object/virtual_interrupt_dispatcher.hpp`, `kernel/object/virtual_interrupt_dispatcher.cpp` — the virtual kind keeps the signal: up at the creation, dropped by `on_trigger()`, raised by `unmask_interrupt()` and `on_destroy()`.
- `system/public/ours/syscalls.gni` — six entries say what Idle is: `interrupt_create`, `interrupt_trigger`, `interrupt_wait`, `interrupt_ack`, `interrupt_destroy`, `object_wait_one`.
- `kernel/commands/interrupt_test.cpp`, `system/app/shell/shell_tests.cpp` — `interrupt.test` grows from 32 to 55 checks, and the user shell checks the signal and a driver's loop through the system calls.
- `.agents/tasks/irq-multi-arch/{DESIGN,PLAN}.md`, `.agents/skills/ours-kernel-design/references/task.md` — step 4 done, D5 and the naming decision recorded.

**Symptom**
Latent, cannot be triggered today: no driver for a controller behind a root line exists. It would show as a driver that cannot tell when to unmask a pin. A driver G for a bank of GPIO pins has one parent interrupt and makes one virtual interrupt for each pin, which it raises with `interrupt_trigger` when the pin fires. For a level pin the cause stays asserted until the client K has cleared it in its device, so G must keep the pin masked in the bank's own register until K has acked, or the parent fires again the moment G acks it. G could not know when K acked: the virtual kind's `unmask_interrupt()`, the one place the object learns its debt is paid, was empty.

**Cause**
The object already knew. `InterruptDispatcher` calls `unmask_interrupt()` exactly when the object is owed no ack: in `ack()` when it moves to Idle, and in a `wait()` that finds the last interrupt taken and nothing pending, since that wait is the ack. A physical kind spends that moment unmasking its line. The virtual kind has no line, so it did nothing with it, and its header said so ("a signal ... is left out: nothing here asks for that yet"). Nothing a program could wait on changed at that moment, and a message from K to G would have made K know that its interrupt is virtual, which a client of a physical interrupt never has to.

Three details decide the shape.

- **A level, not an event.** G triggers and then waits for the ack; K may ack before G gets to the wait. An event would be lost, and G would wait for ever with the pin masked. A signal that stays up until the next trigger cannot be missed, and `object_wait_one` and `object_wait_async` are level-triggered already, so a wait that arrives late returns at once.
- **Dropped before the delivery.** `trigger()` drops Idle first and delivers second. Delivered first and dropped after, K, woken by the delivery, could take the interrupt and ack it, raising Idle, in the few instructions before the drop, and the drop would undo the raise: Idle down with nothing owed, and G waiting for ever. Dropped first, any ack that follows the delivery is the later of the two.
- **Outside the spin lock.** `Dispatcher::update_state()` takes a Mutex, in thread context only, and the object's state is changed under a spin lock with preemption off. So the signal is raised and dropped after the state change, with the lock dropped, at the places `unmask_interrupt()` is already called from. That costs a gap between the state and the signal in which a waiter sees the old signal. It is harmless for a level pin, which G triggers once for each unmask, so no second trigger can overtake the ack of the first; for an edge pin, which G triggers freely and never waits on, nobody reads the signal.

**Change**
The signal, and the kinds' hooks:

```cpp
// interrupt_dispatcher.cpp, trigger()
on_trigger();                       // Idle down, before anything can take the interrupt
auto status = Status::Ok;
{
    Guard<sync::SpinLock, IrqSaveAndPreemptDisable> const guard{m_spinlock};
    if (m_state == State::Destroyed) { status = Status::Cancelled; } else { /* deliver */ }
}
if (status == Status::Cancelled) { on_destroy(); }   // a destroyed object owes nothing

// virtual_interrupt_dispatcher.cpp
auto VirtualInterruptDispatcher::unmask_interrupt() -> void { update_state(Signals::None, Signals::Idle); }
auto VirtualInterruptDispatcher::on_trigger() -> void { update_state(Signals::Idle, Signals::None); }
auto VirtualInterruptDispatcher::on_destroy() -> void { update_state(Signals::None, Signals::Idle); }
```

`create()` raises Idle before any handle exists. `destroy()` calls `on_destroy()` right after the state is set, so that a thread it wakes finds the object destroyed. The signal's meaning, at each moment of a virtual interrupt's life:

| Event | Idle |
|---|---|
| created | up |
| `interrupt_trigger` Ok, folded into a pending interrupt or not | down |
| a wait or an `interrupt_ack` that leaves nothing more to deliver | up |
| an ack or a wait that takes the next, pending interrupt | stays down |
| `interrupt_destroy`, and a trigger the destroyed object refuses | up, for good |

The client's handle is made by G with `handle_duplicate`: Wait, Read, Write and Transfer, no Signal and no Duplicate. `interrupt_trigger` and a duplicate on it answer AccessDenied (checked by the shell test); `interrupt_destroy` asks for no right, so K can destroy its pin's interrupt, which raises Idle and makes G's next trigger answer Cancelled.

Rejected: a signal on a physical interrupt too (its ack is the kernel's own unmask of the line, and the cost of a Mutex on that path is for nothing); a message from K to G (K would tell the two kinds apart); a per-handle "closed" signal (the object has no such notification, and G's own handle keeps it alive); a new system call (the signal is read with the calls that exist). Naming: the calls stay `interrupt_*` and the types stay Interrupt (irq-multi-arch D11, "保留吧").

**Reference**
Zircon's virtual interrupt has a signal for the same moment, `ZX_VIRTUAL_INTERRUPT_UNTRIGGERED` (from memory, unchecked; its sources were unreachable on 2026-11-01). Linux's chained level handler for a GPIO bank masks the child in its `irq_chip`, runs the child's handler, and unmasks it at the end of the child's flow with `irq_unmask`; here the child's handler is a user program, and the signal stands where that unmask callback would be.

**Verification**
- `interrupt.test`, the kernel-shell test, 55 checks all ok (32 before). New: Idle up on a new interrupt; a trigger, a trigger that folds into the pending interrupt, and a wait that takes it leave it down; a wait that acks the last interrupt and parks, on a second thread, raises it, and a trigger drops it and wakes the waiter; a destroy raises it, and a trigger the destroyed object refuses leaves it up; on a port, a trigger sends the packet and drops it, an ack that sends the folded interrupt as the next packet leaves it down, the ack that leaves nothing raises it, and an observer attached while it was down is told once.
- `boot_check.py`: dbg PASS, 45 passed and 4 skipped (44 before); rel PASS, 42 passed and 8 skipped, 6 of 6 required (41 before); host tests 154 passed. The new line in each is the user shell's `interrupt idle checks`: Idle seen through `object_wait_one` on a new interrupt, a triggered one, a taken one and a destroyed one; then a driver's loop on one `BindToInterrupt` port: the parent's Interrupt packet, the child's `object_wait_async` for Idle with no packet while K has not acked, K's packet read from a second port and acked, the SignalOne packet for Idle with the driver's key, and the same after K's destroy, followed by Cancelled on the next trigger. That port takes Interrupt packets and `object_wait_async` packets together, which was unchecked before.
- clang-tidy on the changed kernel sources: no finding. Five mutations, each restored byte for byte, each killed by `interrupt.test`: the trigger not dropping Idle (11 checks fail); the ack not raising it (3); a destroy not raising it (2); a refused trigger not raising it again (1); a new interrupt not Idle (2).
- Not verified: the order of the drop before the delivery, which is argued above and not tested, since the race it prevents needs a client's ack to land inside a few instructions and no test of this tree can aim at it (a mutation that drops after delivering would survive); the cost of the Mutex that every trigger of a virtual interrupt now takes, not measured (virtual interrupts are used by tests and by drivers that have not been written); a real GPIO driver; a client that dies without an ack, which by design keeps the pin masked at the driver.

**Deliberately not done**
- No signal on a physical interrupt, no `object_signal`, no notification that a client's handle was closed, no new system call.
- The Driver Manager's side: handing G a new interrupt for a pin whose client died (`.agents/tasks/driver-manager/DESIGN.md`) is not built.
- Step 5 of the plan, AccessDenied for a source the kernel owns, is separate.

**How it was found**
By the review of the cascade design (`.agents/tasks/irq-multi-arch/DESIGN.md`, 2026-11-01): walking the events of a level child on a level parent, the reviewer found that the controller's driver cannot see the client's ack, and that `unmask_interrupt()` was empty for the virtual kind at exactly that moment.

**The user's prediction**
None. The user approved the interface list that named `Signals::Idle` at bit 6 ("可以", 2026-11-01), and kept the `interrupt_*` names ("保留吧").

**Three questions you should be able to answer**
- Why must G keep a level pin masked in its own register until K has acked, when the kernel already holds the parent line masked until G acks it?
- Why is Idle a level and not an event, and what would G do if it were an event and K acked first?
- Why does `trigger()` drop Idle before it delivers the interrupt, and what would the order after cost?

## `Idle` follows the state: it is set again after every change that moves the debt, and the ordering argument of the first version is gone (2026-11-01)

**Files**
- `kernel/object/include/ours/object/interrupt_dispatcher.hpp`, `kernel/object/interrupt_dispatcher.cpp` — `sync_signals()` replaces the two hooks `on_trigger()` and `on_destroy()`; `trigger()` and `destroy()` call it after the state change.
- `kernel/object/include/ours/object/virtual_interrupt_dispatcher.hpp`, `kernel/object/virtual_interrupt_dispatcher.cpp` — `unmask_interrupt()` and `create()` call it; the two hook overrides are gone.
- `system/public/ours/signals.hpp`, `system/public/ours/syscalls.gni` — the wording of Idle: which wait raises it, that it follows the state, that the client can still destroy.
- `kernel/commands/interrupt_test.cpp`, `system/app/shell/shell_tests.cpp` — a check with triggers racing a taker's waits and acks; a handle closed without an ack; every handle closed on every path.
- `.agents/tasks/irq-multi-arch/{DESIGN,PLAN}.md`, `.agents/skills/ours-kernel-design/references/task.md`, `docs/guide/01-concepts.md` — the corrections the review asked for.

**Symptom**
Latent, cannot be triggered by a driver that follows its protocol. Found by the independent review of the previous entry. With the first version, a trigger that landed between an ack's change of state and the ack's raise of Idle left Idle up while an ack was owed: K parks in a wait that is the ack, the driver triggers again before K's wait has set the signal, K's wait then takes the new interrupt, and Idle goes up behind it. A driver that then waits for Idle unmasks a level pin while K is still servicing it. A driver that triggers once for each unmask cannot reach it, because it triggers again only after it has seen Idle; one that triggers an edge pin freely, or that gives up its wait for Idle and triggers again, can.

**Cause**
The signal was a toggle, not a function of the state. The state of the interrupt changes under a spin lock, in one step; the signal is set afterwards, under a Mutex, in another; and two CPUs doing the two steps for two different events can interleave them. The first version arranged one order, the drop before the delivery, so that a client's ack could not be undone by a drop that came after it. That fixed the interleaving it was written for and none of the others: an ack's raise can still land after a later trigger's drop and delivery, because between the ack's state change and its raise nothing orders it against a trigger on another CPU. The argument ("the ack of it is the later of the two") was true of one trigger and one ack and said nothing of a trigger that comes while an earlier ack is still on its way to the signal.

**Change**
The signal is set from the state, after every change that moves the debt, and set again if the debt moved while it was being set:

```cpp
auto InterruptDispatcher::sync_signals() -> void {
    if (!(m_flags & Flags::Virtual)) { return; }
    auto const owed = [this]() -> bool {
        Guard<sync::SpinLock, IrqSave> const guard{m_spinlock};
        return m_state == State::Triggered || m_state == State::NeedAck;
    };
    auto now = owed();
    for (;;) {
        if (now) { update_state(Signals::Idle, Signals::None); }
        else     { update_state(Signals::None, Signals::Idle); }
        auto const again = owed();
        if (again == now) { return; }
        now = again;
    }
}
```

Why it settles: the last change to the state is followed by a call that reads the state after it, so the last write of that call is the right one; an older call that writes after it read an older state and so reads the state again, finds it changed, and writes again. It is called at the creation, at the end of `trigger()` (whether it delivered or found the object destroyed), in `destroy()` after the state is set, and at the seam `unmask_interrupt()` is called from, which is where the debt is cleared. The two hooks are gone, and so is the question of which comes first. The signal can lag the state only while a call on the same interrupt is in progress on another CPU, which `signals.hpp` now says.

Corrections to the previous entry, which stays as history:
- Its Cause, third bullet, said the signal was raised and dropped after the state change. The drop was before it, as the second bullet said; both are now after.
- Its table said a trigger the destroyed object refuses leaves Idle up "within the refused trigger"; it simply leaves it up now, since the state is read.
- Its Reference named no Linux function. The counterpart of the unmask at the end of a level child's flow is `unmask_irq()` called from `cond_unmask_irq()` in `handle_level_irq()` (from memory, unchecked).
- It cited `.agents/tasks/driver-manager/DESIGN.md` for handing the driver a new interrupt for a pin whose client left. That document does not design it; who does is an open question in `.agents/tasks/irq-multi-arch/DESIGN.md`.
- The client's handle has no Duplicate and no Signal, but `interrupt_destroy` asks for no right, so it can raise Idle without an ack; `syscalls.gni` says so now.

**Reference**
None new: Zircon's signal and Linux's `handle_level_irq()` are cited above and in the previous entry, both from memory.

**Verification**
- `interrupt.test`: 58 checks all ok (55 before). New: with triggers on one CPU racing a taker's waits and acks on another, once the taker has parked, Idle is up, and three more triggers each bring it down and the ack after it back up. The user shell's `interrupt idle checks` adds a client handle closed without an ack, which raises nothing and sends no packet.
- `boot_check.py`: dbg PASS, 45 passed and 4 skipped; rel PASS, 42 passed and 8 skipped, 6 of 6 required; host tests 154 passed. clang-tidy on the changed sources: no finding.
- Six mutations, each restored byte for byte. Five killed by `interrupt.test`: `trigger()` not calling `sync_signals()` (12 failures); the ack seam not calling it (6); `destroy()` not calling it (2); a taken interrupt not counted as owed (8); a parked waiter counted as owed (4). One survived: removing the second read of the state, so that the signal is set once. It was run again with 3 runs of 3000 rounds each and still survived. The window it closes is a few instructions wide, and the test of this tree cannot aim at it; the loop is argued above and not tested.
- Not verified: the loop, as said; the cost of the Mutex and the extra spin-lock reads on each trigger and ack of a virtual interrupt, not measured; a real driver.

**Deliberately not done**
- The Driver Manager side (replacing the interrupt for a pin whose client left) is not designed, and so not built.
- Raising the signal on a physical interrupt, a new system call, a right for `interrupt_destroy`: as in the previous entry, and the right is a decision for the user (`Rights::Destroy` exists and is not in the default rights of an interrupt).

**How it was found**
By the independent review of the previous entry (three reviewers, 2026-11-01): the one who listed every transition of the state found that after an ack's state change and before its raise, a trigger on another CPU leaves the two disagreeing for as long as the object stays owed.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does reading the state a second time make two racing calls end with the right signal, and what would happen with one read?
- Why can a driver that triggers once for each unmask never see the stale Idle that an edge driver could?
- Why is a signal that is a function of the state easier to reason about than one dropped and raised at the moments of the changes?
