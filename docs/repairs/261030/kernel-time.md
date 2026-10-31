# kernel-time outstanding repairs

## 1. A one-CPU boot logs an error-level line, so `SMP=1 boot_check.py` fails

**Severity**
Medium: not wrong on the default four-CPU machine, but `SMP=1` is the documented way to tell an SMP race from a logic bug (`ours-verify`), and it cannot be used while the boot reports an error.

**Symptom**
`SMP=1 python3 .agents/skills/ours-verify/scripts/boot_check.py --only dbg --no-tests` answers `FAIL error-level line (panic or failed ASSERT) · 1 error line(s) · reached: kernel`. The dbg log holds, in this order:

```text
CPU[0] tick device I8253 runs one-shot
CPU[0] tick device I8253 refused a deadline
CPU[0] tick device I8253 refused 3 deadlines in a row and is retired
CPU[0] tick device I8253 failed and no other device can serve it; the CPU has no tick
HPET Frequency = 100000000
CPU[0] tick moved from I8253 to HPET
```

So the boot goes on and the tick is recovered once the HPET is found; the error line is the whole failure. `scripts/kshell_run.py` with `SMP=1` fails the same way (its console goes quiet and the script sends an NMI). The same boot fails on another worktree's image (`feat-pmm-layering`, commit `e391b531`) that has none of the driver-capability changes, so it predates them.

**Cause**
Not diagnosed. What is known: with one CPU the first tick device chosen is the I8253, in one-shot mode, and it refuses its first deadline three times, before the HPET exists as a candidate (`pick_replacement()` in `kernel/kernel/time/tick_device.cpp` finds nothing, and logs at error level). With four CPUs the local APIC timer is the first device (`tick device LVT runs one-shot`) and this path is not taken. Why the I8253 refuses, and why a one-CPU machine does not start on the LVT timer, are the two questions.

**Change**
Find out which, then fix that one. Whatever the cause, a device that has no replacement yet because the platform has not found it is not a failure of the CPU's tick, and should not be an error-level line.

**Reference**
None, this project's own design.

**Acceptance**
`SMP=1 python3 .agents/skills/ours-verify/scripts/boot_check.py --only dbg --no-tests` passes, and `SMP=1 python3 scripts/kshell_run.py --out out/bringup.x86 vmopin.test` prints `vmopin.test: ok`.
