# x86-apic outstanding repairs

## 1. APIC calibration fails a boot when the host runs QEMU slowly, because its factor-of-two check takes host scheduling for a misprogrammed tick

**Severity**
Medium: the kernel is correct, but a slow or loaded host makes it log an error and refuse its calibration, and `boot_check.py` then fails both images. Not reached on an idle host.

**Symptom**
QEMU started at macOS background priority (efficiency cores only, `setpriority(PRIO_DARWIN_PROCESS, 0, PRIO_DARWIN_BG)`) boots both images to their end. Each logs:

```text
[... cpu0 error] APIC calibration: the tick device and the clock source disagree by more than a factor of two; the tick is misprogrammed
```

The same images pass at normal priority (2026-10-07, `.agents/tasks/build-cache/PLAN.md` step 5, Revised).

**Cause**
`kernel/arch/x86/apic/local_apic.cpp`, around line 620, measures the tick period by the clock source over `SPAN` ticks and refuses the calibration when the measurement is more than twice what is expected. Under TCG, QEMU's emulated timers follow host time: when the host deschedules QEMU's threads, timer interrupts arrive late, and the window holding `SPAN` ticks stretches with them. The check's comment already allows for 7% of this noise. At background priority the stretch passes 2x, so a host that is slow, rather than a tick device that is wrong, is reported as "misprogrammed" and fails the calibration.

**Change**
To decide, with numbers, before choosing:

- **Retry the calibration** a few times before refusing. A misprogrammed tick fails every time, while host noise does not. This is cheap and keeps the check meaningful.
- **Measure over a longer window** under a hypervisor, where CPUID's hypervisor bit is set.
- **Downgrade to a warning under a hypervisor** and keep the error on bare metal. This was ruled out as a first choice, since it hides a real misprogramming in exactly the setting this project tests in.

**Reference**
Linux `calibrate_APIC_clock()` (`arch/x86/kernel/apic/apic.c`) cross-checks the APIC timer against a reference too, and under a hypervisor prefers the frequency the hypervisor reports over a measurement. Zircon's counterpart was not checked for this note.

**Acceptance**
`boot_check.py` passes both images with QEMU at background priority (start `qemu_boot.py` from a process that has called the `setpriority` above), and a tick deliberately misprogrammed by a factor still fails the calibration.
