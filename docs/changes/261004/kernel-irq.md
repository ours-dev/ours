# kernel-irq changes

## Title: Callers own their interrupt observers, and a handler gets its context (2026-10-04)

**Files**
- `kernel/include/ours/irq/irq_observer.hpp` — the observer is built by its owner from a handler, a context, flags and a name; it records the line it hangs on; it can be neither copied nor moved.
- `kernel/include/ours/irq/types.hpp` — `IrqHandler`'s parameters are named, and the second one is the context.
- `kernel/include/ours/irq/mod.hpp`, `kernel/kernel/irq/mod.cpp` — `irq::attach(VIrqNum, IrqObserver &)` and `irq::detach(IrqObserver &)` replace `request_irq` and `release_irq`.
- `kernel/include/ours/irq/irq_dispatcher.hpp`, `kernel/kernel/irq/irq_dispatcher.cpp` — the same at the dispatcher, which no longer allocates anything.
- `kernel/kernel/irq/irq_object.cpp` — `attach` refuses a null handler and an observer already on a line, and records the line; `detach` checks that the observer is on this line.
- `kernel/arch/x86/kernel/time/time.cpp` — the device the legacy timer line drives lives in a `LegacyTimer` object, handed to the handler as its context; the line's observer is a static; the global `CLOCK_EVENT` goes.
- `kernel/arch/x86/kernel/include/ours/arch/time.hpp` — `extern CLOCK_EVENT` gives way to `global_clock_event()` and `set_global_clock_event()`.
- `kernel/arch/x86/kernel/time/hpet.cpp` — the hand-over to the HPET calls `set_global_clock_event()`.
- `kernel/platform/pc/keyboard.cpp` — the decoder's shift keys, last scancode and character buffer move into a `KeyboardState` handed to the handler as its context; the line's observer is a static.
- `kernel/kernel/time/tick_device.cpp` — a comment that named `CLOCK_EVENT` as an example of an UPPER_CASE object names `LEGACY_TIMER`, its replacement.
- `docs/repairs/261004/x86-clock.md` — two items on the HPET's hand-over of the legacy timer line, both older than this change.

**Symptom**
Latent, cannot be triggered today. Three defects of the interface, numbered 4 and 5 in `.agents/tasks/microkernel-interrupts/DESIGN.md`:
- Every `request_irq` that the line refused with AlreadyBound leaked the observer it had just allocated. Nothing registers twice on one line today, so nothing has leaked yet; the user-space `interrupt_create` of PLAN step 11 would have leaked one observer per refused attempt, a leak any process holding an IRQ resource could drive.
- `release_irq(virq)` answered Unimplemented, so nothing could ever come off a line.
- A handler's context argument was always `nullptr`, so both handlers read globals: the PIT's `CLOCK_EVENT` and the keyboard's `KEY_BUF`, `key_lshift`, `key_rshift` and `LAST_CODE`.

**Cause**
The question underneath all three is who owns the registration record, the observer that sits in the line's list.

`request_irq` answered "the irq layer": it allocated the `IrqObserver` with `new (mm::GAF_KERNEL)` and handed it to `IrqObject::attach`. When `attach` answered AlreadyBound, `request_irq` returned that status and dropped the only pointer to the allocation. The list did not hold it, and the caller never knew it existed. Linux's `request_threaded_irq()` also allocates its `struct irqaction`, but it frees it on every failure path: `kfree(action)` right after `__setup_irq()` fails. Ours was one `kfree` short. That `kfree` is not the whole fix, though. A layer that allocates the record cannot know how long the caller needs it to live, and in the design that comes next, the interrupt object of PLAN step 11 needs exactly that: its registration must end when the object ends, and not a moment later.

Releasing by line number had the same root. A line can hold several observers when it is shared, and the number cannot say which of them to take off. Linux's `free_irq(irq, dev_id)` needs the second argument for exactly this: `__free_irq()` walks the line's actions looking for the one whose `dev_id` matches. Naming the observer itself is the direct form of that search.

The missing context was what forced globals on the two handlers. A handler with no argument of its own can find its state only through something every handler can reach: a global, or a table indexed by line number. For the kernel's two drivers a global was tolerable. For an interrupt object, which must find *itself* from inside the handler, it is not; Zircon passes the object as the cookie, `interrupt_handler_t{this, ...}` in `InterruptEventDispatcher::RegisterInterruptHandler()`.

Making the caller the owner answers all three at once:
- The observer's lifetime is its owner's. A kernel driver keeps its observer in a static, alive as long as the kernel; an interrupt object will embed its observer, so the observer dies exactly when the object does, after the object has detached it.
- Nothing is allocated, so `attach` cannot fail for lack of memory and has nothing to free when it fails for any other reason.
- `detach` is given the observer, which knows its line, so on a shared line it removes exactly one observer.
- The observer carries the context its owner chose, and the line hands it to the handler on every interrupt.

Ownership by the caller brings one new way to go wrong: the owner might destroy an observer that is still linked into a line's list, which leaves the line walking freed memory at the next interrupt. The observer's list hook is ustl's default SafeLink kind, whose destructor executes `__builtin_trap()` if it is still linked, so that mistake stops the kernel at the destruction rather than corrupting it later. Copying and moving are deleted for the same reason: a copy of a linked observer would be a second, unlinked object with the same handler and context.

The observer records which line it hangs on (`m_virqnum`), so `detach` needs nothing else. `IrqObject` is its friend, and only `IrqObject::attach` and `detach` write that field, in the same breath as they link and unlink the hook, so the two cannot disagree.

**Change**
The dispatcher, before and after:

```cpp
// before
auto observer = new (mm::GAF_KERNEL) IrqObserver(irqnum, flags, handler, name);
if (!observer) {
    return Status::OutOfMem;
}
return object->attach(*observer);   // AlreadyBound: `observer` leaks

// after
return object->attach(observer);    // the caller's observer; nothing to free
```

The PIT, before and after:

```cpp
// before
ClockEventDevice *CLOCK_EVENT;
auto timer_interrupt(VIrqNum irqnum, void *) -> irq::IrqReturn {
    CLOCK_EVENT->handle_event();
    ...
}
auto status = irq::request_irq(ISA_IRQ_PIT, timer_interrupt, irq::IrqFlags::Permanent, "IRQ-Timer");

// after
constinit LegacyTimer LEGACY_TIMER;   // holds std::atomic<ClockEventDevice *>
auto timer_interrupt(VIrqNum, void *context) -> irq::IrqReturn {
    auto *const device = static_cast<LegacyTimer const *>(context)->device();
    device->handle_event();
    ...
}
constinit irq::IrqObserver LEGACY_TIMER_OBSERVER{&timer_interrupt, &LEGACY_TIMER,
                                                 irq::IrqFlags::Permanent, "IRQ-Timer"};
auto const status = irq::attach(ISA_IRQ_PIT, LEGACY_TIMER_OBSERVER);
```

The keyboard follows the same shape: `KeyboardState KEYBOARD` holds the shift keys, the last scancode and the buffer; `KEYBOARD_OBSERVER` carries `&KEYBOARD`; `i8042_interrupt` casts its context back and passes the state to `i8042_process_scode`. The two shift `bool`s became a `ShiftKeys` flags word, as the style rules ask of a set of yes/no facts about one object; both bits are written only by the decoder.

The trade-offs:
- **What the PIT's context is.** The device the legacy line drives changes once, when the HPET takes IRQ0 over through its legacy replacement route. So the context cannot be the PIT's device itself: re-pointing it would mean detaching and attaching again, which a Permanent line will refuse once PLAN step 6 enforces Permanent, as Zircon's `InterruptTableEntry::set_handler()` refuses any change to a permanent entry. The context is a small object holding the current device, and the HPET's hand-over goes through `set_global_clock_event()`, Linux's name for the same pointer.
- **Why that pointer became atomic.** `CLOCK_EVENT` was a plain pointer written in thread context and read by the interrupt handler, a data race in C++'s terms. The new object stores it with release and loads it with acquire, so a handler that sees the HPET's device also sees the writes that enrolled it. On x86 both compile to a plain `mov`.
- **The observer beside its state, or inside it.** The plan keeps each driver's observer as a static whose context is the driver's state, and that is what was done. Embedding the observer in the state object, context `this`, is the shape PLAN step 11's interrupt object takes.
- **Rejected: keep `request_irq` and add a context parameter.** It would still allocate, so it would need a `kfree` on every failure path and a way to find the allocation again at release, which is the bookkeeping the caller-owned form removes.
- **An invalid line number is no longer a panic.** `irq::request_irq` began with `ASSERT(virqnum != INVALID_V_IRQ_NUM)`; `irq::attach` answers InvArgs, as the dispatcher already did. Step 11 passes numbers that came from user space, and a caller's bad argument must not stop the kernel.
- **The new errors.** `attach` answers BadState for an observer already on a line, since its one hook cannot be in two lists, and InvArgs for a null handler, the check `request_irq` made. `detach` answers NotFound for an observer on no line or on another one. Zircon's unregister of an empty entry answers OK and Linux's `free_irq()` WARNs; a status the caller must look at sits between the two.

**Reference**
- Zircon `interrupt_handler_t {cookie, fn}` (`zircon/kernel/dev/interrupt/include/dev/interrupt.h:24-31`); `InterruptEventDispatcher::RegisterInterruptHandler()`, which passes `this` as the cookie, and `UnregisterInterruptHandler()` (`zircon/kernel/object/interrupt_event_dispatcher.cc:155-165`); `InterruptTableEntry::set_handler()`, which refuses to change a permanent entry (`zircon/kernel/platform/pc/interrupt_manager.rs:120-139`). Zircon's own PIT passes a null cookie and reads globals (`register_permanent_int_handler(irq, interrupt_handler_t{nullptr, &pit_timer_tick})`, `zircon/kernel/platform/pc/timer.cc:688-690`); ours passes its state, as the plan asks.
- Linux `request_threaded_irq()`, which allocates the `irqaction` and frees it when `__setup_irq()` fails (`kernel/irq/manage.c:2123-2190`); `free_irq(irq, dev_id)` and `__free_irq()`, which find the action by `dev_id` (`:2006`, `:1859`); `timer_interrupt()` and `global_clock_event` (`arch/x86/kernel/time.c:37-41`); `hpet_legacy_clockevent_register()`, which points `global_clock_event` at the HPET (`arch/x86/kernel/hpet.c:467`).

**Verification**
- Build of `out/bringup.x86`: ok.
- `boot_check.py`: dbg PASS (shell finished at 28.4 s, 24 passed, 1 skipped, 0 failed, 0 error lines); rel PASS (3.5 s, 22 passed, 3 skipped, 0 failed, 0 error lines). Reaching the shell needs the timer tick, first the PIT's through the new context and then the HPET's after `set_global_clock_event()`.
- The keyboard, by script rather than by hand: a script in the run's scratchpad, built on `scripts/kshell_run.py`'s `Console`, booted dev.dbg_1 and, after `--- end ---`, typed Ctrl-A c, `sendkey a`, `sendkey shift-b`, `sendkey c`, Ctrl-A c into QEMU's multiplexed monitor. The serial log shows `char 0x61 (a)`, `char 0x42 (B)` and `char 0x63 (c)`: the handler reached its state through the context, and the shift bit was set and cleared again.
- clang-format: clean on the changed lines. clang-tidy: nothing new on the changed lines. The four UPPER_CASE namespace-scope objects carry `NOLINTNEXTLINE(readability-identifier-naming)` with the reason, as `NOOP_DEVICE` in `kernel/kernel/time/tick_device.cpp` does.
- Not tested: `detach`, which nothing calls until PLAN step 6 makes it a barrier and adds `irq.test`. The irq layer has no host test target.

**Deliberately not done**
- `detach` is not a barrier: the line has no lock yet, so a handler in flight on another CPU may outlive the call. PLAN step 6 adds the lock; until then nothing calls `detach`.
- `Permanent` is still read by nobody, and `attach` still overwrites the line's stored flags (PLAN step 6).
- The PIT still registers by ISA number and the keyboard by GSI (PLAN step 7), and the PIT still unmasks its line after a failed attach (defect 9, PLAN steps 6 and 8).
- The keyboard's init still drains the controller by calling `i8042_interrupt(0, &KEYBOARD)` in thread context after unmasking the line, so an interrupt in the middle of it would run the decoder on the same state re-entrantly. That predates this change, and the file goes in PLAN step 12.
- `IrqObject`'s members keep their trailing-underscore names; PLAN step 6 rewrites the class.
- The HPET's hand-over is published after the HPET is already driving the line; recorded as `docs/repairs/261004/x86-clock.md` item 2.
- `hpet_init()` enrolls channel 0 and hands it the line even when the HPET has no legacy replacement route, which `set_global_clock_event()`'s comment assumes it has; found in review and recorded as `docs/repairs/261004/x86-clock.md` item 1. Both predate this change, which only renamed the assignment `time::CLOCK_EVENT = &device`.

**How it was found**
None. PLAN step 5 of `.agents/tasks/microkernel-interrupts/`, from defects 4 and 5 of its design.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does an irq layer that allocates its own observers leak on AlreadyBound, and why is adding the missing `kfree` not enough once interrupt objects can register?
- Why can the PIT's context not simply be the PIT's `ClockEventDevice`, and what would re-pointing the line by detach and attach run into once Permanent is enforced?
- An interrupt object that embeds its observer is destroyed without detaching it: what happens with a SafeLink hook, and what would have happened with a plain one?

## Title: Interrupt lines are named by GSI, and built once the MADT has been read (2026-10-04)

**Files**
- `kernel/platform/pc/interrupt.cpp` — `platform_init_apic()` programs each ISA line at its GSI with vector `PlatformIrqMin + GSI`, skips a line whose GSI an earlier one took, and builds the irq layer's lines from the GSIs it programmed, before interrupts are enabled; the comment on its init hook says that every driver that attaches to a line runs after it, where it used to claim the opposite; `platform_handle_irq()` says that the number it passes on is the GSI.
- `kernel/include/ours/irq/mod.hpp`, `kernel/kernel/irq/mod.cpp` — `irq::init_lines(Span<HIrqNum const>)` replaces `irq::init_early(usize)`.
- `kernel/include/ours/irq/irq_dispatcher.hpp`, `kernel/kernel/irq/irq_dispatcher.cpp` — the dispatcher builds one line per number it is given, at that number, and leaves a null slot for a number in between that names no line. It refuses a second build, an empty list, an invalid or repeated number, and keeps nothing from a build that fails.
- `kernel/kernel/irq/init.cpp` — the `InitLevel::Vmm` hook that built 16 lines goes.
- `kernel/arch/x86/kernel/time/time.cpp` — the PIT attaches and unmasks by the same GSI; the comment explaining its two numbers goes.
- `kernel/platform/include/ours/platform/isa.hpp` — the comment says that past the platform only the GSI names a line.
- `docs/repairs/261004/kernel-irq.md` — item 1: which ISA line keeps a GSI that two of them reach.

**Symptom**
Latent, cannot be triggered today. Two defects, 7 and 10 in `.agents/tasks/microkernel-interrupts/DESIGN.md`:
- One interrupt had two numbers. On QEMU's q35 the PIT is ISA 0, which the MADT sends to GSI 2. Its IO-APIC entry was GSI 2, but the vector programmed there was 32 + 0, so dispatch delivered 0: the PIT attached to line 0 and unmasked GSI 2. The keyboard attached and unmasked by GSI 1, which worked only because ISA 1 has no override. A driver for an overridden line written the keyboard's way would never have been called; one written the PIT's way had to know which call takes which number.
- The line table was 16 lines made by `init_early(16)` at `InitLevel::Vmm`, before the MADT is read at `InitLevel::Irq`. A GSI of 16 or more had no line, and the PIT's line 0 existed only because every number from 0 to 15 was made, whether or not anything was wired there.

**Cause**
An external interrupt carries three numbers on its way in, and the kernel sees only the last:
- the ISA number, the input of the legacy 8259 PIC that the device was designed for, which ACPI's interrupt source overrides are written in;
- the GSI, the global system interrupt: an IO-APIC pin, numbered across all IO-APICs. An override says "ISA 0 is wired to GSI 2"; a line with no override sits on the pin of its own number. The IO-APIC's redirection table is indexed by GSI, so masking, unmasking and configuring a line all take the GSI;
- the vector, the IDT slot the CPU enters. It is whatever the redirection entry for that GSI was programmed with, and it is the only thing the CPU tells the kernel.

So the number the kernel can recover from an interrupt is whatever the vector was computed from. `platform_init_apic()` computed it from the ISA number, `PlatformIrqMin + isa`, and `faults.cpp` subtracts `PlatformIrqMin` again, so dispatch recovered the ISA number, while every IO-APIC operation took the GSI. Each driver had to hold both numbers and know which call wanted which. The PIT got it right, and `time.cpp` carried a twelve-line comment explaining that this was deliberate after a review had reported it as a bug. The keyboard used the GSI for both, and was right only by coincidence. A layer whose callers need a comment to use a line number correctly has the wrong interface: the defect was the mapping, not any one caller.

The one number has to be the GSI, not the ISA number. Every line the kernel can ever see has a GSI, and only sixteen of them have an ISA number: a PCI INTx line routed through ACPI's `_PRT` arrives as a bare GSI. Zircon names lines by GSI throughout, and its `interrupt_create` converts an ISA number with `remap_interrupt()` when asked to (PLAN step 11 does the same).

How the GSI gets back from the vector is the other half. Zircon never turns a vector into a GSI. Its handler table is indexed by vector; registering a handler for a GSI takes a free vector from a pool and writes it into that GSI's redirection entry (`InterruptManager::register_interrupt_handler()`), and dispatch indexes the table by the vector the CPU reports (`invoke_x86_vector()`). Linux does the same with a per-CPU `vector_irq[]` array, filled when a vector is assigned and read in `common_interrupt()`. Ours keeps `faults.cpp`'s `vector - PlatformIrqMin` (another session has uncommitted work in that file) and makes it return the GSI by programming every GSI with vector `PlatformIrqMin + GSI`: the inverse is arithmetic, and no table is needed. That is D7's deliberate departure, and it costs two things: the GSIs that can have a vector stop at 207, the size of the platform range, and a line's LAPIC priority class (the vector's upper four bits) is fixed by its pin rather than chosen. QEMU's one IO-APIC has 24 pins, so neither matters before PCI and MSI.

The table could not be built correctly at `InitLevel::Vmm` because which GSIs exist is known only once the MADT is read, and the MADT is read in `platform_init_apic()` at `InitLevel::Irq`. It has to exist before the first `irq::attach()`, the PIT's at `InitLevel::Irq + 1`. The end of `platform_init_apic()` is between the two, and is where Zircon builds its interrupt manager too, right after programming the ISA lines. Built there, the table holds exactly the GSIs that have a programmed redirection entry. A line at a GSI nothing programmed would accept an attach and never fire.

The table is read without a lock in `IrqDispatcher::handle_irq()`. That is safe because it is written once, on the boot CPU, with interrupts disabled and every line masked; every line is routed to the boot CPU, and the other CPUs start later, in `init_platform()`. It never changes afterwards.

Two ISA lines can land on one GSI, and programming both would write one redirection entry twice and give `irq::init_lines()` the same number twice. On QEMU this is ISA 0 and ISA 2: the override moves ISA 0 onto GSI 2, while ISA 2, the cascade between the two PICs, has no override and so also sits on GSI 2. ISA 2 carries no device and is skipped, as Zircon's `platform_init_apic()` skips it. Any other pair is now caught: the first line keeps the GSI and the other is logged and left unprogrammed. Linux would give the GSI to the line the override moved there rather than to the lower number; that difference is repair item 1.

**Change**
The ISA loop, before and after:

```cpp
// before: the vector comes from the ISA number
auto const irqnum = IrqVec(usize(IrqVec::PlatformIrqMin) + i);
apic_configure_isa_irq(static_cast<u8>(i), ApicDeliveryMode::Fixed, true,
                       ApicDestinationMode::Physical, static_cast<u8>(bspid), irqnum);

// after: the vector comes from the GSI, and the GSI is kept for the line table
auto const gsi = apic_io_isa_to_global(isa);
...
auto const vector = IrqVec(usize(IrqVec::PlatformIrqMin) + gsi);
apic_configure_isa_irq(isa, ApicDeliveryMode::Fixed, true, ApicDestinationMode::Physical,
                       static_cast<u8>(bspid), vector);
gsis[nr_gsis] = gsi;
++nr_gsis;
...
auto const status = irq::init_lines(ustl::views::Span(gsis).first(nr_gsis));
ASSERT(Status::Ok == status, "Failed to build the interrupt lines: {}", to_string(status));
```

The table, before and after:

```cpp
// before: kernel/kernel/irq/init.cpp
static auto initialize_early() -> void {
    // TODO(SmallHuaZi): Temporary solution for x86
    init_early(16);
}
GKTL_INIT_HOOK(Temporary, initialize_early, gktl::InitLevel::Vmm);

// after: the platform hands the irq layer the numbers it programmed
auto init_lines(ustl::views::Span<HIrqNum const> hirqnums) -> Status;
```

The PIT, before and after:

```cpp
// before
auto const status = irq::attach(ISA_IRQ_PIT, LEGACY_TIMER_OBSERVER);
...
apic_unmask_irq(apic_io_isa_to_global(ISA_IRQ_PIT));

// after
auto const gsi = apic_io_isa_to_global(ISA_IRQ_PIT);
auto const status = irq::attach(gsi, LEGACY_TIMER_OBSERVER);
...
apic_unmask_irq(gsi);
```

The keyboard needed no change: it already attached and unmasked by GSI. So did the HPET, which masks and unmasks the PIT's GSI when it takes the line over through its legacy route; that route delivers on IO-APIC pin 2, the same GSI.

The trade-offs:
- **A line's VIrqNum is its HIrqNum.** The dispatcher indexes lines by the number it is given and maps nothing, so on x86 VIrqNum is the GSI, as D9 says. A map from hardware numbers to line numbers, Linux's irq domain, is needed once MSI brings a second number space.
- **The table has null slots.** It is sized to the highest GSI given, and a number in between that no ISA line reaches, GSI 0 on QEMU, keeps a null slot that `attach()` answers with NoResource, like a number past the end. A dense table with a separate index would save a few pointers and add a lookup on every interrupt.
- **A failed build keeps nothing.** The platform panics on any failure, so a half-built table would never be used, but `init_lines()` frees what it made anyway, so its contract does not depend on its one caller.
- **Rejected: keep the vectors and translate in `platform_handle_irq()`.** A table from ISA number to GSI at dispatch would make the delivered number right, but the vector would still be computed from the ISA number, so a GSI with no ISA number, the first PCI line, would have no vector to be programmed with at all.
- **Rejected: a line for every pin of every IO-APIC.** PLAN step 7 leaves that for PCI. Until then only the ISA lines have a programmed entry, and a line for an unprogrammed pin would take an attach and never fire.
- **Rejected: Zircon's vector pool now.** D7 keeps static vectors until PCI and MSI make the pool necessary.

**Reference**
- Zircon `platform_init_apic()` (`zircon/kernel/platform/pc/interrupts.rs:109-206`), which programs the ISA lines skipping ISA 2 and builds `INTERRUPT_MANAGER` at its end; `InterruptManager::register_interrupt_handler()` (`zircon/kernel/platform/pc/interrupt_manager.rs:269-334`), which takes a vector from the pool on a line's first registration and writes it with `configure_irq_vector`; `InterruptManager::invoke_x86_vector()` (`:251-253`), which dispatches by vector; `remap_interrupt()` (`interrupts.rs:328-335`) and `apic_io_isa_to_global()` (`zircon/kernel/arch/x86/src/ioapic.rs:573-582`), which turn an ISA number into its GSI.
- Linux `mp_config_acpi_legacy_irqs()` (`arch/x86/kernel/acpi/boot.c:1113-1177`), which maps each ISA IRQ to its GSI and skips a pin already mapped; `assign_irq_vector()` (`arch/x86/kernel/apic/vector.c:275`), which gives an irq a vector, and `common_interrupt()` (`arch/x86/kernel/irq.c:273`), which finds the irq through the per-CPU `vector_irq[]`.

**Verification**
- Build of `out/bringup.x86`: ok.
- `boot_check.py`: dbg PASS (shell finished at 15.0 s, 24 passed, 1 skipped, 0 failed, 0 error lines); rel PASS (3.6 s, 22 passed, 3 skipped, 0 failed, 0 error lines).
- The dbg log's IO-APIC trace shows ISA 0 programmed first with vector 34, then vectors 33 and 35 to 47: GSI 2 for ISA 0, nothing at GSI 0, and ISA 5, 9, 10 and 11 level-triggered as q35's overrides say.
- The tick arrives on vector 34 and is dispatched as GSI 2: a temporary log line in `timer_interrupt()`, removed before the commit, printed `s07-probe: legacy timer interrupt on line 2` for each of the first three ticks; the trace above shows GSI 2 programmed with vector 34, the only vector that subtracts to 2.
- The boot depends on that: with the PIT attached by ISA 0 as before, also temporarily, `attach` answered NoResource (`Failed to register legacy timer interrupt: 7`), since no ISA line reaches GSI 0, and dev.dbg_1 stopped with `no "--- end ---" within 60s (hung or looping)`, reached: kernel.
- The keyboard, by step 5's script rather than by hand: after `--- end ---`, Ctrl-A c, `sendkey a`, `sendkey shift-b`, `sendkey c`, Ctrl-A c into QEMU's monitor; the serial log decoded `0x61(a) 0x42(B) 0x63(c)` through GSI 1, vector 33.
- clang-format: clean on the changed lines. clang-tidy on the five changed sources: nothing on the changed lines; three older naming warnings remain in lines this change did not touch (`objects_`, `CURRENT_FRAME`, `IRQ_DISPATCHER`).
- Not tested: `init_lines()`'s refusals (BadState, InvArgs, AlreadyExists, NoMemory) and the skip of a GSI two ISA lines share, which QEMU's MADT never reaches. The irq layer has no kernel test until PLAN step 6's `irq.test`.

**Deliberately not done**
- Vectors stay static, `PlatformIrqMin + GSI` (D7); a pool waits for PCI and MSI.
- Lines exist only for the GSIs the ISA lines reach; a line for every pin waits for PCI.
- No chip is bound to the lines, and the PIT, HPET and keyboard still call `apic_mask_irq` and `apic_unmask_irq` directly (PLAN step 8). The PIT still unmasks its line after a failed attach (PLAN steps 6 and 8).
- Step 1's IO-APIC lookup fix (`irq/s01-ioapic-lookup`) was not cherry-picked: the GSIs the ISA lines reach on QEMU are 1 to 15, below the off-by-one at the last pin, GSI 23, so this step does not depend on it.
- `faults.cpp` is untouched: its `irqvec_to_irqnum()` already returns the GSI once the vectors are `PlatformIrqMin + GSI`, and another session has uncommitted work in it.
- When two ISA lines reach one GSI, the lower ISA number keeps it rather than the line an override moved there, as Linux would: repair item 1.
- No COM1 registration exists in this tree. When the console-input session adds one, it attaches by `apic_io_isa_to_global(ISA_IRQ_SERIAL1)`.

**How it was found**
None. PLAN step 7 of `.agents/tasks/microkernel-interrupts/`, from defects 7 and 10 of its design.

**The user's prediction**
None.

**Three questions you should be able to answer**
- An interrupt carries an ISA number, a GSI and a vector, and the CPU reports only the vector. Why did that make the PIT's line 0 while its IO-APIC entry was GSI 2, and why did the keyboard's GSI happen to work?
- Zircon never turns a vector back into a GSI. How does its dispatch find the handler, and what did ours give up by programming every GSI with vector 32 + GSI instead?
- Why must the line table be built after the MADT has been read but before the PIT's attach, and which line did the old fixed table of 16 lines get wrong on QEMU?

## Title: A spin lock per interrupt line, Permanent lines, and a detach that waits for the handler (2026-10-04)

**Files**
- `kernel/include/ours/irq/irq_object.hpp` — the unused `sync::Mutex request_mutex_` gives way to L1, a `sync::SpinLock` taken with IrqSave that guards the line's observer list; the flags word becomes atomic, written only under L1; a count of interrupts nobody handled; `notify()` answers an `IrqReturn`; the members take the `m_` names the class's style asks for, and the class is `final`.
- `kernel/kernel/irq/irq_object.cpp` — `attach`, `detach` and `notify` take L1; a Permanent line dispatches without it, refuses every further attach and refuses to let its observer go; `attach` adds to the stored flags instead of overwriting them, and `detach` clears Shared when the last observer leaves; `notify` combines the observers' answers and counts an interrupt nobody handled.
- `kernel/include/ours/irq/irq_observer.hpp` — `is_permanent()`; the comment says that one observer is never attached and detached on two CPUs at once.
- `kernel/include/ours/irq/mod.hpp`, `kernel/kernel/irq/mod.cpp` — `attach` and `detach` state the Permanent rules and the barrier; `irq::spurious_count()`.
- `kernel/include/ours/irq/irq_dispatcher.hpp`, `kernel/kernel/irq/irq_dispatcher.cpp` — `handle_irq` answers what the line's observers answered, where it always answered Handled; `spurious_count()`.
- `kernel/commands/irq_test.cpp`, `kernel/commands/BUILD.gn` — `irq.test`.
- `docs/repairs/261004/kernel-irq.md` — item 2: L1 has no lockdep class of its own.

**Symptom**
Latent, cannot be triggered today. Defects 1, 8 and 9 of `.agents/tasks/microkernel-interrupts/DESIGN.md`:
- `attach` and `detach` changed a line's observer list in thread context while `notify` walked it in interrupt context, with no lock. Nothing has gone wrong only because the two drivers attach before they unmask their lines, and nothing detaches. PLAN step 11 attaches and detaches live lines from system calls, and a process can close an interrupt handle at any moment.
- `Permanent` was read by nobody. The PIT asked for it, and the first `attach` on the line then assigned the flags word outright, `irqflags_ = observer.is_shared() ? Shared : None`, so the request was dropped. A `detach` of the PIT's observer would have succeeded.
- `notify` dropped each handler's `IrqReturn`, and `handle_irq` answered Handled even on a line with no observer, so an interrupt nobody handled could not be told from one somebody did.

**Cause**
A line's observer list has two kinds of users, and they run in different contexts, usually on different CPUs. `attach` and `detach` run in thread context: a driver's init today, an `interrupt_create` or a handle's last close from PLAN step 11 on. `notify` runs in interrupt context, on whichever CPU took the interrupt. The list is an intrusive doubly linked list, so unlinking an observer rewrites its neighbours' pointers and resets its own. A walk in progress on another CPU holds a pointer to that very node and, once its handler returns, follows the node's `next`: after the unlink that is no longer the rest of the list. The mutation run under Verification shows exactly this.

The list is not the whole problem; the observer's lifetime is. In step 11 an interrupt object embeds its observer, and the observer's context is the object itself. The object calls `detach` in `Destroy` and may be freed as soon as it returns. If a handler is still running on another CPU at that moment, it dereferences the freed object. So `detach` must be a barrier: once it returns, no handler of that observer is running anywhere, and none will start.

There are two classic ways to build that barrier:
- Zircon runs the handler under the handler-table entry's lock: `InterruptTableEntry::invoke_if_present()` takes the entry's spin lock and calls the handler inside it, and `set_handler()` takes the same lock to change the entry. Taking the lock is the barrier: it cannot be acquired while a handler runs, and after the change no dispatch finds the old handler.
- Linux drops `desc->lock` while the actions run, marking the descriptor `IRQD_IRQ_INPROGRESS` instead; `__free_irq()` unlinks the action under the lock, then calls `__synchronize_irq()`, which waits for the in-progress flag to clear.

DESIGN.md's lock order chose Zircon's: L1 is held while the handlers run, and `detach` takes it. That is what lets step 11's handler take L2 inside L1 and lets `Destroy` rely on `detach` alone. The price is that a handler must never attach or detach on its own line: it would spin on the lock its own dispatch holds.

The lock has to be a spin lock, and taken with interrupts off. The Mutex that sat unused in the class could never have served: a sleeping lock cannot be taken in interrupt context, which has no thread to put to sleep. A spin lock taken with interrupts on is a deadlock waiting for its moment. A thread on CPU0 holds L1 in `attach`; the line fires, and every ISA line is routed to CPU0; the interrupt preempts the thread on that same CPU, and `notify` spins for L1, which the thread it interrupted holds and will never release. IrqSave closes that window.

Permanent exists so the kernel's own lines skip that lock. The timer fires on every tick, its observer never changes, and Zircon's `invoke_if_present()` dispatches a permanent entry without taking the lock. Skipping the lock is only safe if, once a dispatch has seen Permanent, the list can never change again. Three rules make that true:
- `attach` refuses everything on a Permanent line, shared or not.
- `detach` refuses the Permanent line's observer, with AlreadyBound, as Zircon's `set_handler()` refuses to clear a permanent entry.
- A Permanent observer takes only an empty line. On a shared line it would freeze the observers already there, which could then never leave.

The order between linking and publishing matters too. `attach` links the observer first and then stores the flags with release; `notify` loads them with acquire. A dispatch that sees Permanent therefore also sees the linked observer. Zircon's `is_permanent()` uses relaxed loads, reasoning that permanent handlers are set at startup; its own comment in `invoke_if_present()` speaks of acquire-release, which is what ours does.

The flags word follows the repo's rule for a flags word: every bit is written under the same protection, L1. The word is `std::atomic` only because `notify` reads Permanent without L1. Shared needed a change of meaning once `attach` stopped overwriting the word. It used to be reset by the next first attach; now it follows the line's occupants, set by the first shared observer and cleared by `detach` when the last one leaves. Otherwise an exclusive observer could never take a line that had once been shared.

The return value: on a shared line every observer asks its own device whether it raised the line, and the line counts as handled if any one says so. That is Linux's `__handle_irq_event_percpu()`, which ORs every action's return. Linux's `note_interrupt()` then counts what nobody handled in `irq_desc::irqs_unhandled`, and shuts a line off once 99,900 of 100,000 interrupts went unhandled. Without the count an interrupt storm on a line nobody handles is invisible. The count is atomic and relaxed: it is a statistic, and the Permanent path bumps it without any lock.

**Change**
`notify`, before and after:

```cpp
// before
auto IrqObject::notify() -> void {
    for (auto &observer: observers_) {
        auto _ = observer.on_event();
    }
}

// after
auto IrqObject::notify() -> IrqReturn {
    auto result = IrqReturn::None;
    if (is_permanent()) {                    // acquire; the list never changes again
        result = notify_all(permanent_observers());
    } else {
        Guard<sync::SpinLock, IrqSave> const guard{m_lock};
        result = notify_all(m_observers);   // handlers run under L1
    }
    if (result == IrqReturn::None) {
        m_spurious.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}
```

`attach`, before and after, in the part that handled the flags:

```cpp
// before: the first attach overwrote the word, Permanent included
if (observers_.size() == 0) {
    irqflags_ = observer.is_shared() ? IrqFlags::Shared : IrqFlags::None;
}

// after: under L1; the word only gains bits, and is published last
if (!!(flags & IrqFlags::Permanent)) {
    return Status::AlreadyBound;
}
...
m_observers.push_back(observer);
observer.m_virqnum = m_data.virqnum;
if (observer.is_permanent()) {
    flags |= IrqFlags::Permanent;
}
m_flags.store(flags, std::memory_order_release);
```

`detach` takes L1 first, refuses a Permanent line, and clears Shared when the line empties.

The trade-offs:
- **The lock across the handlers, Zircon's barrier, over Linux's in-progress flag.** Linux's lets a detach on a shared line wait only for the dispatch, not hold off attaches, and keeps the handlers outside the descriptor lock; it costs a flag, a wait loop, and a second protocol to reason about. DESIGN.md's lock order L1 → L2 rests on Zircon's.
- **IrqSave in `notify` too**, although a real interrupt arrives with interrupts already off. It costs a flags save and restore, and lets `irq.test` drive a dispatch from thread context through the same function an interrupt reaches.
- **A Permanent observer cannot join a shared line**, as Zircon's table entry has only one handler to make permanent.
- **Deviation: L1 has no lockdep class of its own.** The plan asked for one. `lockdep::Guard` files every `SpinLock` under one `DefaultLockClass`, and the per-declaration class `system/lib/lockdep/docs/interface.md` specifies, `lockdep::Lock<T>` with `class_id<Tag>()`, is not implemented. `system/lib/lockdep` is an uninitialised submodule whose directory holds unversioned work, so this step could not add it. L1 is taken only through `lockdep::Guard`, so lockdep will see it once it can tell locks apart; repair item 2 records the rest.
- **Rejected: a reader-writer lock**, read-held by dispatch. Dispatches of one line do not overlap today, since every ISA line is delivered to the boot CPU and handled with interrupts off, so sharing the read side would buy nothing and cost more than a spin lock on every interrupt.

**Reference**
- Zircon `InterruptTableEntry::invoke_if_present()` (`zircon/kernel/platform/pc/interrupt_manager.rs:90-114`), which runs a permanent handler without the entry's lock and every other one under it; `InterruptTableEntry::set_handler()` (`:120-139`), which answers ALREADY_BOUND on a permanent entry or an occupied one; `is_permanent()` (`:82-86`); `InterruptManager::register_interrupt_handler()` (`:269`), which refuses a permanent vector (`:294-296`).
- Linux `__handle_irq_event_percpu()` (`kernel/irq/handle.c:185`), which ORs the actions' returns (`:236`); `note_interrupt()` (`kernel/irq/spurious.c:222`), which counts `irqs_unhandled` (`:342-364`); `__free_irq()` (`kernel/irq/manage.c:1859`), which unlinks under `desc->lock` and then calls `__synchronize_irq()` (`:118`, called at `:1935`); `__setup_irq()` (`:1471`), which joins a line only if both actions say IRQF_SHARED (`:1627`).

**Verification**
- Build of `out/bringup.x86`: ok.
- `boot_check.py`: dbg PASS (shell finished at 32.4 s, 28 passed, 2 skipped, 0 failed, 0 error lines); rel PASS (3.6 s, 25 passed, 7 skipped, 0 failed, 2/2 required, 0 error lines). A boot alone does not show that a real interrupt can take L1: the keyboard's line is the only one the kernel dispatches under L1, and with no key pressed nothing is known to raise it.
- A real interrupt under L1, added at review: on dev.dbg_1 after the boot, `sendkey a` and `sendkey b` through QEMU's monitor (Ctrl-A c), driven by a script over `scripts/kshell_run.py`'s console, logged the keyboard handler's `char 0x61` and `char 0x62`, so the keyboard's line was dispatched in interrupt context through `notify()`'s IrqSave guard with no assertion; `irq.test` then passed in the same boot, `ok -- 24 checks`, and the boot logged no error line.
- `irq.test` on dev.dbg_1, `scripts/kshell_run.py`: `irq.test: ok -- 24 checks`. It checks that a second registration on the PIT's line, GSI 2, answers AlreadyBound, exclusive or shared. On a Permanent line of the test's own, outside the line table, it checks the Permanent rules: such an observer cannot join an occupied line, the line refuses every other attach, it refuses to let its observer go, and its dispatch without the lock still reaches the observer. On ISA 3, GSI 3, which nothing uses, it attaches and detaches an exclusive observer 1000 times, a second exclusive observer answering AlreadyBound each time, with a dispatch reaching only the attached one. Two shared observers hold the line together, and once both leave an exclusive one takes it again. A dispatch on the empty line answers None and is counted as spurious. For the barrier, a thread pinned to another CPU dispatches the line in a loop while the command attaches and detaches 300 times, waiting each time for a dispatch to reach the observer, and checks that no handler is in flight when `detach` returns and none runs after it. The run logged `300 rounds, 930 dispatches handled, 143121 found the line empty`, and every unhandled dispatch was counted as spurious.
- The barrier check catches a missing barrier: with L1 removed from `detach` alone, a temporary mutation, the dispatching CPU took a page fault at `0xfffffffffffffff8` with `spinlocks=1`, a walk following an unlinked node's null `next` back to its container, and `kshell_run.py` reported `irq.test: FAIL, no "irq.test: ok" or FAILED line within 120s`.
- clang-format: clean on the changed lines. clang-tidy on the four changed sources: nothing on the changed lines; three older naming warnings remain on lines this change did not touch (`objects_`, `CURRENT_FRAME`, `IRQ_DISPATCHER`).
- Not tested: a real interrupt racing a `detach`. The test drives dispatches from thread context through `irq::handle_irq_generic()`, which is the same function and the same lock. Also not tested: the lock order, which lockdep cannot check yet (`docs/repairs/260917/kernel-task-sched.md` item 6, and repair item 2).

**Deliberately not done**
- L1's own lockdep class: repair item 2.
- Holding preemption off in `notify()` as well, found at review. A dispatch `irq.test` drives from thread context runs the handlers under L1 with preemption enabled, unlike one in interrupt context, so a handler that woke a thread there could switch away holding L1. Its handlers wake nobody, so nothing reaches it today; repair item 3 has the recommended guard and why it is put to the user.
- Shutting off a line that keeps firing for nobody, as Linux's `note_interrupt()` does. Interrupts are only counted.
- An interrupt on a GSI with no line is not counted anywhere: `handle_irq` answers None before there is a line to count it on.
- The PIT still unmasks its line after a failed attach, and the chip is still unbound (PLAN step 8, which changes `time.cpp`).
- `irq.test` names the PC platform's lines, through `apic_io_isa_to_global()` and `ISA_IRQ_*`, so `kernel/commands`, built for every architecture, now includes an x86 header. It builds because x86 is the only kernel that builds; once a second one does, `kernel/commands/BUILD.gn` adds the file only for the PC platform.
- The concurrency argument CLAUDE.md asks for before a lock decision was skipped for this run, by the user's instruction; it stays open in DESIGN.md's Open questions.

**How it was found**
None. PLAN step 6 of `.agents/tasks/microkernel-interrupts/`, from defects 1, 8 and 9 of its design.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does holding L1 while the handlers run make `detach` a barrier, and what does Linux do instead, given that it drops `desc->lock` before calling the actions?
- A thread holds L1 with interrupts enabled on CPU0, and the line fires on CPU0. What happens, and why would it not happen if the interrupt went to CPU1?
- Why may a Permanent line skip L1, and which three refusals in `attach` and `detach` keep that safe? Why must `attach` link the observer before it publishes Permanent with a release store?
