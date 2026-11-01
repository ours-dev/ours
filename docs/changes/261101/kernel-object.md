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
