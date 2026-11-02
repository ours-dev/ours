# clock changes

## Title: clock_get_monotonic, the kernel's steady clock handed to user space (2026-11-02)

**Files**
- `system/public/ours/syscalls.gni` — `clock_get_monotonic(out_now: Instant) -> Status`, appended (new highest call number, private numbering keeps both sides in step).
- `kernel/lib/syscall/src/clock.cpp` — the handler: `SteadyClock::now()` through `user_copy` into the caller's Instant. No resource, no arguments, no failure path past the copy.
- `kernel/lib/syscall/BUILD.gn` — src/clock.cpp into the syscall sources.
- `system/public/ours/syscalls.gni` (doc lines) — `object_wait_one`/`port_wait` deadline notes reworded: waits cannot time out yet (the missing piece is timer-driven wakeup; the phrase "until a program can read a clock" outlived its truth the moment this call landed).
- `system/service/init/test/routing_checks/` — check_clock: two reads, non-decreasing, nonzero.

**Symptom**
None — an enabling capability. No user-space-visible time source existed: 273 syscalls and not one handed the clock over; wait deadlines beyond 0/infinite answered NotSupported; the fs benchmark this branch needs next had no way to time an operation.

**Cause**
The vDSO's own doc said it twice: "NotSupported until a program can read a clock." The kernel's steady clock has been running since early boot (the scheduler, the lockup detector and every serial-log timestamp read it), so the missing piece was never a time source — it was the exit door to user space.

**Change**
The syscall reads `time::SteadyClock::now()` and copies the count (Instant, i64 nanoseconds since the clock's epoch) out. Zircon's counterpart is `zx_clock_get_monotonic()`, which returns the value as the result rather than through an out parameter; ours goes through an out Instant because every call in this table answers Status and the generator's shim shape is built around that. The deliberately delayed parts: a vDSO data-page clock (reads without a syscall trap — needed only when timing something cheaper than the read itself) and deadline-driven waits (needs timer-driven wakeup, a different half).

**Reference**
`zx_clock_get_monotonic()` — nanoseconds since boot, monotonic, no resource. The narrow first version of it: no vDSO fast path yet.

**Verification**
`boot_check.py`: dbg PASS (44 passed, 0 failed — check_clock's five expectations among them), rel PASS (42 passed, 8 skipped, 0 failed, 6/6 required), host-tests PASS 158. Disked boot: `routing_checks: passed`. The syscall is picked up by the generated pipeline end to end: kernel shim, vDSO stub, `ours::clock_get_monotonic` in the vDSO declarations — user code calls it with no manual wiring anywhere.

**Deliberately not done**
- The vDSO data-page fast path: the syscall round trip is fine while the callers time batches; it becomes the work item when something needs cheaper reads.
- Deadline waits (`object_wait_one`/`port_wait` with a real deadline): the clock exists now, but timing out a wait needs timer-driven wakeup — the high-res-timers territory, its own change.
- getrtc/wall-clock: the steady clock is the only promise; time of day is nobody's requirement yet.

**How it was found**
The fs benchmark step opened with a survey of the 273 syscalls: nothing handed time over, and the vDSO doc named the gap as deliberate.

**The user's prediction**
None. The user asked what capabilities were missing, received the list, and said continue.

**Three questions you should be able to answer**
- Why is the clock read resourceless where `interrupt_create` needs the IRQ root -- what makes a capability requirement wrong for this call?
- What does the syscall round trip cost that a vDSO data-page read would not, and why is that acceptable for batch timing but not for sub-microsecond per-op timing?
- The old deadline doc said timeouts were blocked "until a program can read a clock" -- why is that still true after this call, and what is the missing half?
