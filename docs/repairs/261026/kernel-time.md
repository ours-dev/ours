# kernel-time outstanding repairs

## 1. After `tickreplace.test`, preemption on the CPU it used is late and uneven

**Severity**
Medium: slices end early and late on a CPU whose tick device has been swapped, but only after swaps, which today only the test's fake devices cause.

**Symptom**
`preempt.test` passes on its own and after `hotplug.test`, and fails every time it runs after `tickreplace.test` in the same boot, on the same CPU (CPU 3, the last). Seen on 2026-10-26 at 21a17c02, before and after the M2 change (`docs/changes/261026/kernel-time.md`), with `scripts/kshell_run.py --out out/bringup.x86 tickreplace.test preempt.test preempt.test`. Its two spinners take 11 to 13 turns of a 64 ms slice. Only 1 to 5 of them last a whole slice, against 12 or 13 of 13 on a fresh boot, and the longest lasts 69.0 to 70.0 ms, past the test's bound of the slice plus 4 ms. The checks "no turn runs past its slice" and "most turns are a whole slice" fail. It persists for every later `preempt.test` in that boot.

**Cause**
Not diagnosed. `tickreplace.test` moves CPU 3's tick through a refusing device, the HPET's broadcast group, a device that stops taking the one-shot mode, a flaky device and a fake global device, and puts it back on the CPU's own local APIC timer after each case. Its own checks pass, including that the CPU left the groups. Turns that end early mean something passes the preemption deadline before the slice is spent, or sets it too early. Turns that end late mean the device fires late. Candidates to rule out first:
- a broadcast group, the HPET's or the fake device's, still holding a deadline for CPU 3 and sending it timer IPIs;
- the local APIC timer re-taken by `replace_with()` in a state that differs from its boot state (mode, divider, minimum delta raised by the device layer's retries);
- `m_armed_deadline` of CPU 3's queue left naming a deadline the device does not hold.
Bisect by case: run `tickreplace.test` with all but one case skipped, then `preempt.test`.

**Change**
Find the cause first. The fix belongs in whichever of `TickDevice::replace_with()`, `TickBroadcast` or `TimerQueue` leaves the state behind.

**Reference**
None until the cause is known. Linux's `clockevents_exchange_device()` and `tick_setup_device()` are the counterpart of the swap.

**Acceptance**
`scripts/kshell_run.py --out out/bringup.x86 tickreplace.test preempt.test preempt.test` reports both `preempt.test` runs ok, with every turn a whole slice.
