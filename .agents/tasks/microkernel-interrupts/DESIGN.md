# microkernel-interrupts: interrupt objects that let a user-space driver own an interrupt line

**Date**: 2026-10-04
**Status**: Accepted

## Problem
Paths without a prefix are relative to the ours repository root. `zircon/` is `~/Workspaces/projects/fuchsia/zircon`, `fuchsia/` is `~/Workspaces/projects/fuchsia` and `linux/` is `~/Workspaces/projects/linux`. Lines of `kernel/arch/x86/kernel/faults.cpp` are cited as committed at 116c12ca. Another session has uncommitted work in that file and in `kernel/arch/x86/lib/arch/include/arch/x86/interrupt.hpp`; in the shared worktree it moves every line of faults.cpp after :263 down by five. This design was translated from a proposal written in Chinese and checked against the sources on 2026-10-04.

In short:
- **The overall shape suits a microkernel.** `kernel/kernel/irq` (chips, a per-line object, observers, `request_irq`) is an in-kernel layer like Linux's genirq. Zircon has such a layer in the same place: `dev/interrupt` plus x86's `InterruptManager`. It stays, serving only the PIT and HPET, the debug serial port and, until a driver replaces it, PS/2. IPIs and the LAPIC timer use fixed vectors and never pass through it (`faults.cpp:250-286`; `zircon/kernel/arch/x86/exceptions_c.cc:503-542`).
- **The one structural gap is that an interrupt line cannot reach user space.** Three things are missing: an interrupt kernel object, an IRQ resource that authorizes it, and the protocol "mask on delivery, unmask only when the driver acks".
- **The layers below cannot carry that path yet.** The irq layer has no lock, no unregistration barrier and no context pointer. IO-APIC register access is unlocked. A port cannot be posted from IRQ context.

What forces it now: the component framework (`docs/design/261004/component-framework.md`) makes a process's capabilities a matter of routing, and a driver is the first component that needs something only the kernel has, an interrupt line. On 2026-10-04 the user asked for both plans to be built to an MVP.

### Today's objects
- `irq::IrqChip`: a table of function pointers for controller operations, Linux's `irq_chip` (`kernel/include/ours/irq/irq_chip.hpp:29-38`).
- `irq::IrqData`: hirq, virq, chip and `platform_data`, which defaults to nullptr (`irq_chip.hpp:16-25`).
- `irq::IrqObject`: one per line, roughly Linux's `irq_desc`. It holds a canary, a `sync::Mutex request_mutex_`, flags, the IrqData and a list of observers (`kernel/include/ours/irq/irq_object.hpp:15-41`).
- `irq::IrqObserver`: roughly Linux's `irqaction` (`kernel/include/ours/irq/irq_observer.hpp:11-40`).
- `irq::IrqDispatcher`: a `Span<IrqObject*>` indexed by VIrqNum, a function-local static singleton (`kernel/kernel/irq/mod.cpp:21-25`). It is not a kernel object, though its name collides with `object::*Dispatcher`.
- `domain.hpp` is empty and `action.hpp` holds only `class Action {}` (`kernel/include/ours/irq/action.hpp:11`). They are scaffolding, which the rules say not to delete (`.claude/skills/ours-kernel-design/SKILL.md:49-50`).
- The API: `request_irq(virq, handler, flags, name)` allocates an observer itself and takes no context argument; `release_irq(virq)` knows only the line number (`kernel/include/ours/irq/mod.hpp:17-18`, `kernel/kernel/irq/irq_dispatcher.cpp:53-57`).
- Number types: `HIrqNum` and `VIrqNum` are both plain `u32` aliases, and they live in the public ABI header (`system/public/ours/types.hpp:14-15`).
- Controllers: `IOAPIC_CHIP` and `XAPIC_CHIP` are both `[[maybe_unused]]` and bound to no line (`kernel/arch/x86/apic/io_apic.cpp:92-102`, `kernel/arch/x86/apic/local_apic.cpp:111-119`).
- There are two callers:
  - the timer, ISA 0, with Permanent (`kernel/arch/x86/kernel/time/time.cpp:52-53`);
  - the PS/2 keyboard (`kernel/platform/pc/keyboard.cpp:697-699`).

### Today's events: one keyboard interrupt
1. `platform_init_apic` programs ISA 0-15 (2 excepted) masked, with vector `32+isa`, delivered to the BSP, then runs `sti` (`kernel/platform/pc/interrupt.cpp:93-110`).
2. The keyboard's init calls `request_irq` with the GSI, checks the result only with DEBUG_ASSERT, then calls `apic_unmask_irq` directly (`keyboard.cpp:697-701`).
3. A key is pressed. The CPU enters `arch_handle_exception` and runs the preamble (`faults.cpp:415-419`).
4. A vector in 32..0xEF calls `platform_handle_irq(vector-32)` (`faults.cpp:21-25`, `:244-245`).
5. Then `handle_irq_generic` calls `IrqDispatcher::handle_irq`, which calls `IrqObject::notify`, which walks the observer list **without a lock** (`interrupt.cpp:116-118`, `irq_dispatcher.cpp:64-73`, `kernel/kernel/irq/irq_object.cpp:40-44`).
6. `handler(virq, nullptr)` runs (`irq_observer.hpp:20-22`). `i8042_interrupt` reads until OBF clears, so the device is quieted inside the handler. The bytes it reads go into `KEY_BUF`, and it logs from IRQ context (`keyboard.cpp:641-662`, `:529-530`).
7. The return value is dropped, and `handle_irq` always returns Handled (`irq_object.cpp:42`, `irq_dispatcher.cpp:72`).
8. The LAPIC EOI is hard-coded, next to a TODO asking for it to go through the chip (`faults.cpp:246-247`).
9. The postamble decides whether to preempt (`faults.cpp:421`, `:431-433`, `mod.cpp:85-93`).

### Defects, most severe first
1. **The irq layer has no lock.**
   - `attach` and `detach` change the list in thread context while `notify` walks the same list in IRQ context (`irq_object.cpp:23-44`).
   - `request_mutex_` is never taken, and as a sleeping lock it could not be taken in IRQ context anyway (`irq_object.hpp:35`).
   - Nothing has gone wrong yet only because both lines attach before they unmask (`time.cpp:53-58`, `keyboard.cpp:699-701`).
2. **IO-APIC register access has no lock.**
   - One `read` or `write` is two separate MMIO accesses, IOREGSEL then IOWIN (`kernel/arch/x86/lib/arch/include/arch/x86/apic/ioapic.hpp:127-135`), and `mask_irq`/`unmask_irq` are a read-modify-write on top (`:145-155`). The callers take no lock either (`io_apic.cpp:116-126`).
   - Once user interrupt objects exist, CPU0 masks one line in IRQ context while CPU1 unmasks another in an ack. Their select and window accesses interleave, and a redirection entry is corrupted.
   - Zircon guards all IO-APIC MMIO with one global spinlock (`zircon/kernel/arch/x86/src/ioapic.rs:95-101`, `:386`).
3. **The IO-APIC lookup is off by one, and a second IO-APIC never matches.**
   - The test is `irqnum < max_local_irqnum()` (`io_apic.cpp:107`), but that value is the highest redirection index (`ioapic.hpp:112-118`).
   - So GSI 23 on a 24-pin IO-APIC resolves to null, then hits a DEBUG_ASSERT or a null dereference (`io_apic.cpp:116-126`). As soon as a user can request that line, the kernel crashes.
   - The correct test is already written: `owns_global_irq` (`ioapic.hpp:121-124`).
4. **Nothing can unregister.**
   - `release_irq` returns `Unimplemented` (`irq_dispatcher.cpp:60-62`), so there is no barrier that waits for a handler in flight either.
   - When `attach` returns AlreadyBound, the freshly allocated observer leaks (`:53-57`).
   - Releasing by line number cannot tell which observer to remove (`mod.hpp:18`).
5. **The context pointer is always null** (`irq_observer.hpp:21`), so handlers fall back on globals (`time.cpp:13`, `keyboard.cpp:442`).
6. **The chip layer is dead.**
   - `IrqObject::init(chip)` has no caller, so the chip starts as 0 (`irq_object.cpp:10`, `:18-21`).
   - Every `IOAPIC_CHIP` operation treats `platform_data` as an `IoApicChipData*` and walks its `pins_` (`io_apic.cpp:30-55`, `:62-70`), but no code allocates one. Bound as it is, the first mask dereferences null.
   - Drivers bypass the generic layer and program the IO-APIC directly: `time.cpp:58`, HPET at `kernel/arch/x86/kernel/time/hpet.cpp:320` and `:328`, and `keyboard.cpp:701`.
   - The trigger mode is set once, at init (`io_apic.cpp:142-155`).
7. **Two number spaces are mixed.** The keyboard registers by GSI (`keyboard.cpp:697-699`) while dispatch delivers the ISA number; `time.cpp:37-48` says so on purpose. It works today only because ISA 1 happens to have no override. The preamble's argument is called `HIrqNum(vector)` (`faults.cpp:415`) but is never used (`mod.cpp:59-65`), so that one is only a naming problem.
8. **Nobody reads `Permanent`.** A grep finds only its definition and the one use at `time.cpp:53`. `attach` also overwrites the flags already stored (`irq_object.cpp:28`).
9. **Return values are dropped.**
   - When the timer fails to register, a trace line is logged and the line is unmasked anyway (`time.cpp:52-58`).
   - `notify` drops each observer's IrqReturn (`irq_object.cpp:42`).
   - `handle_irq` returns Handled even with no observer (`irq_dispatcher.cpp:72`), so spurious interrupts cannot be counted.
10. **Only 16 lines are built, and too early.** The line table is built at `InitLevel::Vmm` by `init_early(16)` (`kernel/kernel/irq/init.cpp:10-14`), while the MADT's IO-APICs are parsed only at `InitLevel::Irq` (`interrupt.cpp:64-71`, `:113`; `kernel/lib/gktl/include/gktl/init_hook.hpp:46-49`).
11. **`KEY_BUF` has no reader.** The IRQ and the one direct call at init both write it without synchronization (`keyboard.cpp:530`, `:703`).
12. **A port cannot be posted from IRQ context.**
    - `m_port_lock` is a Mutex (`kernel/object/include/ours/object/port_dispatcher.hpp:205`).
    - Every `port_queue` packet is allocated (`kernel/object/port_dispatcher.cpp:81`); only SignalOne packets are embedded in their observer (`:22-31`).
    - The invariant "`m_ready` is set exactly when the queue is not empty" is kept only under `m_port_lock` (`:103-105`, `:129-131`, `:144-151`).
    - `sync::Semaphore` is an empty shell: `post` does nothing and `wait` returns Unimplemented (`kernel/kernel/sync/semaphore.cpp:4-8`).
13. **An auto-unsignal event cannot be made.** `Event::Flags::AutoUnsignal` is protected, `m_flags` is private, and the constructor always sets None (`kernel/include/ours/sync/event.hpp:25`, `:51-56`, `:64`); nothing in the tree sets it.

### Outside this design
Two defects found on the way are not on this design's path. They are items 2 and 3 of `docs/repairs/261003/x86-apic.md`:
- `IoApic::issue_eoi` writes the GSI to the EOI register (`ioapic.hpp:137-143`), where Linux writes the vector (`linux/arch/x86/kernel/apic/io_apic.c:264-268`). Item 1 there covers only old IO-APICs that have no EOI register (`docs/repairs/261003/x86-apic.md:3-21`).
- `XAPIC_CHIP`'s mask and unmask always touch LINT0 (`local_apic.cpp:113-116`).

## Goals and non-goals
Goals:
- A user-space driver can own an interrupt line: create an interrupt object on it with an IRQ resource, bind it to a port, wait for packets and ack them. A virtual interrupt works without any hardware.
- A port can be posted from IRQ context without allocating, as Zircon's interrupt packets are.
- IRQ and I/O port resources exist, the root holds them, and the component framework routes them to a driver; a driver process can open I/O ports for itself.
- The first real user is a user-space PS/2 keyboard driver, after which the kernel's keyboard IRQ path goes (D3).
- The lower-layer defects this path exposes are fixed first: the lookup (defect 3), caller-owned observers with a context (4, 5), one number space (7, 10), a per-line lock, Permanent and an unregistration barrier (1, 8, 9), the chip and the IO-APIC lock (2, 6), and the port (12, 13).

Non-goals:
- MSI and PCI (D1), and ACPI AML and `_PRT` (D4). Until a user-space ACPI component exists, only ISA lines can be used.
- A line for every GSI of every IO-APIC, and dynamic vector allocation. Both wait for PCI (D7).
- Strong number types (D9), and exclusive per-driver sub-resources with a range allocator (D5 b, D8).
- Moving the PIT, HPET, COM1, the LAPIC timer or IPIs out of the kernel.
- Shared lines for user interrupts (D2).
- Performance work. The choices that cap it are D6 and D10.
- Renaming `irq::IrqDispatcher`, and deleting the scaffolding in `domain.hpp` and `action.hpp`.
- The two x86-apic defects above, which are recorded as repair items.

## Design
### Objects
- `object::InterruptDispatcher`: the base class. A state machine over WAITING, DESTROYED, TRIGGERED, NEEDACK and IDLE; a spinlock, L2, guarding the state, the timestamp, the bound port and the embedded port packet; an auto-unsignal event for synchronous waits; four pure virtuals, MaskInterrupt, UnmaskInterrupt, DeactivateInterrupt and UnregisterInterruptHandler.
- `object::InterruptEventDispatcher`: a physical line named by its GSI. It owns an `irq::IrqObserver` whose context is the object itself; a level-triggered line gets MASK_POSTWAIT and UNMASK_PREWAIT.
- `object::VirtualInterruptDispatcher`: no line behind it. Its four hardware operations are empty, it is the only kind `interrupt_trigger` accepts, and creating one needs no resource.
- The port's interrupt queue: interrupt packets, embedded in their interrupt objects, queued under a spinlock, L3, without allocating; a packet already queued is not queued twice, and the queue is served before other packets.
- `sync::Semaphore`: a counting semaphore whose post may be called in IRQ context. A port waiter blocks on it.
- `irq::IrqObject`: one per GSI line, with a spinlock L1 taken with IrqSave, its chip, its flags including Permanent, and its observers.
- `irq::IrqObserver`: owned by whoever registers it (a kernel driver's static, or an InterruptEventDispatcher), and carrying the context its handler receives.
- `IOAPIC_CHIP`: resolves the IO-APIC from `data.hirqnum`; all IO-APIC MMIO happens under a spinlock, LI.
- `ResourceDispatcher` of kind Irq or IoPort: a range of lines or ports. The IoPort kind is checked against a deny table of the kernel's own ports.
- The per-process I/O bitmap: the ports a process has opened, kept as ranges and loaded into the TSS bitmap when its threads run.
- The root's built-ins `fuchsia.kernel.IrqResource` and `fuchsia.kernel.IoportResource`: hand a component a duplicate of the root resource, if its manifest's route reaches them.
- The PS/2 driver: a component of the root's manifest that reads the i8042 from user space.

### Events: one level-triggered interrupt reaching a user-space driver
The line is created LEVEL_HIGH, as ACPI creates PCI INTx lines in Fuchsia (`fuchsia/src/devices/board/lib/acpi/pci.cc:293-304`). ours has no such line before PCI (D4), so this is the path the design must get right rather than one QEMU exercises today. Each event names the Zircon code it follows.
1. The driver calls `interrupt_create` with its IRQ resource, a line number and options. The kernel:
   - checks that the resource covers `[irq, irq+1)` (`zircon/kernel/lib/syscalls/driver.cc:201-205`);
   - converts an ISA number to its GSI when REMAP is given, before constructing anything (D7; Zircon remaps after construction, see "Edge lines" below);
   - constructs the object, giving a level line MASK_POSTWAIT and UNMASK_PREWAIT (`zircon/kernel/object/interrupt_event_dispatcher.cc:72-74`, `:89`);
   - sets the trigger mode through `irq::configure` (`:99-104`);
   - attaches its observer to the line with `irq::attach` (`:107`);
   - unmasks the line (`:118`);
   - creates the handle last (`:122`).

   The object starts IDLE.
2. `interrupt_bind` records the port and the key under L2 (`zircon/kernel/object/interrupt_dispatcher.cc:320-321`).
3. The device raises the line. The CPU takes vector 32+GSI, `arch_handle_exception` calls `platform_handle_irq` with the GSI, and the irq layer calls the observer's handler under L1, with the object as context (Zircon calls it under the handler-table entry's lock, `zircon/kernel/platform/pc/interrupt_manager.rs:106-110`). The handler takes L2 with IrqSave (`interrupt_dispatcher.cc:209`).
4. It records a timestamp if none is pending (`:218-225`).
5. The state is not NEEDACK, so it sends a packet (`SendPacketLocked`, `:136-143`, `:239-242`):
   - it queues the embedded packet on the port's interrupt queue under L3, releases L3, then posts the port's semaphore;
   - it masks the line, because of MASK_POSTWAIT, through `irq::mask`, which takes LI;
   - it clears the timestamp;
   - it sets NEEDACK.
6. Only after the handler returns does the platform send the LAPIC EOI (`faults.cpp:246-247`; `zircon/kernel/platform/pc/interrupts.rs:224-230`). The line is masked, so the level that is still high does not fire again at once.
7. The driver's `port_wait` wakes on the semaphore and takes the Interrupt packet ahead of other packets (`zircon/kernel/object/port_dispatcher.cc:376`, `:381-391`). It quiets the device, then calls `interrupt_ack`.
8. In NEEDACK, `ack` unmasks the line under L2 (`interrupt_dispatcher.cc:392-397`). If a timestamp was left meanwhile, it sends another packet at once and the result is Retriggered (`:399-416`); otherwise the object returns to IDLE (`:417-425`). A level still held high fires again right after the unmask (`zircon/vdso/interrupt.fidl:407-409`).
9. When the last handle closes, `on_zero_handles` calls `Destroy` (`interrupt_dispatcher.cc:435`). `Destroy` marks the object DESTROYED, takes a packet still queued off the port, masks the line and detaches the observer; which order those steps take, and which of them run under L2, is the open question at the end. Because it runs on the last close, a crashed driver's line is handed back on its own.

### Edge lines: the PS/2 case
- Fuchsia's pc-ps2 creates its interrupt with `REMAP_IRQ` alone, in the default mode (`fuchsia/src/ui/input/drivers/pc-ps2/device.cc:140-141`, `zircon/kernel/object/interrupt_event_dispatcher.cc:38-45`). ISA 1 is an edge line and gets no MASK_POSTWAIT, so the "line is masked" of event 6 does not hold for it: during NEEDACK, interrupts are only coalesced. The driver reads the device until OBF clears, then acks (`device.cc:361-376`).
- Zircon stores the number **before** remapping when it constructs the object (`interrupt_event_dispatcher.cc:89`, `:133-134`): configure and unmask use the GSI (`:95-100`, `:118`), while registering the handler and Mask/Unmask use the original number (`:107`, `:144-146`). ours remaps before constructing the object and does not copy this inconsistency.

### Why NEEDACK exists
- **The kernel cannot make the device stop.** It does not know the device's registers. The one device-independent thing it can do in hard IRQ context is mask the line, and wait for the driver to say the device is quiet.
- **The EOI cannot wait for user space.** The EOI goes out as soon as the handler returns (`interrupts.rs:224-230`). The reason, which is inferred and not checked against the manual: until the EOI, the LAPIC holds back vectors of the same and lower priority. So the EOI is sent at once, and the mask is what lasts until the ack.
- **Edge lines go through NEEDACK too.** There is one packet, embedded in the object, and the IRQ path never allocates. Interrupts that arrive during NEEDACK only fold into the timestamp (`interrupt_dispatcher.cc:219`, `:236-241`), so the queue is bounded and a storm cannot flood it.
- **Without a bound port**, the next wait is the only way to ack (`zircon/vdso/interrupt.fidl:250-254`). A synchronous wait unsignals the event under the lock (`interrupt_dispatcher.cc:65`, `:89`).

### Linux's counterparts
- **genirq's level flow**: `mask_ack_irq` first, then the handlers, then `cond_unmask_irq` (`linux/kernel/irq/chip.c:689-701`). If a oneshot thread is still running, fasteoi sends only the EOI and leaves the line masked (`:716-722`).
- **Edge lines**: an edge arriving while the line is masked is recorded as `IRQS_PENDING` (`chip.c:831-834`). `disable_irq` is lazy: it leaves the hardware alone until an interrupt actually arrives, then masks and records it as pending (`:381-388`).
- **`IRQF_ONESHOT`**: the line stays disabled until the thread finishes (`linux/include/linux/interrupt.h:52-54`), and the last thread to finish unmasks it (`linux/kernel/irq/manage.c:1125-1129`). A threaded request with no primary handler and no ONESHOT is refused, because a level line would "Rinse and repeat" (`manage.c:1692-1711`).
- **UIO**: the hard handler calls the driver's callback and wakes the reader when it returns Handled (`linux/drivers/uio/uio.c:445-455`). `uio_pdrv_genirq` calls `disable_irq_nosync` at the controller and `enable_irq` when the user writes 1 (`linux/drivers/uio/uio_pdrv_genirq.c:68-71`, `:89-92`). Because it masks the whole line, it refuses `IRQF_SHARED` (`:144-146`).
- **VFIO INTx**: the handler masks first (at the controller, or at the device with PCI 2.3), then signals the eventfd (`linux/drivers/vfio/pci/vfio_pci_intrs.c:228-245`). The uapi calls this AUTOMASKED, "user needs to unmask" (`linux/include/uapi/linux/vfio.h:517-520`). If the interrupt is still pending at unmask, it stays masked and signals again (`vfio_pci_intrs.c:191-197`).
- **The common lesson**: a user-space driver is a oneshot threaded handler across a protection boundary, and every implementation takes the same five steps:
  1. mask a level line in hard IRQ context, and only coalesce an edge line;
  2. send the EOI at once;
  3. notify once, and coalesce what follows;
  4. the ack is the unmask;
  5. at the ack, handle what is pending.

  Zircon's NEEDACK state machine is these five steps written down.

### What stays in the kernel
- The LAPIC timer, the APIC error and spurious vectors, and IPIs are handled directly on fixed vectors (`zircon/kernel/arch/x86/exceptions_c.cc:503-542`; ours already does, `faults.cpp:250-286`).
- The PIT fallback registers a permanent handler (`zircon/kernel/platform/pc/timer.cc:688-692`).
- The debug UART:
  - registers a permanent handler (`zircon/kernel/platform/debug.cc:340-341`), and its ports go on the deny table (`:260-263`);
  - when its buffer is full, clears the RX enable bit in the UART's own IER under the RX lock, a mask at the device (`:280-309`; `zircon/system/ulib/uart/include/lib/uart/ns8250.h:451-455`, `:502-506`).
- Zircon's kernel keeps only the i8042's reboot (`zircon/kernel/platform/pc/keyboard.rs:9`). ours keeps its reboot through port 0x64 in `kernel/platform/pc/power.cpp`.

### Target layering
```text
user     driver component --port_wait-->        component root (holds IRQ/IoPort roots)
syscall  interrupt_create/bind/ack/wait/destroy/trigger, ioports_request/release
object   InterruptDispatcher (state machine, L2 spinlock, embedded packet, auto-unsignal event)
           +- InterruptEventDispatcher -- owns an irq::IrqObserver (ctx = this)
           +- VirtualInterruptDispatcher
           +- (later) MsiInterruptDispatcher
         PortDispatcher: interrupt queue (L3) + counting Semaphore
         ResourceDispatcher: Irq/IoPort ranges + IoPort deny table
irq      IrqObject per GSI: L1 line spinlock, chip, Permanent
         attach(gsi, IrqObserver&)/detach(observer) barrier, mask/unmask/configure
         kernel users: PIT/HPET (Permanent), COM1 RX (Permanent)
chip     IOAPIC_CHIP resolves by data.hirqnum; IO-APIC MMIO under LI spinlock
arch     IDT; LAPIC EOI after handler; LAPIC timer, IPIs, APIC error on fixed vectors (bypass irq)
```

### Lock order
The order is L1 → L2 → {LI, L3}. LI and L3 are never held together.
- **L1**, the `IrqObject`'s line lock, an IrqSave spinlock. It guards the line's observer list and flags. Dispatch holds it while the handlers run, and `detach` takes it too, so `detach` returning means no handler is in flight. A Permanent line dispatches without it (Zircon `interrupt_manager.rs:91-104`).
- **L2**, the `InterruptDispatcher`'s spinlock. It guards the state, the timestamp, the bound port and key, and the embedded packet.
- **LI**, the IO-APIC register lock. It guards the IOREGSEL/IOWIN pair and each read-modify-write of a redirection entry. MaskInterrupt takes it under L2 (Zircon `interrupt_dispatcher.cc:136-139`).
- **L3**, the port's interrupt-queue lock. It guards the queue of embedded packets.
- **Wakeups** (the semaphore's post, or `Event::signal`, which takes the wait queue's lock, then the PI lock, then the run-queue lock, `kernel/kernel/sync/event.cpp:123-124`, `:146-148`) happen after L3 is released but inside L1 and L2. Zircon does the same: it posts after releasing the port's lock (`port_dispatcher.cc:289-293`) while its caller still holds the line's lock and the object's lock.
- The accepted design's total lock order (`docs/design/260923/userspace-interface.md:434`) has none of these IRQ-path locks. It is revised along with the steps that add them (PLAN step 4).
- CLAUDE.md asks the user to write the concurrency argument for this order before it is used. For this MVP run the user skipped it on 2026-10-04; it is the open question at the end.

### How the new objects sit on the irq layer
- **Registration.** `InterruptEventDispatcher` owns an `IrqObserver` (`docs/design/260923/userspace-interface.md:191`) and hangs it on the line with `irq::attach(gsi, observer)`, with itself as context. It unregisters with `irq::detach(observer)`, which must be a barrier.
- **The order of creation.**
  - Remap before constructing.
  - After constructing, attach first, and create the KernelHandle only if that succeeded.
  - If attach returns AlreadyBound, drop the object; it must not go through `Destroy` (Zircon `interrupt_event_dispatcher.cc:84-87`), or it would tear down the interrupt already on that line.
- **The four virtuals.**
  - `MaskInterrupt` and `UnmaskInterrupt` call `irq::mask` and `irq::unmask` on the GSI, which take LI.
  - `UnregisterInterruptHandler` calls `irq::detach`.
  - `DeactivateInterrupt` is empty on x86 (Zircon `:148-153`).
- **The kernel's own lines** register with Permanent, so a user's attach answers AlreadyBound (Zircon `interrupt_manager.rs:294-296`).
- **The ABI already has room.**
  - The comment on the port packet's union reserves the interrupt payload (`system/public/ours/port.hpp:49-52`).
  - The comment on `PortCreateOptions` reserves `BIND_TO_INTERRUPT` (`:79-83`).
  - `HandleType::IrqResource` and `IoportResource` exist (`system/public/ours/process_args.hpp:105-106`).

### Zircon counterparts
#### The layer that stays in the kernel
| ours piece | Zircon counterpart |
|---|---|
| Registration, unregistration, Permanent, mask/unmask/configure in `kernel/kernel/irq` | `zircon/kernel/dev/interrupt/include/dev/interrupt.h`; the x86 implementation is `zircon/kernel/platform/pc/interrupt_manager.rs` |
| The `IrqObject`'s line lock L1 | the handler-table entry in `interrupt_manager.rs` (`:90-138`) |
| ISA to GSI | `zircon/kernel/platform/pc/interrupts.rs` (`remap_interrupt`, `:328-335`) |
| `IOAPIC_CHIP` with the LI lock | `zircon/kernel/arch/x86/src/ioapic.rs` (`:87-103`, `:383-386`) |
| Fixed vectors (timer, IPIs) | `zircon/kernel/arch/x86/exceptions_c.cc`; ours already has `faults.cpp:250-286` |

#### The new pieces
| ours piece | Zircon counterpart |
|---|---|
| `object::InterruptDispatcher` | `zircon/kernel/object/interrupt_dispatcher.cc`, `zircon/kernel/object/include/object/interrupt_dispatcher.h` |
| `object::InterruptEventDispatcher` | `zircon/kernel/object/interrupt_event_dispatcher.cc` |
| `object::VirtualInterruptDispatcher` | `zircon/kernel/object/virtual_interrupt_dispatcher.cc` |
| MSI, later | `zircon/kernel/object/include/object/msi_allocation.h`, `zircon/kernel/object/msi_dispatcher.cc`, `zircon/kernel/object/msi_interrupt_dispatcher.cc` |
| The port's interrupt queue, `sync::Semaphore` | `zircon/kernel/object/port_dispatcher.cc` (`:270-295`, `:376-391`), `zircon/kernel/object/include/object/port_dispatcher.h` (`PortInterruptPacket` at `:155`, `sema_` at `:326`) |
| The Irq and IoPort resource kinds, the deny table | `zircon/kernel/object/include/object/resource.h`, `zircon/kernel/object/resource_dispatcher.rs`, `zircon/kernel/lib/root_resource_filter/src/mod.rs`; the IOPORT allocator is at `zircon/kernel/platform/pc/memory.cc:89`, the IRQ allocator at `zircon/kernel/platform/pc/interrupts.rs:200-205` |
| The per-process I/O bitmap | `zircon/kernel/arch/x86/ioport.cc` |
| The system calls | `zircon/kernel/lib/syscalls/driver.cc` (ioports at `:158-179`, create at `:194-221`) |
| userboot handing out the root resources | `zircon/kernel/lib/userabi/userboot.cc:582-589` |
| The root's IrqResource built-in | `fuchsia/src/sys/component_manager/lib/builtins/src/irq_resource.rs` |
| The public ABI | `ObjectType::Interrupt` is `zircon/system/public/zircon/types.h:465`; `PacketType::Interrupt` and `PortCreateOptions::BindToInterrupt` are `zircon/system/public/zircon/syscalls/port.h:32` and `:38` |

### Gaps and the steps that close them
| Capability | Zircon | ours today | Closed by |
|---|---|---|---|
| Registration and unregistration exclusive with dispatch | a lock per vector; unregistering is a barrier (`interrupt_manager.rs:106-113`, `:125`) | no lock; `release_irq` unimplemented | steps 5 and 6 |
| Handler context, observer ownership | the cookie is `this`; the object owns its registration | context is `nullptr`; `request_irq` allocates the observer itself | step 5 |
| Permanent lines | dispatched without the lock, re-registration refused (`interrupt_manager.rs:91-104`, `:127-128`) | defined, read by nobody | step 6 |
| mask/unmask/configure | a generic interface (`interrupt_manager.rs:200-210`) | drivers call the IO-APIC; the chip is unbound and its data never allocated | step 8 |
| IO-APIC register access | one global spinlock (`ioapic.rs:95-101`) | no lock | step 8 |
| Interrupt numbers | GSI; ISA converted by `remap_interrupt` (`interrupts.rs:328-335`) | dispatch delivers ISA, the keyboard passes GSI | step 7 (D7); strong types are D9 |
| Vectors and line count | a vector allocated on first registration (`interrupt_manager.rs:302-322`), then `configure_irq_vector` (`:333`) | 16 lines, static `32+isa`, built before the MADT is read | step 7 makes it a static `32+GSI`, a deliberate departure (D7); a line per GSI and dynamic allocation wait for PCI |
| EOI timing | LAPIC EOI after the handler returns | the same, but hard-coded (`faults.cpp:246-247`) | kept |
| A user-visible interrupt object | the `InterruptDispatcher` family | none; `ObjectType` has no Interrupt (`system/public/ours/types.hpp:97-109`) | steps 2, 4 and 11 |
| The event for synchronous waits | `AutounsignalEvent` (`interrupt_dispatcher.h:118`) | AutoUnsignal cannot be set | step 2 |
| Posting to a port | embedded packets, a spinlock queue and a counting semaphore | a Mutex; the `m_ready` invariant; Semaphore is a shell | steps 3 and 4 (D6) |
| Masked until the ack | `MASK_POSTWAIT`/`UNMASK_PREWAIT` on level lines | none | step 11 (D2) |
| The line freed when the last handle closes | `on_zero_handles` calls `Destroy` (`interrupt_dispatcher.cc:435`) | none | step 11 |
| Authorization | IRQ resources; userboot hands out the MMIO, IRQ and IOPORT roots (`zircon/kernel/lib/userabi/userboot.cc:582-589`) | `RsrcKind::Irq` is defined (`system/public/ours/syscall/resource.hpp:27-32`), but `create` supports only System (`kernel/object/resource_dispatcher.cpp:35-37`); userboot gives out only VMEX and DEBUG (`kernel/lib/userabi/src/userboot.cpp:413-417`, `:435-441`) | step 9 |
| The kernel's own ports | a deny table for MMIO and IOPORT only (`zircon/kernel/lib/root_resource_filter/src/mod.rs:35-39`, `:130-136`) | none | step 9 (D8) |
| I/O ports | `ioports_request` and a per-process RLE bitmap (`driver.cc:158-179`; `zircon/kernel/arch/x86/ioport.cc:36-64`) | the TSS bitmap closes every port (`kernel/arch/x86/kernel/include/ours/arch/x86/percpu.hpp:86-91`) | step 10 (D10) |
| MSI | `MsiAllocation`, `MsiInterruptDispatcher` | no PCI or MSI | later |
| PCI INTx routing (`_PRT`) | user-space ACPI (`fuchsia/src/devices/board/lib/acpi/pci_irqs.cc:29`, `:206`) | only a table decoding library (`kernel/lib/acpi/README.md:3-13`) | D4 |

### Relation to work in progress
- **The console-input session.** COM1 RX writing into a kernel ring buffer is what Zircon does, and does not conflict with moving drivers out of the kernel:
  - it registers with Permanent, but until step 6 Permanent is silently dropped (`irq_object.cpp:28`);
  - when the buffer is full, it clears the RX bit in COM1's IER, and the reader sets it again under the same lock (`zircon/kernel/platform/debug.cc:280-309`; `zircon/system/ulib/uart/include/lib/uart/interrupt.h:28-30`). The IO-APIC line stays unmasked throughout, or TX interrupts would be blocked as well;
  - the reader replaces today's `pause` busy loop (`kernel/platform/pc/debug_console.cpp:33-40`), and the IRQ can wake it directly with `Event::signal` (`kernel/kernel/sync/event.cpp:106-124`);
  - before step 7, COM1 must register as ISA 4 (`kernel/platform/include/ours/platform/isa.hpp:26`), not by GSI as the keyboard does;
  - steps 5 and 7 change the registration interface and the numbers, so the two sessions agree on which lands first;
  - wiring PS/2 up as console input is exactly what D3 recommends against.
- **The component framework.** The root already plans to offer `DebugResource` as a built-in; `IrqResource` and `IoportResource` use the same mechanism (step 13). Its known gap, "routing is the only gate" (`docs/design/261004/component-framework.md:49`), means any parent can pass the IRQ root on, which is why D5 settles the policy with the mechanism.

## Alternatives considered
- **All three object kinds at once (D1 a).** MSI needs PCI, which ours does not have. The base class keeps Zircon's four pure virtuals (`interrupt_dispatcher.h:60-63`), so adding MSI later does not change it.
- **Masking on every trigger, UIO style (D2 b).** The cost is correctness, not a few more IO-APIC writes: an edge arriving while the line is masked must be recorded as pending in software and replayed at unmask (`linux/kernel/irq/chip.c:381-388`, `:831-834`), and ours has no such mechanism. Without it the edge is lost. For example, while OBF stays set PS/2 raises no new edge, so the keyboard would hang (inferred). Sharing a line among user interrupts was rejected for now too: masking a shared line starves the other devices on it (`uio_pdrv_genirq.c:144-146`).
- **Deleting the kernel's PS/2 now (D3 a), or keeping it for good as a Linux-style exception (D3 c).** (a) removes the keyboard before anything replaces it. (c) would make permanent a path whose buffer has no reader, and wiring it up as console input would be the "replace later" stand-in the rules forbid (`.claude/skills/ours-kernel-design/SKILL.md:26-29`).
- **An AML interpreter in the kernel (D4 b).** That is Linux's approach. Fuchsia keeps AML and `_PRT` in a user-space ACPI component (`fuchsia/src/devices/board/lib/acpi/pci_irqs.cc:29`, `:206`), which is where a microkernel puts it.
- **Exclusive sub-resources per driver (D5 b), or the root creating interrupt objects itself (D5 c).** (b) needs `RsrcFlag::Exclusive` and a range allocator (`system/public/ours/syscall/resource.hpp:34-38`), which buy nothing while there is one driver. (c) puts device knowledge, which line and which trigger mode, into the root.
- **Keeping the Event and checking both queues under both locks (D6 a2), or posting through a Dpc (D6 b).** (a2) keeps the `m_ready` invariant but makes every dequeue take two locks before it unsignals. (b) adds a thread switch to every interrupt, makes `Destroy` cancel the Dpc first, and caps the latency later.
- **Keeping ISA numbers (D7 b).** They cannot name a PCI line. Dynamic vectors now would solve a problem that one 24-pin IO-APIC does not have.
- **Range allocators for every kind now, IRQ lines included (D8 b).** `userspace-interface.md:184` describes them, deducting the kernel's ranges at build time; they come with exclusive sub-resources (D5 b). Permanent already keeps users off the kernel's lines.
- **Distinct enum classes for `Gsi` and `VIrqNum` (D9 b).** The 2026-09-03 change record left them as aliases on purpose, to spare a cast at every call site (`docs/changes/260903/platform.md:38-41`), and after step 7 there is one number space on x86. `Uandle` chose the opposite for type safety (`system/public/ours/types.hpp:59-70`); the question returns with MSI's second number space.
- **Copying the whole 8 KiB bitmap at every context switch (D10 a).** It puts a fixed cost on every switch to a driver thread.
- **Allocating chip data for each line at init (step 8).** It keeps `IoApicChipData`, whose pin list serves a line wired to several pins, which nothing has. Resolving the IO-APIC from `data.hirqnum` is closer to Zircon (`interrupt_manager.rs:200-210`).
- **Linux's UIO or VFIO as the model.** They take the same five steps as Zircon (see "Linux's counterparts"); Zircon's state machine spells them out and is the default spec.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| D1. Copy the whole object family, or a subset | Background: ours has no PCI and no MSI. (a) Event, Virtual and MSI at once; (b) Virtual first, then Event, MSI once PCI exists | (b). The base class keeps the four pure virtuals (`interrupt_dispatcher.h:60-63`), so adding MSI does not change it | Not asked | (b), as recommended (2026-10-04) |
| D2. Edge, level and the meaning of ack | Background: the EOI goes out right after the handler (`faults.cpp:247`), so an unmasked level line fires again at once (`manage.c:1692-1711`). (a) as Zircon: a level line is masked until the ack; an edge line is not masked, only coalesced during NEEDACK; in synchronous mode the next wait is the ack; (b) UIO style: mask on every trigger, which needs software pending bits replayed at unmask (`chip.c:381-388`, `:831-834`) that ours lacks, so edges would be lost. Also: whether a user interrupt's line may be shared | (a), with exclusive lines only: masking a shared line starves the other devices on it (`uio_pdrv_genirq.c:144-146`) | Not asked | (a) with exclusive lines, as recommended (2026-10-04) |
| D3. Retire the kernel's PS/2 | Background: Zircon's kernel keeps only the i8042's reboot (`keyboard.rs:9`); ours's `KEY_BUF` has no reader. (a) remove its IRQ path now; (b) invest nothing more in it, and remove it once step 12 works; (c) keep it for good as a Linux-style exception, recorded in `ours-kernel-design`. Wiring it up as console input would be the "replace later" stand-in the rules forbid (`SKILL.md:26-29`) | (b), and the console-input session does COM1 only | Not asked | (b), as recommended (2026-10-04); the console-input session does COM1 only |
| D4. Where ACPI interrupt routing lives | Background: the kernel decodes only static tables; ISA to GSI needs only the MADT's overrides (`interrupt.cpp:73-77`), but PCI INTx needs `_PRT` and so AML. (a) the kernel keeps the MADT only; AML and `_PRT` go to a later user-space ACPI component (Fuchsia `pci_irqs.cc:29`, `:206`, `pci.cc:293-304`); (b) an AML interpreter in the kernel (Linux) | (a). Until then only ISA lines can be used | Not asked | (a), as recommended (2026-10-04) |
| D5. How the component framework hands IRQ resources to drivers | Background: in Fuchsia the root gets the IRQ root (`fuchsia/src/sys/component_manager/src/builtin_environment.rs:596-597`), offers `IrqResource` by duplicating it (`irq_resource.rs:26-30`), the board driver declares it in its cml (`fuchsia/src/devices/board/drivers/x86/meta/x86.cml:30-37`) and cuts exclusive one-line sub-resources (`pci.cc:293-304`); ours's root already offers `DebugResource` the same way (`component-framework.md:77`), but routing is the only gate (`:49`). (a) as Fuchsia: offer a duplicate of the whole root, routed to one board or driver-host component; (b) the root cuts an exclusive sub-resource per driver from a static configuration, which needs `RsrcFlag::Exclusive` and a range allocator (`resource.hpp:34-38`); (c) the root creates the interrupt objects and hands out only interrupt handles | (a) now, (b) once there is more than one driver | Not asked | (a), as recommended (2026-10-04); (b) once there is more than one driver |
| D6. How a port is posted from IRQ context | Background: the accepted design says interrupt-context producers post only a Dpc (`docs/design/260923/userspace-interface.md:438`), and a Dpc runs on a per-CPU worker thread (`kernel/include/ours/dpc.hpp:30-38`). The wakeup itself works in IRQ context: `Event::signal` disables interrupts and takes the wait queue's lock with IrqSave (`event.cpp:106-124`). The real obstacle is the `m_ready` invariant of defect 12: with a separate spinlock queue, a dequeue that sees the ordinary queue empty under the Mutex unsignals, and a wakeup is lost. (a1) as Zircon: step 3 first, a counting Semaphore, Post and Wait; (a2) keep the Event, but check both queues under both locks before unsignalling; (b) a Dpc: the IRQ only masks, records the timestamp and queues a Dpc, whose thread posts to the Mutex port; a thread switch per interrupt, and `Destroy` must cancel the Dpc first. (b) caps the latency later | (a1). It departs from the accepted design and needs the user's call | Not asked | (a1), as recommended (2026-10-04). This departs from `docs/design/260923/userspace-interface.md:438`, where interrupt-context producers post only a Dpc; PLAN step 4 records the revision there |
| D7. What names an interrupt, and how vectors are set | (a) the GSI, with ISA numbers converted by REMAP (`interrupts.rs:328-335`); (b) keep ISA numbers. Vectors: a static `32+GSI` departs from Zircon's dynamic allocation (`interrupt_manager.rs:302-322`), and is enough on QEMU's one 24-pin IO-APIC | (a) with static vectors, the departure written down in the code; dynamic allocation once PCI exists | Not asked | (a) with static `32+GSI` vectors, as recommended (2026-10-04) |
| D8. How the kernel's own resources are protected | (a) split in two: IRQ lines by Permanent (Zircon's deny table does not cover IRQs, `mod.rs:130-136`); IoPort, and MMIO later, by a deny table as the accepted design (`userspace-interface.md:184`) and Zircon (`debug.cc:260-263`, `mod.rs:117-121`) have it, ready before step 10; (b) the range allocators of `userspace-interface.md:184` for every kind, IRQ lines included | (a) | Not asked | (a), as recommended (2026-10-04) |
| D9. Strong types for the numbers | Background: the 2026-09-03 change record kept them as aliases on purpose, because an enum class costs a cast at every call site (`docs/changes/260903/platform.md:38-41`); `Uandle` chose an enum class for type safety (`types.hpp:59-70`). After step 7, VIrqNum is the GSI on x86. (a) keep the aliases and only unify the names; (b) make `Gsi` and `VIrqNum` distinct enum classes | (a), which is basically usable; revisit when MSI brings a second number space | Not asked | (a), as recommended (2026-10-04) |
| D10. The shape of the per-process I/O bitmap (caps performance) | (a) copy the whole 8 KiB at every context switch; (b) Zircon's RLE ranges, changing only the difference at a switch (`ioport.cc:36-64`). Either way, a change must reach the CPUs running the process's other threads: Zircon uses `mp_sync_exec` (`:180`), ours has `IpiService::sync_exec` (`kernel/include/ours/ipi_service.hpp:59`) | (b): it sets the later performance ceiling | Not asked | (b), as recommended (2026-10-04) |

## Testing and verification
- Every step is one commit, and both images build and boot under `ours-verify` (`.claude/skills/ours-verify/SKILL.md:8-9`).
- Kernel tests are kshell commands in `kernel/commands/*_test.cpp`, since ours has no kernel unit-test runtime (`kernel/products/bin/BUILD.gn:23-24`). They run on the dbg image with `scripts/kshell_run.py`; the rel image's kernel shell is off (`.claude/skills/ours-verify/SKILL.md:33-44`, `.agents/tasks/user-shell/DESIGN.md:18`).
- User-space tests go into the shell's start-up self-checks (`.agents/tasks/shell-tests/PLAN.md:7`), each reporting `shell: <name> checks passed` or FAILED, which boot_check fails on.
- What each layer is checked by:
  - the virtual path (steps 2 and 4): shell checks on both images, covering trigger and wait, coalescing before the ack, Retriggered after it, and nothing left on the port after destroy;
  - the irq layer (step 6): a new `irq.test` kshell command;
  - the resources and I/O ports (steps 9 and 10): shell checks on dev.dbg_1, whose shell userboot starts directly (`userboot.next=bin/shell`) and so receives the root resources;
  - a physical line's lifecycle (step 11): shell checks on dev.dbg_1 on an idle ISA line;
  - delivery through a real line: the PS/2 driver on dev (step 12), with a key sent through QEMU's monitor.
- What cannot be verified yet:
  - the level-triggered path: before PCI there is no checked level-triggered source on QEMU. It rests on the virtual tests and on argument;
  - lock order: lockdep cannot be switched on in this kernel yet (`docs/repairs/260917/kernel-task-sched.md` item 6), and the concurrency argument was skipped for this run (Open questions);
  - real delivery before step 12: ISA 0, 1 and 4 belong to the kernel, and HPET's legacy route takes the RTC's ISA 8 (`hpet.cpp:319-328`; Linux says it "eats up the RTC interrupt line", `linux/arch/x86/kernel/hpet.c:1183`).

## Open questions
- **The concurrency argument for L1 → L2 → {LI, L3}.** CLAUDE.md asks the user to write it before a lock decision is used. The user skipped it for the MVP on 2026-10-04, so it stays open as their exercise; the AI grades it once written. Its pieces:
  - the locks are those of "Lock order" above;
  - the packet is embedded in the interrupt object. Once queued, the port holds a pointer into the object's memory, and `Destroy` and `Unbind` call the port's `RemoveInterruptPacket` to take back a packet still queued (`zircon/kernel/object/interrupt_dispatcher.cc:284-285`, `:348`; `zircon/kernel/object/port_dispatcher.cc:270-277`);
  - the actors: CPU0 runs the IRQ path; T1 is the driver thread, doing wait and ack; T2 closes the last handle and so runs `Destroy`, which calls `detach`;
  - interleaving A: T2 enters `Destroy`, takes L2, sets DESTROYED and is about to call `detach`, which takes L1 and so waits for any dispatch in flight. Meanwhile the device fires on CPU0, which already holds L1 and is about to enter `InterruptHandler`, which takes L2;
  - interleaving B: T1, in a synchronous wait, sees NEEDACK with no interrupt pending, so it unmasks and is about to set WAITING and block. Between the unmask and setting the state, CPU1 takes the interrupt;
  - the questions: (1) which invariant each of L1, L2, LI and L3 protects, and who touches each in which context; (2) in A, what each CPU waits for next; how `Destroy`'s steps should be ordered and which of them run outside L2; and, with that order, a proof that once `detach` returns no handler touches the object again and no packet pointing into it stays on a port's queue; (3) in B, whether the interrupt is lost, delivered twice or delivered correctly, and which check under which lock decides it; (4) a one-line prediction: under D6 (b), a Dpc, which extra race needs arguing in A and which in B.
- Whether the LAPIC holds back vectors of the same and lower priority until the EOI, the reason the EOI cannot wait for user space ("Why NEEDACK exists"). It is inferred, not checked against the Intel SDM.
- Whether a PS/2 controller with OBF held set raises no new edge, the reason D2 (b) would hang the keyboard. It is inferred, not checked.
