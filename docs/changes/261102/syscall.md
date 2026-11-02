# syscall changes

## clock_get_monotonic returns the steady clock through a trap (2026-11-02)

**Files**
- `system/public/ours/syscalls.gni` — one entry appended, so it takes the next call number; the stub, the declaration and the shim are generated from it
- `kernel/object/include/ours/object/clock.hpp`, `kernel/object/clock.cpp` — `object::monotonic_now()`, the one reading a program and the kernel share
- `kernel/lib/syscall/src/time.cpp` — `syscall::clock_get_monotonic()`, which returns that reading
- `kernel/lib/syscall/include/syscall/handlers.hpp` — the comment that said every handler returns `Status`
- `kernel/lib/syscall/BUILD.gn`, `kernel/object/BUILD.gn` — the new sources
- `kernel/object/include/ours/object/wait.hpp` — a finite deadline is still `NotSupported`, and the comment no longer says a program cannot read the clock
- `system/app/shell/shell_tests.cpp` — `check_clock()`, run first on both images
- `docs/repairs/261005/pc-ps2.md` — item 2's cause no longer says there is no call that reads a clock; the missing piece there is still a sleep

**Symptom**
None, a new feature. No program could read the monotonic clock. `object_wait_one` and `port_wait` therefore accept only `0` and `INSTANT_INFINITE` as a deadline.

**Cause**
None, a new feature. The call was specified as `Instant ours_clock_get_monotonic()` in `docs/design/260923/userspace-interface.md` (section 2.5), returning `SteadyClock::now()` through an object-layer facade. A stored nanosecond in the vDSO was the other shape considered, and it was rejected for this slice: the dev images are one-shot, so nothing ticks that a writer could ride, and a value written on its own timer is stale by up to that period and disagrees with the clock the kernel's timers read. The reading a program gets has to be the reading `SteadyClock::now()` computes.

**Change**
The handler does not name a clock. It asks the object layer, which is the layer a system call is allowed to call:

```cpp
auto clock_get_monotonic() -> Instant {
    return object::monotonic_now();
}

auto monotonic_now() -> Instant {
    return time::SteadyClock::now().time_since_epoch().count();
}
```

`Instant` is signed nanoseconds (`system/public/ours/types.hpp`). The generated shim stores it in `frame->ax`, and the exit path pops the whole `rax`, so the value is not cut to 32 bits. The vDSO entry is the ordinary trap stub. The shell reads it twice, with at most a thousand calls between them, and fails the boot unless the first reading is past the epoch and the second is later.

Ruled out for this change: publishing `Clock`'s anchor into a vDSO data page and computing the time in the vDSO from a counter read. That is how the value stays current without a periodic writer, and it is the next slice. It cannot be this one, because the only enrolled clock source is the HPET and ring 3 cannot read it.

**Reference**
Zircon `sys_clock_get_monotonic_via_kernel()` (`zircon/kernel/lib/syscalls/clock.rs`), which returns `current_mono_time()`. The fast path that does not trap, `compute_monotonic_time()` over `fasttime::internal::TimeValues` (`zircon/kernel/lib/fasttime/include/lib/fasttime/internal/time.h`), is the slice not taken here.

**Verification**
`python3 scripts/format_cpp.py --check` was clean on the touched C++ files, and `python3 scripts/format_gn.py` reported both `BUILD.gn` files already formatted. clang-tidy against `out/clock-mono.x86` reported nothing on the new lines of `clock.cpp`, `time.cpp` and `shell_tests.cpp`. `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/clock-mono.x86` passed: dbg and rel both reached the end with no error line, host tests 151 passed, and both serial logs contain `shell: clock checks passed`.

**Deliberately not done**
The vDSO data page, the published anchor and enrolling an invariant TSC as a `ClockSource`. Ring 3 cannot sample the HPET, and QEMU's TCG does not report an invariant TSC, so the non-trapping read has no counter to sample on the machines this check boots. `deadline_after`, `nanosleep` and a finite deadline on `object_wait_one` are separate calls. A kernel timer that stores a nanosecond into the vDSO was rejected: on a one-shot build it is a new tick, and the stored word lags `SteadyClock::now()`.

**How it was found**
None.

**The user's prediction**
None. The user approved this slice: the trap that returns `SteadyClock::now()`, with the anchor page and the TSC source decided separately.

**Three questions you should be able to answer**
- Why does a nanosecond stored in the vDSO and rewritten on a timer disagree with a deadline the kernel arms from `SteadyClock::now()`?
- Why is the non-trapping read impossible while the steady clock's source is the HPET?
- Why does `syscall::clock_get_monotonic` call `object::monotonic_now()` instead of `SteadyClock::now()`?
