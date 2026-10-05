# kernel-irq outstanding repairs

## 1. When two ISA lines land on one GSI, the lower ISA number keeps it, where Linux gives it to the line an override moved there

**Severity**
Medium: the wrong ISA line would own the GSI, but only on firmware whose MADT moves one ISA line onto another's own pin, other than ISA 0 onto GSI 2. QEMU's does not.

**Symptom**
Latent, cannot be triggered today. On such firmware, say an override of ISA 9 onto GSI 5 with ISA 5 left alone, the boot logs `ISA IRQ 9 shares GSI 5 with an earlier ISA line; not programmed`, the IO-APIC entry for GSI 5 carries ISA 5's trigger mode and polarity instead of the override's, and a driver for the device the firmware actually wired to GSI 5 gets a line configured for the wrong one.

**Cause**
`platform_init_apic()` (`kernel/platform/pc/interrupt.cpp`) walks the ISA lines in order, 0 to 15, and gives each GSI to the first line that reaches it, skipping any later line on the same GSI so that `irq::init_lines()` is not handed a number twice. The order says nothing about which line is real. An override is the firmware saying where a line is wired; a line with no override only defaults to the pin of its own number. When the two meet on one pin, the override is the one to believe. The ISA 0 and 2 pair, the only one QEMU has, comes out right only because ISA 2 is skipped before the check, as Zircon skips it.

**Change**
Program the lines an override moved, those whose GSI differs from their ISA number, in a first pass, and the rest in a second pass that skips a GSI already taken, so an override always wins. Rejected: programming every line and letting the later one overwrite the entry, as Zircon's `platform_init_apic()` does, since the table would then hold a line whose entry belongs to another, and `init_lines()` refuses a repeated number.

**Reference**
Linux `mp_config_acpi_legacy_irqs()` (`arch/x86/kernel/acpi/boot.c:1113-1177`), which runs after the overrides are recorded and skips an ISA IRQ whose IO-APIC pin already has a mapping. Zircon `platform_init_apic()` (`zircon/kernel/platform/pc/interrupts.rs:109-206`) has no such check.

**Acceptance**
`python3 .agents/skills/ours-verify/scripts/boot_check.py` passes, and with a fake override of ISA 9 onto GSI 5 injected by hand into the parsed list in `platform_init_apic()`, dev.dbg_1 logs that ISA 5, not ISA 9, was not programmed, and the IO-APIC trace line for GSI 5 shows the override's trigger mode.

## 2. The line lock L1 has no lockdep class of its own: lockdep files it with every other spin lock

**Severity**
Low: nothing is wrong at run time, and lockdep cannot be switched on yet (`docs/repairs/260917/kernel-task-sched.md` item 6). Once it can, the lock order the interrupt path depends on, L1 → L2 → {LI, L3} (`.agents/tasks/microkernel-interrupts/DESIGN.md`, Lock order), cannot be checked.

**Symptom**
Latent, cannot be triggered today. With lockdep linked in, the first dispatch of a line whose handler takes another spin lock, an interrupt object's L2 from PLAN step 11 or the console's lock when the keyboard handler logs, would be reported as the same class acquired twice. An inverted order between L1 and any other spin lock would read the same way, so a real inversion could not be told from the false report.

**Cause**
`lockdep::Guard` registers each acquisition under `DefaultLockClass<Lock>::id()` (`system/lib/lockdep/include/lockdep/guard.hpp`), one class per lock *type*. Every `sync::SpinLock` in the kernel is that one class, so lockdep sees L1 nested inside a run-queue lock, a wait-queue lock or an interrupt object's lock as one lock taken inside itself. PLAN step 6 asked for L1 to get a class of its own. The mechanism for that, a lock declaration naming its class (`lockdep::Lock<sync::SpinLock> m_lock{lockdep::class_id<struct IrqLineLock>()}`), is specified in `system/lib/lockdep/docs/interface.md` but not implemented: `lockdep::Lock` is an empty placeholder in `lock_class.hpp`. `system/lib/lockdep` is an uninitialised submodule whose directory holds unversioned work, so the step could not add it from this repository.

**Change**
Once `lockdep::Lock<T>` and `class_id<Tag>()` exist and `Guard` takes the class from the lock it is given, declare `IrqObject::m_lock` as `lockdep::Lock<sync::SpinLock>` with a tag of its own, say `struct IrqLineLock`. Every line shares that class, as every Linux `irq_desc::lock` shares one: no path holds two lines' locks at once. Ruled out: a `LockClass<IrqObject, SpinLock>` passed by hand at the three `Guard` sites in `irq_object.cpp`, which would need `Guard` to take a class id it does not take today, and would have to be repeated at every acquisition instead of once at the declaration.

**Reference**
Zircon's C++ kernel names a lock's class at its declaration, `DECLARE_SPINLOCK(containing_type)` (`zircon/kernel/include/kernel/spinlock.h:196-197`), through `LOCK_DEP_INSTRUMENT` and the `LockDep<Class, LockType, Index>` wrapper (`fuchsia/src/lib/lockdep/include/lockdep/lock_class.h`); its interrupt table entry has since moved to Rust, `InterruptTableEntry::mu` (`zircon/kernel/platform/pc/interrupt_manager.rs:61-63`). Linux `init_desc()` (`kernel/irq/irqdesc.c:208`) calls `lockdep_set_class(&desc->lock, &irq_desc_lock_class)` (`:223`), which gives every `irq_desc::lock` one class of its own.

**Acceptance**
With lockdep enabled (after `kernel-task-sched` item 6), `irq.test` on dev.dbg_1 reports no violation, and a deliberate inversion, an `irq.test` handler that takes a spin lock the test also holds around an `irq::attach()` of a line, is reported as an order violation between two named classes.
