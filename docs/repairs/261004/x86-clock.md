# x86-clock outstanding repairs

## 1. The HPET's channel 0 becomes the tick device even when the HPET cannot take over IRQ0

**Severity**
Medium: on an HPET without the legacy replacement capability the tick stops at `InitLevel::Irq + 2`, and the boot hangs. QEMU's HPET has the capability, so it is not reached today.

**Symptom**
Latent, cannot be triggered today. It would be on a machine whose HPET clears LEG_RT_CAP in its general capabilities register: no tick after the HPET is enrolled, so jiffies stop, and the local APIC timer calibration, which waits for jiffies, would wait forever (inferred from the code, not run).

**Cause**
`hpet_init_internal()` takes IRQ0 from the PIT only inside `if (mmio->general_caps.support_legacy())` (`kernel/arch/x86/kernel/time/hpet.cpp:320-330`), and returns Ok either way. `hpet_init()` (`:365-389`) then does the rest unconditionally:
- `ClockEventDevice::enroll()` offers channel 0 to TickDevice, whose rating, 120, beats the PIT's 110 (`HPET_CLKEVT_RATING`, `kernel/driver/clock/i8253.cpp:33`) on purpose, so TickDevice takes it and detaches the PIT, which shuts the PIT down;
- `time::set_global_clock_event()` points the legacy timer line at channel 0.

Without the legacy route nothing programs channel 0's own interrupt route (Tn_INT_ROUTE_CNF), so its interrupts reach no line, and the PIT that did reach IRQ0 is stopped. The rating was raised above Linux's on the stated assumption that the HPET owns IRQ0 (`hpet.cpp:33-44`); without LEG_RT_CAP that assumption is false, and so is the comment on `set_global_clock_event()` in `kernel/arch/x86/kernel/include/ours/arch/time.hpp`, which says hpet_init() calls it once the HPET has taken IRQ0. This predates PLAN step 5 of `.agents/tasks/microkernel-interrupts/`, which only renamed the assignment: `time::CLOCK_EVENT = &device` was unconditional too.

**Change**
Follow Linux: enroll channel 0 as a clock-event device and call `set_global_clock_event()` only when the legacy route was enabled, for example by having `hpet_init_internal()` report it, and otherwise register the HPET as a clock source alone, leaving the PIT as the tick. Item 2 changes the order of the same two calls, so the two are best fixed together. Rejected: routing channel 0 through Tn_INT_ROUTE_CNF to a free IO-APIC pin, which needs a line of its own in the irq layer; Linux does that only for per-CPU HPET channels (`hpet_assign_irq()`), never for the legacy tick.

**Reference**
Linux `hpet_enable()` (`arch/x86/kernel/hpet.c:1087-1093`), which calls `hpet_legacy_clockevent_register()` only `if (id & HPET_ID_LEGSUP)`.

**Acceptance**
`python3 .agents/skills/ours-verify/scripts/boot_check.py` passes, and with `hpet_init_internal()`'s legacy branch forced off by hand, dev.dbg_1 still boots on the PIT's tick, logging that the HPET is a clock source only.

## 2. The legacy timer line is pointed at the HPET only after the HPET already drives it

**Severity**
Low: at most a tick or two is dropped once, at boot, on a machine whose HPET supports the legacy replacement route.

**Symptom**
Latent and not observed. Between `ClockEventDevice::enroll()` starting the HPET and `time::set_global_clock_event()` publishing it, an HPET interrupt on IRQ0 reaches the PIT's device, whose handler TickDevice has already taken, and the tick is dropped. Channel 0's first interrupt is due one tick after `enroll()` programs it (`HpetChannel::set_mode_periodic()`), so the window has to outlast a whole tick, which takes a stall such as a preempted vCPU.

**Cause**
`hpet_init()` (`kernel/arch/x86/kernel/time/hpet.cpp:365-389`) runs three things in this order:
- `hpet_init_internal()` enables the legacy replacement route and unmasks the line (`:325`, `:329`), so from here on IRQ0 is the HPET's channel 0 and no longer the PIT;
- `ClockEventDevice::enroll()` (`:386`) reaches `TickDevice::check_new_device()` (`kernel/kernel/time/clock_event_device.cpp:104`), which rebinds the tick to the HPET's device: it takes the PIT device's handler (`kernel/kernel/time/tick_device.cpp:192`) and programs the HPET to fire;
- only then does `set_global_clock_event(device)` (`hpet.cpp:388`) tell the line's handler that the HPET's device is the one to call.

An interrupt between the second and the third goes to the PIT's device, whose `handle_event()` finds no handler and does nothing. Linux has the same order in `hpet_legacy_clockevent_register()`, but there IRQ0 has no handler yet at that point: `setup_default_timer_irq()` registers it afterwards, in `hpet_time_init()`. Here the PIT registered the line at `InitLevel::Irq + 1`, so the line is live throughout.

**Change**
Publish the HPET's device before `enroll()` can make it fire: call `set_global_clock_event()` right after `hpet_init_internal()` has enabled the legacy route. Until `enroll()` hands the tick to the HPET, an interrupt then finds the HPET's device with no handler, which is harmless, since channel 0 is not programmed to fire before `enroll()`. Rejected: re-attaching the line's observer with the HPET's device as its context, since the line is Permanent and PLAN step 6 of `.agents/tasks/microkernel-interrupts/` makes a Permanent line refuse that.

**Reference**
Linux `hpet_legacy_clockevent_register()` (`arch/x86/kernel/hpet.c:419-469`), which sets `global_clock_event` after `clockevents_config_and_register()`, and `setup_default_timer_irq()` (`arch/x86/kernel/time.c:43-54`), which registers IRQ0 only afterwards.

**Acceptance**
`python3 .agents/skills/ours-verify/scripts/boot_check.py` passes, and `hpet_init()` calls `set_global_clock_event()` before `ClockEventDevice::enroll()`.
