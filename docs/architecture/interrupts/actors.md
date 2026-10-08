# Who says what to whom in an interrupt

Four parties take part in every device interrupt, and each "acknowledgement" goes between a different pair of them. Mixing them up is the usual source of confusion: neither EOI nor ack quiets a device. Only the driver, by servicing the device, does. The design behind this is in [microkernel-interrupts](../../../.agents/tasks/microkernel-interrupts/DESIGN.md) and [irq-genirq](../../../.agents/tasks/irq-genirq/DESIGN.md).

## The parties

- **Device**: the hardware that wants attention, such as the PS/2 controller (i8042). It raises its interrupt pin and keeps it raised until it is serviced, when the line is level-triggered.
- **Interrupt controller**: two parts.
  - The **IO-APIC** owns the pins: whether each one is masked, how it signals (edge or level), and, for a level pin, the Remote IRR bit that holds the pin back until its interrupt is ended.
  - Each CPU's **local APIC** owns which interrupt that CPU is serving now. It holds back every interrupt of the same priority and below until it is told the CPU is done.
- **Kernel**: kernel/irq and the interrupt objects. It knows lines, masks and the protocol with drivers, and nothing about any device.
- **Driver**: a user program, such as pc-ps2. It is the only party that knows the device's registers.

## The messages

| From | To | What | Meaning |
|---|---|---|---|
| Device | Interrupt controller | raises the pin | "I need attention" |
| Interrupt controller | Kernel | delivers a vector on a CPU | "a pin you route here is asking" |
| Kernel | Interrupt controller | **EOI** (end of interrupt), to the local APIC | "this CPU is done with that interrupt"; for a level pin the local APIC passes it on to the IO-APIC, which clears Remote IRR and looks at the pin again |
| Kernel | Interrupt controller | **mask / unmask** a line, at the IO-APIC | "hold this pin back" / "let it through again" |
| Kernel | Driver | a port packet, or wakes the waiting thread | "your device asked" |
| Driver | Device | reads and writes the device's registers | services the device; the device drops its pin. **The only message that quiets the device** |
| Driver | Kernel | **ack** (`interrupt_ack`, or the next wait) | "I am done with that one; let the next in" |

In one line each:

- **EOI** is the kernel's reply to the interrupt controller.
- **ack** is the driver's reply to the kernel.
- **servicing** is the driver's work on the device, and it has no reply.

## One level-triggered interrupt, in order

1. The device raises its pin; the IO-APIC delivers a vector to a CPU.
2. The kernel masks the line at the IO-APIC, runs the interrupt object's handler, which queues a packet, and marks the line held.
3. The kernel sends EOI. The pin is still raised, but the line is masked, so nothing comes in again.
4. The driver reads the packet and services the device. The device drops its pin.
5. The driver acks. The kernel's object becomes idle and calls `finalize_oneshot()`, which clears held and unmasks the line.
6. The pin is low, so nothing happens until the device has something new.

If the driver acks before step 4, the kernel unmasks a line whose pin is still raised. The IO-APIC delivers it again at once, and the driver gets a packet for an interrupt it has not finished. That costs an extra packet, not a storm: the line is masked again on that delivery.

An edge-triggered line (the PS/2 keyboard's ISA IRQ 1) skips the mask in step 2 and the unmask in step 5. An edge is one event, and the pin does not ask again until the device signals anew.

## Where it is in the code

- EOI: `EoiGuard` in `kernel/kernel/irq/desc.cpp`, ended once per interrupt as the line's flow returns.
- Mask, held and finalize: `handle_oneshot_locked()` and `Desc::finalize_oneshot()` in `kernel/kernel/irq/desc.cpp`.
- Ack: `InterruptDispatcher::ack()` in `kernel/object/interrupt_dispatcher.cpp`.
- A driver servicing, then acking: `system/driver/pc-ps2/pc_ps2.cpp`, which reads the i8042 and then calls `ours::interrupt_ack()`.
