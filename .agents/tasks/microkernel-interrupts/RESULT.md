# microkernel-interrupts: interrupt objects on virtual and physical lines, and IRQ and I/O port resources routed to components; the user-space PS/2 driver is not done

**Date**: 2026-10-05

## Delivered
Steps 1 to 11 and 13 of PLAN.md are on baseline/20260824, landed on 2026-10-05 up to `c1581fcc`. Step 12, the user-space PS/2 driver, is not (Not done). What works now:
- A program creates a virtual interrupt, triggers it, and waits on it or binds it to a port and acks it; triggers that come before the ack fold into one packet, and an ack with one pending sends it at once.
- A program holding the IRQ root creates an interrupt object on a free ISA line, with RemapIrq, and the line goes back when the object's last handle closes. An edge line is never masked; a level line is masked from delivery to ack, which no test sees yet (Verification).
- A program holding the I/O port root opens and closes I/O ports for itself, `ioports_request` and `ioports_release`, except the kernel's own.
- The kernel mints both roots, userboot hands them on, and on dev the root keeps them and serves `ours.resource.Irq` and `ours.resource.Ioport`, which root.cm offers to routing_checks alone.

The kernel still drives the PS/2 keyboard, so ISA 1 and the i8042's ports 0x60 and 0x64 are still the kernel's, and the deny table still refuses those ports.

Hashes are those on baseline/20260824. mvp/integration holds the same commits under other hashes, one for one by subject. The steps landed in the order 1, 5, 7, 2, 3, 4, 6, 8, 9, 10 and 11, 13.

| Step | Commits |
|---|---|
| plan | `49eafe49` docs(irq): design and plan for interrupts in user space |
| 1 | `d568dcb8` fix(x86-apic): resolve the last pin of an IO-APIC to it; `475576ca` fix(x86-apic): give the boot check's true ordering reason; `7a1c30e7` docs(x86-apic): repair item for an override naming an unowned GSI |
| 2 | `95a79b81` feat(object): interrupt objects, virtual first; `616213fd` fix(sync): name Zircon's AUTOUNSIGNAL flag as its checkout spells it; `d1640951` docs(object): repair item for two waiters parked on one interrupt |
| 3 | `3d7f8d4d` refactor(object): ports wait on a counting semaphore |
| 4 | `c51e4813` feat(object): bind interrupts to ports; `8e9164ee` fix(object): a bind takes the wait's latched signal with the interrupt; `2c83ef25` test(object): the shell checks a trigger folding into a packet acked unread |
| 5 | `8e2cb8c3` refactor(irq): callers own their observers and get a context; `14b15653` fix(irq): drop the last CLOCK_EVENT reference, record the HPET's LEG_RT gap |
| 6 | `8a8fb2c9` feat(irq): a spinlock per line, Permanent lines and a detach barrier; `6deec23b` fix(irq): record a real interrupt under L1, and the thread-context gap |
| 7 | `5b30995c` refactor(irq): name interrupt lines by GSI |
| 8 | `84a39af1` refactor(irq): drive the IO-APIC through its chip, under one lock; `010f6e85` docs(x86-apic): say which IO-APIC calls still go around the line; `a84ba62a` docs(irq): record the window between configure and attach |
| 9 | `3fb22dac` feat(object): IRQ and I/O port resources, handed to the root; `b95e2608` fix(object): the dev shell checks that the root keeps both resource roots; `83535f10` refactor(object): a resource's range is a u64, not a PhysAddr |
| 10 | `62173901` feat(x86): ioports_request with a per-process I/O bitmap; `5fbe44a5` docs(x86): the I/O bitmap's lifetime comment names what keeps it alive; `e9ea90a9` docs(ipi): a sync_exec task may take a lock held only with interrupts off; `98c7ec83` docs(x86): ioports_release says when it answers OutOfMem; `b48fba6b` docs(x86): the plan's files list names every file step 10 changed; `47d6f0bd` docs(x86): the switch's repair item names the lock every CPU shares; `e84e04fd` fix(x86): ioport.test leaks its address space when the observer is stuck |
| 11 | `40c2f02b` feat(object): interrupt objects for physical lines; `d574b6f7` test(object): a physical interrupt_create that cannot write its handle gives the line back; `312a1012` fix(object): two interrupt comments say what the code does; `30e3b49f` style(irq): irq::trigger_mode() spells its result as the layer's headers do |
| 12 | Not done |
| 13 | `3ed40db6` feat(init): ours.resource.Irq and ours.resource.Ioport built-ins; `eb48d365` test(ours-resource): ProtocolsRefuseEachOther pairs protocols by name; `c1581fcc` docs(init): step 13's record is exact about two of the reference's choices |

The rename of the protocol libraries, the comment rule and its sweep, and the prof session's commits that share the branch are listed in `.agents/tasks/component-framework/RESULT.md`.

## Verification
- `boot_check.py --out out/results` from a fresh out dir on mvp/integration at `44924c04`: build ok (31.2 s); dbg PASS (34 passed, 2 skipped, 0 failed, 0 error lines); rel PASS (30 passed, 10 skipped, 0 failed, 3/3 required, 0 error lines). The same on baseline/20260824 at `2f14dc0c`, also from a fresh out dir: build ok (31.1 s), dbg 34 passed and 2 skipped, rel 30 passed and 10 skipped with 3/3 required.
- dev.dbg_1, whose shell holds both roots: `shell: ioports_request(the root, RTC 0x70, 2) = Ok`, `shell: RTC through 0x70/0x71: register D 0x80, register B 0x2, seconds 0x46`, `shell: probe gp_io_rtc killed after exception 0x8, while 0x70 is open in this shell`, the refusals (no handle BadHandle, past 0xFFFF OutOfRange, COM1 AccessDenied, the IRQ root BadType) and `shell: ioport checks passed`; `shell: interrupt_create(ISA 0, the timer's) = AlreadyBound`, `shell: interrupt_create(ISA 3) = Ok`, `shell: interrupt_create(ISA 3, taken) = AlreadyBound`, `shell: interrupt_create(ISA 3, after the close) = Ok` and `shell: interrupt checks passed`; `shell: interrupt port checks passed`; `shell: resource checks passed`.
- dev: `init: holding the IRQ and I/O port resources`, a line boot_check requires; `routing_checks: passed`, which now covers `ours.resource/Irq.Get` with an interrupt on ISA 3 and `ours.resource/Ioport.Get` with port 0x70; `shell: interrupt checks on physical lines skipped (the root keeps the IRQ root)`, `shell: interrupt checks passed`, `shell: interrupt port checks passed`, `shell: ioport checks passed (the root keeps the I/O port root)` and `shell: resource checks passed (the root kept both roots)`.
- Kernel tests on dev.dbg_1, `scripts/kshell_run.py --out out/results` on mvp/integration at `44924c04`: `semaphore.test: ok -- 18 checks`; `port.test: ok`; `irq.test: ok -- 33 checks`, with 1000 attach and detach rounds on line 3 and 300 rounds of dispatch against detach, 1073 dispatches handled and 163598 finding the line empty; `interrupt.test: ok -- 22 checks`; `interruptline.test: ok -- 37 checks`, 200 rounds, 200 objects reached by 44413 interrupts raised by IPI; `resource.test: ok -- 32 checks`; `ioport.test: ok -- 14 checks`.
- Host: `ours_resource` 6 and `route` 32 passed; `python3 system/lib/wire/test/ordinals_test.py`: OK, 5 tests, among them the selectors `ours.resource/Irq.Get` and `ours.resource/Ioport.Get`.
- The kernel's keyboard through the reworked irq layer: steps 5, 7 and 8 each typed `sendkey a`, `sendkey shift-b` and `sendkey c` into QEMU's monitor by script on dev.dbg_1 and saw `char 0x61 (a)`, `char 0x42 (B)` and `char 0x63 (c)`. Not repeated here.
- Each step's mutation checks are in its change record: for step 10, a switch that never closes ports, a change that sends no reload, a stale switch closing only the current runs, and a request that skips its resource check, each caught; for step 11, `destroy()` masking and detaching under L2, which deadlocks `interruptline.test`; for step 13, root.cm's offer removed and the two roots swapped.
- Not tested: a level line's mask, since `interruptline.test` raises its interrupts by IPI, around the IO-APIC, and q35 has no free level source before PCI (`docs/repairs/261005/object.md` item 2); a device raising a line into a user driver, which was step 12's to show; the order L1 → L2 → {LI, L3} under lockdep, which cannot be switched on yet (`docs/repairs/260917/kernel-task-sched.md` item 6); one process using its ports from threads on several CPUs at once.

## Deviations from the design
- **Every decision, D1 to D10, was taken as recommended**, with no prediction asked, and **the concurrency argument for L1 → L2 → {LI, L3} was skipped**, both by the user's instruction for this run. It stays DESIGN.md's open question.
- **The built-ins take the names of the user's rename of 2026-10-04**: the plan's `ours.kernel.IrqResource` and `ours.kernel.IoportResource` in `system/lib/ours-kernel` are `ours.resource.Irq` and `ours.resource.Ioport` in `system/lib/ours.resource`.
- Step 1: an ISA override whose ISA number is out of range is refused and the boot goes on, as Linux does where Zircon asserts; `apic_configure_isa_irq()` also asserts its argument, which the plan did not list. An override whose GSI no IO-APIC owns still stops the boot (`docs/repairs/261004/x86-apic.md` item 1).
- Step 2: a program cannot start a second thread, so the shell checks what one thread sees, and the parked paths, two threads in turn and destroy waking a waiter, are `interrupt.test`'s, on kernel threads (`docs/repairs/261004/object.md` item 2). The pending time is an Option, so a trigger at time 0 counts, where Zircon's 0 means "none pending".
- Step 4: `bind()` clears the wait's latched signal when it hands a pending interrupt to the port (`8e9164ee`), where Zircon's `Bind()` leaves it set.
- Step 6: L1 has no lockdep class of its own, though the plan asked for one: `lockdep::Guard` files every spin lock under one class, the per-declaration class is not implemented, and `system/lib/lockdep` is an uninitialised submodule (`docs/repairs/261004/kernel-irq.md` item 2).
- Step 9, two departures written into PLAN.md:
  - The i8042's 0x60 and 0x64 went on the IoPort deny table, where the plan had left them open for step 12. At this step `kernel/platform/pc/keyboard.cpp` still sets the controller up and reads every scancode from its interrupt, so a second reader would steal scancodes. Step 12 takes both ports off the table in the commit that deletes that driver.
  - The table is the platform's, `kernel/platform/pc/ioport.cpp` behind `<ours/platform/ioport.hpp>`, with empty ones for arm and riscv, not a list beside `resource_dispatcher.cpp`: which ports the kernel drives is a fact of the PC.

  Beyond the plan: `object_get_info`'s Resource topic (18), `resource.test`, boot_check requiring the root's line, the dev shell checking that it holds neither root, and a resource's range typed `u64`. Against Zircon's `validate_ranged_resource()`, the deny table is checked on every IoPort resource, not only on a root, since there is no `resource_create`, and a root grants its kind's whole space and answers OutOfRange past it.
- Step 10, three departures from the reference mechanism, Zircon's `IoBitmap::SetIoBitmap()`:
  - the reload goes only to the CPUs running the address space, not to every CPU (D10);
  - the change does not hold off preemption: the mask of running CPUs, read after the edit, and the version cover a caller switched away midway;
  - a version per bitmap makes a switch that lands between a release and its reload close every port. Zircon's switch closes the outgoing ports by the current runs, so it appears to leave a just-released port open for whatever runs next on that CPU; that is inferred from its code, not run.

  The plan's verification was revised: the shell's probe can run on any CPU, so it shows only that a process that did not ask for a port lacks it, and `ioport.test` checks the switch, the reload of another CPU and the version's window from the kernel.
- Step 11, two revisions written into PLAN.md:
  - REMAP goes through a platform seam, `platform::remap_isa_irq()` in `<ours/platform/isa.hpp>`: `apic_io_isa_to_global()` behind a bound check on the PC, the number unchanged on arm and riscv, because `kernel/object` is the same on every architecture.
  - A create in the default mode reads the line's trigger mode back from the IO-APIC through a new `irq::trigger_mode()` and chip operation, so a line the firmware made level, as q35 makes ISA 5, 9, 10 and 11, gets MaskPostwait without the driver naming the mode.

  The remap happens before the object exists, so every operation names the line by one number, where Zircon keeps the number from before the remap. Delivery is shown by `interruptline.test`, which raises ISA 3's vector by IPI, since the shell can neither park a waiter nor raise an interrupt.
- Step 13: one `ResourceServer` class, made once per protocol, and one set of codec functions that take the protocol, replacing Debug's four; the built-ins are one list on every architecture, where component_manager registers each only when it holds the resource, and the I/O port one on x86 alone; and no kind check per connection, so a resource of the wrong kind shows as BadType at the client's first use.

## Found along the way
Open, Medium, most important first:
- `docs/repairs/261005/object.md` item 1: two creates on one free line can each set or read its trigger mode before either attaches, so the winner may run with the other's mode, an edge object on a level entry storming until the ack. A locking decision left to the user, with three options; (a), the mode and the attach under one L1 hold, is recommended. `docs/repairs/261004/kernel-irq.md` item 4 records the same race from the irq layer's side and still says Low, "once step 11 lands", which it has: the two should become one item.
- `docs/repairs/261004/object.md` item 1: two threads can both park on one interrupt, and destroy then wakes only one.
- `docs/repairs/261004/x86-clock.md` item 1: the HPET's channel 0 becomes the tick device even on an HPET that cannot take IRQ0 over, where the boot would hang; QEMU's HPET can.
- `docs/repairs/261004/kernel-irq.md` item 1: when two ISA lines land on one GSI, the lower ISA number keeps it.
- `docs/repairs/261003/x86-apic.md` items 2 and 3, which DESIGN.md put outside itself: `IoApic::issue_eoi` writes the GSI where the vector belongs, and `XAPIC_CHIP`'s mask and unmask always touch LINT0. Neither is reached today.

Open, Low:
- `docs/repairs/261004/kernel-irq.md` item 2, no lockdep class for L1 and LI; item 3, a dispatch from thread context holds L1 with preemption enabled.
- `docs/repairs/261005/object.md` item 2, no test sees a level line's mask; `docs/repairs/261004/object.md` items 2 and 3, the shell cannot check a parked wait, and a cross-CPU handoff through a virtual interrupt takes about 4 ms on dbg.
- `docs/repairs/261004/x86-apic.md` item 1, an override naming an unowned GSI stops the boot; `docs/repairs/261004/x86-clock.md` item 2.
- Costs to measure before changing anything: `docs/repairs/261004/kernel-sync.md` item 1, the semaphore takes its wait queue's lock when nobody blocks; `docs/repairs/261005/x86-kernel.md` item 1, every address space switch takes both I/O bitmaps' locks.

Review notes not recorded in `docs/repairs/`: an ISA override past GSI 207, the end of D7's static vectors, panics the boot (step 7); the HPET's legacy route works only because q35's MADT moves ISA 0 to GSI 2 (step 7); `bind()` trusts its caller to have checked that the port takes interrupts, where a DEBUG_ASSERT would catch a later kernel caller (step 4); the startup-handle arrays of both userboots and init's `RESOURCE_SERVERS` are counted by hand, with nothing asserting the bound (steps 9 and 13).

While step 13 was integrated, dbg failed twice in five boots on `shell: profile checks FAILED` with 0 user samples, the slow CPU of `docs/repairs/261005/kernel-top.md` item 1. The prof session's `43adb3de` reworked that check afterwards, and every boot for this result passed.

## Not done
- **Step 12**, the user-space PS/2 driver: `system/driver/pc-ps2` as an eager child of dev's root.cm, deleting `kernel/platform/pc/keyboard.cpp`, taking 0x60 and 0x64 off the deny table, and boot_check requiring `pc-ps2: ready`. It was started twice on 2026-10-05 and stopped both times before writing anything, once at the session limit and once on a dropped API connection while it was still reading; its branch `irq/s12-ps2` holds no work. Until it lands, D3 (b) is not reached: the kernel keeps its keyboard driver and ISA 1, and no device raises a line into a user driver.
- The concurrency argument for L1 → L2 → {LI, L3}, the user's exercise (DESIGN.md, Open questions).
- Two inferences DESIGN.md rests on, unchecked: that the local APIC holds back vectors of the same and lower priority until the EOI, and that a PS/2 controller with OBF held raises no new edge.
- The decisions recorded for the user in repair items: `docs/repairs/261005/object.md` item 1, `docs/repairs/261004/object.md` item 1, and `docs/repairs/261004/kernel-irq.md` items 1, 3 and 4.
- Out of scope by PLAN.md: MSI and PCI, ACPI AML, a line per GSI with dynamic vectors, strong number types (D9), exclusive sub-resources (D5 b), and performance work.
