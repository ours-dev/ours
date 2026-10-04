# x86-clock outstanding repairs

## 1. The legacy timer line is pointed at the HPET only after the HPET already drives it

**Severity**
Low: at most a tick or two is dropped once, at boot, on a machine whose HPET supports the legacy replacement route.

**Symptom**
Latent and not observed. Between `ClockEventDevice::enroll()` starting the HPET and `time::set_global_clock_event()` publishing it, an HPET interrupt on IRQ0 reaches the PIT's device, whose handler TickDevice has already taken, and the tick is dropped.

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
