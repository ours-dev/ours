# kernel-time changes

## The fallback to the periodic tick has a test, and is a warning (2026-10-21)

**Files**
- `kernel/commands/tick_fallback_test.cpp` — `tickfallback.test`; `kernel/commands/BUILD.gn` lists it
- `kernel/kernel/time/tick_device.cpp` — the log line of the fallback in `rebind_device()` is a warning, not an error
- `docs/repairs/261010/kernel-time.md` — item 3 is removed, as fixed
- `.agents/tasks/high-res-timers/STATUS.md` — the open list

**Symptom**
Latent, cannot be triggered with the devices in the tree: the local APIC timer always accepts one-shot, so the branch of `rebind_device()` that handles a device that claims Oneshot and will not enter it never ran. Nothing said that a CPU in that case keeps a tick, or that `m_mode` stays Periodic.

**Cause**
The branch needs a device that advertises `Feature::Oneshot` and fails `switch_mode(Oneshot)`, and no real device does. A path that has never run is a path whose order of operations nobody has looked at: the one that matters here is `setup_oneshot_tick()`, which sets the handler, asks the device for Oneshot, and only then writes `m_mode = Oneshot`. Writing the mode first would leave the CPU believing it is one-shot while its device runs a periodic tick or none, and the timer queue would then stop being run from `on_tick()`, which follows the mode. A CPU in that state has no timers at all.

The line that reports the fallback was also at error level, and `scripts/kshell_run.py` fails a command that prints an error line, so no test of the fallback could pass. The fallback is the CPU carrying on with a coarser tick; it is a warning.

**Change**
`tickfallback.test` builds a `TickDevice` of its own on the stack and offers it a fake device that refuses Oneshot (and runs Periodic):

```cpp
TickDevice own;
offered = own.check_new_device(&fake.device());
still_periodic = own.mode() == TickDevice::Mode::Periodic;
```

It is safe on a running kernel for three reasons. A fresh `TickDevice` points at the placeholder device and starts Periodic, so the first real device is taken. The refusal returns from `setup_oneshot_tick()` before the timer queue or the tick is touched, and the periodic setup that follows touches only the fake. And the fake is not enrolled: `ClockEventDevice::enroll()` would offer it to the CPU's own `TickDevice`, which would take it, rated as it is above the local APIC timer.

Checks: the device is taken (`Ok`); the tick device points at it; the mode is still Periodic; the device runs Periodic with a handler on it; it was asked for Oneshot once. With `kernel.time.oneshot` off it prints a skip line and passes, since the fallback is then never entered.

**Reference**
Linux `tick_switch_to_oneshot()` (`kernel/time/tick-oneshot.c`) refuses before it changes anything when the device lacks `CLOCK_EVT_FEAT_ONESHOT` or is not functional (`-EINVAL`, mode untouched). It then writes `td->mode` before `clockevents_switch_state()` and does not look at the result of the switch. So Linux decides on what the device claims; this tree decides on what the device answered, and commits the mode after it, which is stricter and is what the test holds to. `docs/repairs/261010/kernel-time.md` had cited Linux for the stricter behaviour; it was the capability check only.

**Verification**
- `tickfallback.test` on the default build (one-shot): 6 checks ok. The log shows `could not run oneshot-refusing one-shot (NotSupported), keeping the periodic tick` as a warning.
- Mutation, not committed: `m_mode = Mode::Oneshot` moved above `switch_mode(Oneshot)` in `setup_oneshot_tick()`. The test fails exactly the check "the mode is still Periodic" (1 of 6) and the command reports FAILED; with the line restored it passes again. This is the acceptance of the repair item: the test fails if the mode is committed before the device accepted.
- On a build with `time_oneshot=false`: the skip line, ok. Both images of that build boot.
- `ours-verify` on the default build: dbg PASS (39 passed, 4 skipped, 0 failed, 0 error lines), rel PASS (37 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines); host-tests PASS (145). `tickreplace.test`, `tickbcast.test`, `timer.test` and `sleep.test`: ok.
- `scripts/format_cpp.py`, `scripts/format_gn.py`, `clang-tidy -p out/bringup.x86` on the new file and `tick_device.cpp`: no diagnostic in them.
- Not verified: a real device that refuses Oneshot, and the CPU that has no device at all (item 6).

**Deliberately not done**
- A case where the device also refuses Periodic. `rebind_device()` then returns the error and leaves the CPU bound to a device that is not in service; whether that should be handled is item 6's question, a design decision for the user.
- Offering the fake to the CPU's real `TickDevice`. It would exercise `replace_with()`, which `tickreplace.test` does.

**How it was found**
Picked from the open repairs: a path with no test and a known order-of-operations hazard. The error-level line was found by running the test.

**The user's prediction**
Not given.

**Three questions you should be able to answer**
1. Why does writing `m_mode` before the device accepts Oneshot leave the CPU without timers, and not only with a wrong tick?
2. Why is the test's `TickDevice` safe to use on a running CPU, and what would go wrong if the fake were enrolled?
3. What does Linux's `tick_switch_to_oneshot()` check, and what does it not?
