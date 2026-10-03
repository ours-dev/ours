# kernel-time changes

## Title: ClockEventDevice::enroll() answers nothing, and arch, driver and ktl answer for every Status they used to drop (system-abi-surface step 2, part 4) (2026-10-03)

**Files**
- `kernel/include/ours/time/clock_event_device.hpp`, `kernel/kernel/time/clock_event_device.cpp` — `enroll()` returns void; a driver whose vtable lacks a method its features claim is refused with a message and a debug assertion; a device that fails to start is logged.
- `kernel/arch/x86/kernel/time/hpet.cpp` — `ClockSource::enroll()` dropped explicitly.
- `kernel/arch/x86/kernel/mp.cpp` — the non-interruptible sleep asserts Ok; the interruptible one is dropped explicitly.
- `kernel/arch/x86/apic/io_apic.cpp` — `IoApic::issue_eoi()` dropped explicitly, pointing at the new repair item.
- `kernel/lib/ktl/include/ktl/name.hpp` — both constructors drop `set()` explicitly.
- `docs/repairs/261003/x86-apic.md` — new: an IOAPIC older than version 0x20 never gets an EOI.

**Symptom**
The last 8 of the 44 silent drops counted for step 2. Four of them were callers of `ClockEventDevice::enroll()`, which answered a Status that no caller could act on: one of its answers, AlreadyBound, is not a failure at all.

**Cause**
`ClockEventDevice::enroll()` answered three kinds of thing in one Status:
- InvArgs: the driver's vtable lacks a method its features claim. A driver bug, already logged inside, and the device not enrolled.
- AlreadyBound: this CPU's TickDevice keeps the device it has, because it is preferable. A normal outcome; the new device is still enrolled.
- The error that stopped the swap part-way, from switching the new device into service.

A driver can do nothing with any of them. The first is a bug to fix in the source, the second is not its business, and the third is about the tick layer, not the driver. A Status that carries a non-error to a caller that cannot act on it only trains callers to ignore it, which is what all three did. So `enroll()` now returns void, as Linux's does, and handles each case where it is known: the bug is logged, refused and asserted, AlreadyBound is accepted, and a failed swap is logged with the device and the CPU.

The other drops:
- `ClockSource::enroll()` keeps its Status, which a caller could act on, but the HPET's has nothing to do with it: every failure is logged inside, the clocks keep the source they had, and the HPET's comparators do not depend on it.
- `sleep_for()` with `Interruptible::No` can only be woken by its timeout, which answers Ok, so it is asserted. With `Interruptible::Yes`, a signal ends it early with InternalIrqRetry, which only shortens a pause before a bounded wait, so it is dropped.
- `Name::set()` answers InvArgs only for a null name, and leaves the name empty. That is all a constructor can do with one.
- `IoApic::issue_eoi()` answers NotSupported on an IOAPIC older than version 0x20, which has no EOI register. Dropping it there means the EOI is not sent at all, where Linux falls back to toggling the entry to edge-triggered and back. That gap is now `docs/repairs/261003/x86-apic.md` item 1. The QEMU this project boots emulates version 0x20 by default, so it is latent.

**Change**

```cpp
// ClockEventDevice::enroll(), before
    return TickDevice::Current::get().check_new_device(&self);
// after
    // Whether this CPU takes the device is TickDevice's decision, not the
    // driver's: AlreadyBound means it keeps the one it has, which is logged
    // there and is no failure. Anything else stopped the swap part-way.
    auto const status = TickDevice::Current::get().check_new_device(&self);
    if (Status::Ok != status && Status::AlreadyBound != status) {
        log::error("ClockEventDevice {} could not be put into service on CPU[{}]: {}", ...);
    }
```

The four vtable checks now set one message and share one log, assertion and return, instead of returning InvArgs each.

**Reference**
- Linux's `clockevents_config_and_register()` and `clockevents_register_device()` (kernel/time/clockevents.c) return void. Whether the device becomes a CPU's tick device is decided underneath, by `tick_check_new_device()` (kernel/time/tick-common.c), which the driver never hears from.
- `clocksource_register_hz()` returns an int, which x86's `hpet_enable()` (arch/x86/kernel/hpet.c) does not check.
- `__eoi_ioapic_pin()` (arch/x86/kernel/apic/io_apic.c) is the fallback for an IOAPIC without an EOI register.

**Verification**
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped).
- No "could not be put into service" line and no "keeps tick device" line in either log: every enroll on this machine installs its device.
- QEMU's IOAPIC version default, read from `qemu-system-x86_64 -M q35 -device ioapic,help`: 32 (0x20).

**Deliberately not done**
- The IOAPIC fallback itself (the repair item).
- The hazard behind option C, which the user did not choose: in a release build, a refused HPET device still becomes `time::CLOCK_EVENT`. A debug build stops at the assertion first, and only a driver bug reaches it.
- The `[[nodiscard]]` attribute itself, in the next commit.

**How it was found**
The count for step 2. The user chose between three options: `enroll()` returning void (A, recommended), each caller dropping it with a reason (B), or each caller checking it, with `hpet_init()` returning the error (C).

**The user's prediction**
None: the user accepted option A as recommended, without a prediction.

**Three questions you should be able to answer**
- Why is AlreadyBound a poor thing to answer as a Status, even though it is a well-defined outcome?
- Which of `ClockEventDevice::enroll()`'s old answers could a driver have acted on, and what does each become now?
- What does an IOAPIC older than 0x20 need instead of an EOI register write, and what goes wrong without it?
