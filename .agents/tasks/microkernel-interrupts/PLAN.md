# microkernel-interrupts: the steps from a kernel-only irq layer to a user-space PS/2 driver

**Date**: 2026-10-04
**Status**: Proposed

## Request
On 2026-10-04 the user, away until about 20:00 PDT, asked for an MVP built by following the recommended steps of two plans: the component framework (`docs/design/261004/component-framework.md`, `.agents/tasks/component-framework/PLAN.md`) and this one, user-space interrupts (DESIGN.md, written in this run from a proposal checked against the sources).

Every decision in DESIGN.md is taken as recommended. By the user's instruction for this run, the concurrency argument CLAUDE.md asks for before a lock decision is skipped; it stays open in DESIGN.md's Open questions.

The scope ends at "basically usable":
- a virtual interrupt works end to end, through a port;
- IRQ and I/O port resources exist, the root holds them and routes them to a driver;
- a user-space PS/2 driver, a component of the root's manifest, takes ISA 1 through an interrupt object and logs the keys it decodes, and the kernel no longer registers the keyboard IRQ.

Out of scope: MSI and PCI, ACPI AML, a line per GSI with dynamic vectors, strong number types, exclusive sub-resources, performance work, and the two x86-apic defects DESIGN.md puts outside this design (`docs/repairs/261003/x86-apic.md` items 2 and 3).

## Decisions
None. The plan follows DESIGN.md's decisions D1-D10, all taken as recommended on 2026-10-04.

## Steps
The steps keep DESIGN.md's numbers, the proposal's, and are listed in the order they land in this run: 1, 5, 7; 2 and 3, then 4; 6, then 8; then, after the component framework's step 16, 9; 10 and 11; 13; 12.

Every step:
- lands as one commit, made with `git commit -- <paths>`;
- builds both images and boots dev and dev.dbg_1 under `python3 .agents/skills/ours-verify/scripts/boot_check.py`;
- runs `cpp-write-and-verify` on the C++ it touches;
- writes its change record in `docs/changes/<YYMMDD>/<module>.md` in the same commit, with the module the step names;
- puts kernel tests in `kernel/commands/*_test.cpp`, run on dev.dbg_1 with `python3 scripts/kshell_run.py --out <dir> <name>.test`, and user-space tests in the shell's start-up self-checks.

No step touches `kernel/arch/x86/kernel/faults.cpp` or `kernel/arch/x86/lib/arch/include/arch/x86/interrupt.hpp` while another session has uncommitted work there. None needs to: the vector range stays `PlatformIrqMin..PlatformIrqMax`, faults.cpp keeps handing the irq layer `vector - 32` (`faults.cpp:21-25`), which step 7 makes the GSI by programming the vectors as `32+GSI`, and the LAPIC EOI stays hard-coded with its TODO (`faults.cpp:246-247`). A step that finds it needs either file stops and is reported blocked.

### 1. Fix the IO-APIC lookup
- What:
  - `resolve_irqnum_to_ioapic` tests `owns_global_irq` (`ioapic.hpp:121-124`) instead of `irqnum < max_local_irqnum()` (`kernel/arch/x86/apic/io_apic.cpp:107`).
  - `init_io_apic`'s override table logs each override's own entry rather than `IOAPIC_ISA_OVERRIDES[i]` (`io_apic.cpp:213-216`), and refuses an override whose ISA number is not below `NUM_ISA_IRQS` before indexing with it (`:211`).
  - A boot-time DEBUG_ASSERT that each IO-APIC's last GSI resolves to that IO-APIC.
- Why now: it depends on nothing, and every later step that names a line by GSI goes through this lookup (7, 8 and 11). Today GSI 23 on a 24-pin IO-APIC resolves to null, so the first user request for it would crash the kernel (defect 3).
- Verified: both images boot, dev.dbg_1 with the new assertion; the override table in the log shows q35's ISA 0 → GSI 2 override on its own row.
- Module: `x86-apic`.
- Commit: `fix(x86-apic): resolve the last pin of an IO-APIC to it`.

### 5. Callers own their observers
- What:
  - `irq::attach(VIrqNum, IrqObserver&)` and `irq::detach(IrqObserver&)` replace `request_irq` and `release_irq` (`kernel/include/ours/irq/mod.hpp:17-18`).
  - The handler receives the observer's context instead of `nullptr` (`irq_observer.hpp:20-22`).
  - The PIT and the keyboard keep their observer as a static and pass their state as the context instead of reading globals (`time.cpp:13`, `keyboard.cpp:442`).
  - The layer no longer allocates the observer, so the leak on AlreadyBound goes with it (`irq_dispatcher.cpp:53-57`).
  - `detach` removes one observer; step 6 makes it a barrier.
- Why now: step 11's object owns its observer and needs its context, and the interface is cheapest to change while it has two callers. It lands before step 7, which renumbers the same call sites.
- Verified: both images boot, which needs the PIT's tick. A key sent through QEMU's monitor still reaches the kernel's keyboard handler: with `-serial mon:stdio` (`scripts/qemu_boot.py:231`), Ctrl-A c, `sendkey a`, Ctrl-A c, and the handler's trace line `char 0x61 (a)` appears (`keyboard.cpp:529`). This is checked by hand and noted in the change record.
- Coordination: if the console-input session has registered COM1 by then, its registration moves to the new interface in this step.
- Module: `kernel-irq`.
- Commit: `refactor(irq): callers own their observers and get a context`.

### 7. One number space: the GSI
- What:
  - `platform_init_apic` programs each ISA line at its GSI with vector `32+GSI` (`kernel/platform/pc/interrupt.cpp:93-110`), so faults.cpp's unchanged `vector - 32` hands the irq layer the GSI. A comment there records the static vector as a departure from Zircon's dynamic allocation (`zircon/kernel/platform/pc/interrupt_manager.rs:302-322`), D7.
  - The PIT registers by GSI, and `time.cpp:37-48`'s comment on the two numbers goes.
  - The platform builds the line table after `platform_init_apic` and hands it to the irq layer, instead of `init_early(16)` at `InitLevel::Vmm` (`kernel/kernel/irq/init.cpp:10-14`). It covers the GSIs the ISA lines map to; a line for every GSI waits for PCI. Nothing calls into the irq layer before the Irq level today (`time.cpp:60`, `keyboard.cpp:709`).
- Why now: right after step 5, on the same call sites. Steps 6, 8 and 11 key lines by GSI, and REMAP (step 11) converts into this space.
- Verified: both images boot with q35's ISA 0 → GSI 2 override, so the tick arrives on vector 34 and is dispatched as GSI 2; the keyboard is checked by hand as in step 5.
- Coordination: before this step COM1 registers as ISA 4 (`kernel/platform/include/ours/platform/isa.hpp:26`), after it by GSI. If the console-input session has registered it, the registration moves here.
- Module: `kernel-irq`.
- Commit: `refactor(irq): name interrupt lines by GSI`.

### 2. The interrupt object, virtual first
- What:
  - `object::InterruptDispatcher`, with the state machine, the L2 spinlock and the four pure virtuals (Zircon `interrupt_dispatcher.h:21-27`, `:60-63`), and `object::VirtualInterruptDispatcher`, whose hardware operations are empty (`virtual_interrupt_dispatcher.cc:78-81`).
  - `ObjectType::Interrupt` (`system/public/ours/types.hpp:97-109`; Zircon `types.h:465`), the interrupt options in a public header, and the system calls `interrupt_create` (VIRTUAL only for now), `interrupt_trigger`, `interrupt_wait` and `interrupt_destroy` in `system/public/ours/syscalls.gni`. A virtual interrupt needs no resource (`driver.cc:199-206`).
  - The synchronous wait as Zircon's, which has both an auto-unsignal event (`interrupt_dispatcher.h:118`) and an explicit unsignal under the lock when a wait consumes a trigger without blocking (`interrupt_dispatcher.cc:65`, `:89`). `sync::Event` gains a way to be constructed with AutoUnsignal, which today cannot be set (defect 13; `kernel/include/ours/sync/event.hpp:25`, `:51-56`, `:64`).
- Why now: the user-facing interface comes first (`.claude/skills/ours-kernel-design/SKILL.md:26-29`). A virtual interrupt needs nothing below it, so the object, its handle type and its calls work end to end before any hardware path.
- Verified: new shell checks on both images, `shell: interrupt checks passed`: two threads trigger and wait in turn, a wait returns the trigger's timestamp, and destroy wakes a waiter with Canceled.
- Module: `object`.
- Commit: `feat(object): interrupt objects, virtual first`.

### 3. A counting semaphore under the port (D6)
- What:
  - `sync::Semaphore` gets a count: `post` may be called in IRQ context, and `wait` blocks, up to a deadline, while the count is zero (`kernel/kernel/sync/semaphore.cpp:4-8` is a stub today).
  - The port moves to Zircon's model: queueing a packet posts the semaphore after the queue's lock is released, and a dequeue waits on the semaphore before it takes the queue (`zircon/kernel/object/port_dispatcher.cc:279-295`, `:376`). This replaces `m_ready`, whose invariant is kept only under the Mutex (`kernel/object/port_dispatcher.cpp:103-105`, `:129-131`, `:144-151`).
- Why now: step 4's interrupt queue is posted from IRQ context, which `m_ready` under a Mutex cannot take (defect 12, D6 a1).
- Verified: `port.test` on dev.dbg_1 and the shell's port and wait_async checks on both images pass unchanged. A new `semaphore.test` kshell command posts N times and waits N times without blocking, then shows the next wait blocking until another thread posts, and a wait with a deadline timing out.
- Module: `object`.
- Commit: `refactor(object): ports wait on a counting semaphore`.

### 4. Interrupt packets on ports: bind and ack
- What:
  - `PortCreateOptions::BindToInterrupt` (`system/public/ours/port.hpp:79-83`), `PacketType::Interrupt` and the interrupt payload in the packet's union (`:49-52`; Zircon `syscalls/port.h:32`, `:38`).
  - The port's interrupt queue: packets embedded in the interrupt object, queued under the spinlock L3 without allocating; a packet already queued is not queued twice; the queue is served before other packets; `RemoveInterruptPacket` serves `Destroy` and unbinding (`port_dispatcher.cc:270-295`, `:381-391`).
  - `interrupt_bind` and `interrupt_ack`, with Retriggered (`interrupt_dispatcher.cc:320-321`, `:392-425`).
  - `docs/design/260923/userspace-interface.md` §4.8 gains an English "Revised 2026-10-04" note: hard rule 3 (`:438`) gets the interrupt object's exception (D6), and the total order (`:434`) gets L1 → L2 → {LI, L3}. The `ours-kernel-design` skill's `references/task.md` records the same exception with its reason.
- Why now: it finishes the virtual path to a port, so everything above the hardware is tested before step 11.
- Verified: shell checks on both images, with a virtual interrupt: triggers before an ack give one packet; a trigger during NEEDACK gives a Retriggered packet at the ack; after destroy no packet stays queued; a port created without BindToInterrupt refuses the bind.
- Module: `object`.
- Commit: `feat(object): bind interrupts to ports`.

### 6. A spinlock per line, and Permanent
- What:
  - An IrqSave spinlock, L1, replaces the unused Mutex (`irq_object.hpp:35`) and guards attach, detach and notify. It gets a lockdep class.
  - A Permanent line dispatches without L1 and refuses any further attach; `attach` stops overwriting the stored flags (`irq_object.cpp:28`).
  - `detach` takes L1, so it returns only once no handler is in flight: the barrier.
  - `notify` combines the observers' IrqReturn, and a line nobody handled counts as spurious (`irq_object.cpp:42`, `irq_dispatcher.cpp:72`).
- Why now: before step 8 adds masking from IRQ context and step 11 attaches and detaches in thread context while lines are live. Until then the only thing keeping the list safe is that both lines attach before they unmask (defect 1).
- Verified: a new `kernel/commands/irq_test.cpp`, `irq.test` on dev.dbg_1: an observer attaches to and detaches from an idle line many times, and a second attach to the PIT's line answers AlreadyBound. lockdep cannot check the order until it can be switched on (`docs/repairs/260917/kernel-task-sched.md` item 6).
- Module: `kernel-irq`.
- Commit: `feat(irq): a spinlock per line, Permanent lines and a detach barrier`.

### 8. Bind the chip
- What:
  - `IoApicChipData` goes, and `IOAPIC_CHIP`'s operations resolve the IO-APIC from `data.hirqnum` (`io_apic.cpp:30-70`), which is closer to Zircon (`interrupt_manager.rs:200-210`).
  - The platform calls `IrqObject::init(chip)` on each line when it hands the table over (step 7); today it has no caller (`irq_object.cpp:18-21`).
  - An IrqSave spinlock, LI, in `io_apic.cpp` covers every IOREGSEL/IOWIN pair and every read-modify-write (`ioapic.hpp:127-135`, `:145-155`; `io_apic.cpp:116-126`), as Zircon's one lock does (`zircon/kernel/arch/x86/src/ioapic.rs:95-101`, `:386`).
  - `irq::mask`, `irq::unmask` and `irq::configure` go through the chip, and the PIT (`time.cpp:58`), HPET (`kernel/arch/x86/kernel/time/hpet.cpp:320`, `:328`) and the keyboard (`keyboard.cpp:701`) use them.
  - The chip's `issue_eoi` stays unused: the LAPIC EOI stays in faults.cpp, and the IO-APIC EOI's own defect is `docs/repairs/261003/x86-apic.md` item 2.
- Why now: step 11 masks a line from IRQ context on one CPU while another CPU unmasks in an ack (defect 2), and its Mask and Unmask need `irq::mask` and `irq::unmask`.
- Verified: both images boot; `grep -rn 'apic_\(un\)\?mask_irq' kernel/platform kernel/arch/x86/kernel/time` finds nothing; the keyboard is checked by hand as in step 5.
- Module: `kernel-irq`.
- Commit: `refactor(irq): drive the IO-APIC through its chip, under one lock`.

### 9. IRQ and I/O port resources, handed to the root (D8)
- What:
  - An IoPort deny table of the kernel's own ports: COM1 0x3F8-0x3FF, the PIT 0x40-0x43, the PICs 0x20-0x21 and 0xA0-0xA1 (`docs/design/260923/userspace-interface.md:184`), and the reset control port 0xCF9 (`kernel/platform/pc/power.cpp:17`), as Zircon's (`zircon/kernel/lib/root_resource_filter/src/mod.rs:117-121`, `zircon/kernel/platform/debug.cc:260-263`). The i8042's 0x60 and 0x64 stay open for step 12; the kernel writes 0x64 only to reboot (`power.cpp:24`). **Revised 2026-10-05:** at this step `kernel/platform/pc/keyboard.cpp` still sets the i8042 up and reads every scancode from its interrupt, so 0x60 and 0x64 are on the table until step 12 deletes that driver. The table is the platform's, `kernel/platform/pc/ioport.cpp` behind `<ours/platform/ioport.hpp>`, since which ports the kernel drives is a fact of the platform; `resource_dispatcher.cpp` reads it.
  - `ResourceDispatcher::create` accepts the Irq and IoPort range kinds (`kernel/object/resource_dispatcher.cpp:35-37`). `validate_ranged_resource` refuses a range that touches the deny table with AccessDenied, as Zircon does for a root resource (`zircon/kernel/object/resource.rs:97-111`).
  - The kernel's userboot mints an IRQ root and an IoPort root (`kernel/lib/userabi/src/userboot.cpp:413-417`, `:435-441`; Zircon `userboot.cc:582-589`). User userboot's fixed `infos[6]` is full (`kernel/lib/userabi/userboot/userboot.cpp:305-330`); it grows and forwards both, as `HandleType::IrqResource` and `IoportResource` (`system/public/ours/process_args.hpp:105-106`), and the root takes them.
- Why now: after the component framework's step 16. Its steps 11 and 13 to 16 rewrite userboot's handoff and init's startup; landing this after them avoids carrying it through that rewrite, and the root it hands the resources to is the one step 13 extends.
- Verified: on dev the root logs that it received both resources. On dev.dbg_1, where userboot starts the shell directly (`userboot.next=bin/shell`), a new shell check finds both handles with the right kinds, `shell: resource checks passed`. OutOfRange cannot be tested yet, since no system call goes through `validate_ranged_resource` (the names in `system/public/ours/syscalls.gni:56-1095`). **Revised 2026-10-05:** boot_check requires the root's line on dev; the shell reads the kinds through a new `object_get_info` topic, Resource (18), and on dev checks that it holds neither root, since the root keeps them; and `resource.test` on dev.dbg_1 checks `validate_ranged_resource()`'s answers for both kinds, OutOfRange and AccessDenied included, from the kernel shell.
- Module: `object`.
- Commit: `feat(object): IRQ and I/O port resources, handed to the root`.

### 10. ioports_request with a per-process I/O bitmap (D10)
- What:
  - `ioports_request` and `ioports_release`, checked by `validate_ranged_resource` with the IoPort kind (Zircon `zircon/kernel/lib/syscalls/driver.cc:158-179`).
  - A per-process I/O bitmap kept as RLE ranges (Zircon `zircon/kernel/arch/x86/ioport.cc:36-64`). At a switch to another process's thread, only the ports that differ change in the TSS bitmap, which closes every port today (`kernel/arch/x86/kernel/include/ours/arch/x86/percpu.hpp:86-91`).
  - A change reaches the CPUs running the process's other threads through `IpiService::sync_exec` (`kernel/include/ours/ipi_service.hpp:59`; Zircon's `mp_sync_exec`, `ioport.cc:180`).
- Why now: step 9 gives a process an IoPort resource, and step 12's driver needs ports 0x60 and 0x64.
- Verified: new dev.dbg_1 shell checks, `shell: ioport checks passed`: a child process that touches 0x70 without a request takes a #GP and is killed; after a request, the shell reads the RTC through 0x70 and 0x71; a range beyond the resource answers OutOfRange; a request for 0x3F8 answers AccessDenied. **Revised 2026-10-05:** the child may run on any CPU, so the shell shows only that a process that did not ask for a port lacks it; `ioport.test` on dev.dbg_1 checks the TSS the CPUs consult: the runs against a model, the switch closing a process's ports, the reload of another CPU running the address space, and a switch made while a release's reload is held off, which a per-bitmap version makes close every port. On dev the shell checks that a request with no handle answers BadHandle.
- Module: `x86-kernel`.
- Commit: `feat(x86): ioports_request with a per-process I/O bitmap`.

### 11. Interrupt objects for physical lines
- What:
  - `object::InterruptEventDispatcher` and `interrupt_create`'s physical form: an Irq resource covering `[irq, irq+1)` (`driver.cc:199-206`; `zircon/kernel/object/include/object/resource.h:64-66`); REMAP converting an ISA number with `apic_io_isa_to_global` before the object is constructed; then configure, attach, unmask, and the handle last. AlreadyBound drops the object without `Destroy` (`interrupt_event_dispatcher.cc:84-87`).
  - A level line gets MASK_POSTWAIT and UNMASK_PREWAIT, and every user line is exclusive (D2).
  - The four virtuals over `irq::mask`, `irq::unmask` and `irq::detach`, with `DeactivateInterrupt` empty (Zircon `:148-153`).
  - `on_zero_handles` calls `Destroy`, which detaches.
- Why now: every layer under it was fixed in steps 1 and 5 to 8, and step 9 made the resource.
- Verified: new dev.dbg_1 shell checks, part of `shell: interrupt checks passed`, since the shell holds the IRQ root there:
  - creating on the PIT's line answers AlreadyBound (Permanent);
  - creating on ISA 3 with REMAP works (QEMU has no COM2 with one `-serial`, `scripts/qemu_boot.py:231`), and a second create on it answers AlreadyBound;
  - after the handle closes, creating on ISA 3 works again;
  - destroy wakes a waiter with Canceled;
  - a resource of another kind is refused.

  Real delivery waits for step 12 on ISA 1: before it, ISA 0, 1 and 4 are the kernel's, and HPET's legacy route takes the RTC's ISA 8 (`hpet.cpp:319-328`; `linux/arch/x86/kernel/hpet.c:1183`). The level path has no checked source on QEMU before PCI; it rests on the virtual tests and on argument.
- Module: `object`.
- Commit: `feat(object): interrupt objects for physical lines`.

### 13. The root offers ours.resource.Irq and ours.resource.Ioport (D5)
- What:
  - The root serves `ours.resource.Irq` and `ours.resource.Ioport` as built-ins next to `ours.resource.Debug` (`docs/design/261004/component-framework.md:77`). Each Get answers a duplicate of the root resource (`fuchsia/src/sys/component_manager/lib/builtins/src/irq_resource.rs:26-30`).
  - `system/lib/ours.resource` gains the two Get messages, each ordinal declared beside its selector, so `system/lib/wire/test/ordinals_test.py` checks them.
  - Routing is the only gate (`component-framework.md:49`): a component holds them only if root.cm offers them to it.
  - root.cm offers both to routing_checks, which gets each through /svc, creates an interrupt on ISA 3 with REMAP and closes it, and requests and releases port 0x70.
- Why now: it needs the component framework's /svc and built-ins (its step 14) and step 11, and step 12's driver gets its resources this way.
- Verified: `ours-verify`, with dev's `routing_checks: passed` now covering the two resources; the ordinals test prints OK.
- Module: `init`.
- Commit: `feat(init): ours.resource.Irq and ours.resource.Ioport built-ins`.

### 12. A user-space PS/2 driver, and the kernel's keyboard IRQ goes (D3)
- What:
  - `system/driver/pc-ps2`, an eager child `pc-ps2` of dev's root.cm, which uses `ours.resource.Irq` and `ours.resource.Ioport` offered from the root's built-ins (step 13).
  - It requests ports 0x64 and 0x60 (Fuchsia `fuchsia/src/ui/input/drivers/pc-ps2/controller.cc:57-64`), resets the controller as Fuchsia's does (`controller.cc:70-109`), creates its interrupt on ISA 1 with REMAP (`device.cc:140-141`), an edge line with no MASK_POSTWAIT, and binds it to a port.
  - On each packet it reads the data port while OBF is set, then acks (`device.cc:361-376`), decodes the scancodes with the table moved from the kernel, and logs each key, as `pc-ps2: key a`.
  - At start it checks the line's lifecycle: a second create on ISA 1 answers AlreadyBound, and after it closes its handle, create works again, which is what hands the line back when a driver dies. Then it logs `pc-ps2: ready`.
  - The kernel stops registering the keyboard IRQ (D3): `kernel/platform/pc/keyboard.cpp` goes, with its `KEY_BUF`, its handler and its controller setup, since two owners of one controller would race. The reboot through 0x64 stays in `kernel/platform/pc/power.cpp`, as Zircon's kernel keeps only the i8042's reboot (`zircon/kernel/platform/pc/keyboard.rs:9`). With the driver, 0x60 and 0x64 come off the IoPort deny table in `kernel/platform/pc/ioport.cpp`, where step 9 put them while the kernel drove the i8042 (revised 2026-10-05).
  - `boot_check.py` requires `pc-ps2: ready` on dev, through the required lines the component framework's step 13 adds.
- Why now: it is last, as it needs I/O ports (10), physical interrupts (11) and the routed resources (13).
- Verified: `ours-verify`. On dev, a key sent through QEMU's monitor (Ctrl-A c, `sendkey a`, Ctrl-A c) logs `pc-ps2: key a`; this is checked by script or by hand and recorded in RESULT.md. dev.dbg_1 is unchanged, apart from the kernel no longer logging keys.
- Module: `pc-ps2`.
- Commit: `feat(pc-ps2): a user-space PS/2 keyboard driver`.

## Files
Created:
- `kernel/object/include/ours/object/interrupt_dispatcher.hpp`, `kernel/object/interrupt_dispatcher.cpp`, and the virtual kind's header and source (step 2).
- `kernel/lib/syscall/src/interrupt.cpp`: the interrupt system calls (steps 2, 4 and 11).
- A public header for the interrupt options under `system/public/ours/` (step 2).
- `kernel/commands/semaphore_test.cpp` (step 3), `kernel/commands/irq_test.cpp` (step 6), `kernel/commands/resource_test.cpp` (step 9), `kernel/commands/ioport_test.cpp` (step 10).
- The IoPort deny table, `kernel/platform/pc/ioport.cpp` behind `kernel/platform/include/ours/platform/ioport.hpp`, with an empty one for arm and riscv (step 9; revised 2026-10-05 from beside `kernel/object/resource_dispatcher.cpp`).
- The per-process I/O bitmap under `kernel/arch/x86/kernel/`, and `kernel/lib/syscall/src/ioport.cpp` (step 10).
- `kernel/object/include/ours/object/interrupt_event_dispatcher.hpp`, `kernel/object/interrupt_event_dispatcher.cpp` (step 11).
- `system/driver/pc-ps2/`: the driver and its manifest (step 12).
- `docs/changes/<YYMMDD>/<module>.md`: one record per step, modules `x86-apic`, `kernel-irq`, `object`, `x86-kernel`, `init` and `pc-ps2`.

Changed:
- `kernel/arch/x86/apic/io_apic.cpp` (steps 1 and 8).
- `kernel/include/ours/irq/mod.hpp`, `irq_object.hpp`, `irq_observer.hpp`, `irq_chip.hpp`; `kernel/kernel/irq/irq_dispatcher.cpp`, `irq_object.cpp`, `init.cpp` (steps 5 to 8).
- `kernel/platform/pc/interrupt.cpp` (steps 7 and 8).
- `kernel/arch/x86/kernel/time/time.cpp` (steps 5, 7 and 8), `kernel/arch/x86/kernel/time/hpet.cpp` (step 8).
- `kernel/platform/pc/keyboard.cpp` (steps 5, 7 and 8; deleted in step 12), `kernel/platform/pc/BUILD.gn` (step 12).
- `kernel/include/ours/sync/event.hpp`, `kernel/kernel/sync/event.cpp` (step 2); `kernel/include/ours/sync/semaphore.hpp`, `kernel/kernel/sync/semaphore.cpp` (step 3).
- `kernel/object/include/ours/object/port_dispatcher.hpp`, `kernel/object/port_dispatcher.cpp` (steps 3 and 4).
- `kernel/object/resource_dispatcher.cpp`, `kernel/object/include/ours/object/resource_dispatcher.hpp` (step 9).
- `system/public/ours/types.hpp` (step 2), `system/public/ours/port.hpp` (step 4), `system/public/ours/syscalls.gni` (steps 2, 4, 10 and 11).
- `kernel/lib/userabi/src/userboot.cpp`, `kernel/lib/userabi/src/resource.hpp`, `kernel/lib/userabi/src/resource.cpp`, `kernel/lib/userabi/userboot/userboot.cpp`, `system/service/init/` (step 9).
- `system/public/ours/object_info.hpp`, `kernel/lib/syscall/src/object.cpp`: the Resource info topic (step 9).
- `kernel/arch/x86/kernel/include/ours/arch/x86/percpu.hpp` and the context-switch code that loads it (step 10).
- `examples/shell/shell_tests.cpp`, `examples/shell/shell_tests.hpp` (steps 2, 4, 9, 10 and 11).
- `docs/design/260923/userspace-interface.md`, `.agents/skills/ours-kernel-design/references/task.md` (step 4).
- `system/lib/ours.resource/`, `system/service/init/` and its `routing_checks` test component (step 13).
- `kernel/products/image/dev/BUILD.gn`: root.cm gains `pc-ps2` (step 12).
- `.agents/skills/ours-verify/scripts/boot_check.py`: dev requires the root's line that it holds the IRQ and I/O port resources (step 9), and `pc-ps2: ready` (step 12).

Deleted:
- `kernel/platform/pc/keyboard.cpp` (step 12).

## Test plan
- **Boot:** `python3 .agents/skills/ours-verify/scripts/boot_check.py` prints PASS for both images after every step.
- **kshell commands on dev.dbg_1** (`scripts/kshell_run.py`), each ending with its verdict:

```text
semaphore.test: ok      (step 3)
port.test: ok           (steps 3 and 4, unchanged)
irq.test: ok            (step 6)
resource.test: ok       (step 9)
ioport.test: ok         (step 10)
```

- **Shell self-checks** (exact wording fixed in the steps):

```text
shell: interrupt checks passed   (both images from step 2; step 11 adds the physical checks on dev.dbg_1)
shell: port checks passed        (unchanged through steps 3 and 4)
shell: resource checks passed    (both images from step 9; on dev, that the root kept both roots)
shell: ioport checks passed      (both images from step 10; on dev, only the refusal of no handle)
```

- **dev's root and components:** from step 9 the root logs that it received the IRQ and I/O port resources, which boot_check requires; from step 13 `routing_checks: passed` covers them; from step 12 boot_check requires `pc-ps2: ready`.
- **By hand, through QEMU's monitor** (Ctrl-A c, `sendkey a`, Ctrl-A c): the kernel's trace line `char 0x61 (a)` after steps 5, 7 and 8, noted in their change records; `pc-ps2: key a` on dev after step 12, recorded in RESULT.md.
- **Not tested:** a level-triggered line, for which QEMU has no checked source before PCI; the lock order, until lockdep can be switched on (`docs/repairs/260917/kernel-task-sched.md` item 6).
