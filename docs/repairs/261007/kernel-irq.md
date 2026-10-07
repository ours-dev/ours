# kernel-irq outstanding repairs

## 1. An edge object's every ack, and every wait that serves as one, takes the line's lock and rewrites the pin's IO-APIC entry to unmask a pin that is already unmasked

**Severity**
Low: a cost, not a wrong result. It is paid once per ack on an edge line, such as each keyboard interrupt that the pc-ps2 driver acks, and nothing measures it yet.

**Symptom**
Latent, not measured. Since irq-genirq step 7, `InterruptDispatcher::ack()` and a wait in NeedAck that finds nothing pending call `unmask_interrupt()`, which is `irq::finalize_oneshot()`, on every line object. On an edge line that does the following, and before step 7 it did nothing:
- takes the line's lock L1 with interrupts off;
- runs `Desc::finalize_oneshot()`, which finds the line Oneshot and clears a held bit that the edge flow never sets;
- calls the IO-APIC chip's unmask, a read-modify-write of the pin's redirection entry under the IO-APIC register lock LI.

Under QEMU each IO-APIC register access is an exit to the host, so this costs host time.

**Cause**
The object attaches Oneshot on every line, because it cannot know which flow the line will run: the line chooses its flow from its mode. The edge flow ignores Oneshot and never sets held. `Desc::finalize_oneshot()` still unmasks on any Oneshot line, without asking whether the line is held or which flow it runs. Step 6 chose that on purpose: held records a debt, not the mask, and an unmask when nothing was owed is idempotent at the register. The DESIGN's Non-goals list "skipping the finalize on edge acks" as performance work to put to the user, not to do quietly.

**Change**
Measure first: count the cycles from the finalize's call to its return on the keyboard's line, for example with the thread sampler or a `benches/` loop that acks an edge object on the free line. Then compare with the cost of an ack without it. Candidates, each with what it costs:
- (a) `finalize_oneshot()` returns early when the line runs the edge flow. Under L1 this reads one more byte; the line's lock is still taken.
- (b) `finalize_oneshot()` unmasks only when held was set. This saves the register access on edge lines and on level lines whose last interrupt was handled without a thread. It needs an argument that no path leaves a Oneshot line masked with held clear.
- (c) The object attaches Oneshot only on a level line. This puts back into the object the mode knowledge that step 7 removed, and a line's mode cannot change while the object holds it, but the object would again depend on the line's mode.

The choice goes to the user, with the measurement.

**Reference**
Linux attaches `IRQF_ONESHOT` regardless of trigger. On an edge line `handle_edge_irq()` never masks, but `irq_finalize_oneshot()` still runs after each thread run. It returns early when `!(desc->istate & IRQS_ONESHOT)`, and otherwise takes `desc->lock` and unmasks through `unmask_threaded_irq()` only if the line is masked (`irqd_irq_masked()`), which needs the software Masked bit this design defers.

**Acceptance**
A measurement of the finalize's cost on an edge line is recorded in the change record of whichever option the user picks. After the change, `interruptline.test` still passes its edge check ("an edge object's line is never masked"), and `irq.test` still passes its finalize checks.
