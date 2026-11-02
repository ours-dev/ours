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

## The monotonic anchor is published into the vDSO (2026-11-02)

**Files**
- `system/public/ours/time_values.hpp` — the page layout and `monotonic_from`, the same scale `ClockSource::ticks_to_time` uses
- `kernel/lib/userabi/vdso/vdso.S`, `kernel/lib/userabi/vdso/clock.cpp` — the page in read-only data, and `ours_clock_get_monotonic`, which scales a counter sample when the page says the counter is readable and otherwise traps
- `build/abi/gen_syscalls.py`, `system/public/ours/syscalls.gni` — the `vdso` attribute: the generated stub is the hidden `ours_clock_get_monotonic_via_kernel`
- `kernel/include/ours/time/anchor_page.hpp`, `kernel/kernel/time/anchor_page.cpp` — writes the steady clock's anchor into the page, sequence odd then even
- `kernel/kernel/time/clock.cpp`, `kernel/include/ours/time/clock.hpp`, `kernel/include/ours/time/clock_source.hpp`, `kernel/lib/mtl/include/mtl/ratio.hpp` — the reading that gets published, and the ratio's two halves
- `kernel/mm/vm_object_paged.cpp`, `kernel/mm/include/ours/mm/vm_object.hpp`, `kernel/mm/include/ours/mm/vm_object_paged.hpp`, `kernel/object/include/ours/object/vmo_dispatcher.hpp` — `store_u64`, one store for the sequence word, because `write()` copies a byte at a time
- `kernel/lib/userabi/src/userboot.cpp` — finds the page by its magic after laying the vDSO out, and binds it
- `kernel/lib/userabi/vdso/BUILD.gn` — the C++ body, and `-z norelro`, because an empty `.got` plus a RELRO pad was a writable segment
- `system/lib/time/test/read_test.cpp` — `monotonic_from` against the kernel scale, including a wrapped counter

**Symptom**
None, a new feature. The call trapped and returned `SteadyClock::now()`. Nothing in the vDSO carried the anchor a later non-trapping read scales.

**Cause**
None, a new feature. A stored nanosecond goes stale between writes, and this kernel has no periodic tick to hang a write on. The anchor is already what `Clock::now` scales, and it changes only when the source changes, so publishing that is what keeps a read in the vDSO equal to a read in the kernel.

**Change**
The page is 48 bytes in the vDSO's read-only data, magic `0x0C10C1A11E000001`, then a sequence, the anchor ticks, the anchor nanoseconds, the ratio, and `accessible`. `lay_out_vdso` scans the VMO for the magic, because the packaged image is stripped. `SteadyClock::notify` and the bind both publish. The publish stores an odd sequence, the other words, then an even sequence. `accessible` is written 0.

```text
read:
  seq1 = load(seq)          # acquire
  if seq1 is odd: retry
  copy the anchor
  seq2 = load(seq)
  if seq1 != seq2: retry
  if accessible == 0: trap to SteadyClock::now()
  else: ns = anchor_ns + scale_up(counter - anchor_ticks, numerator, denominator)
```

A sum that does not fit in an `Instant` saturates at its maximum. The counter subtract is unsigned, so a wrap is a large delta and saturates rather than going backwards.

Ruled out: a C++ definition of the page in the same file as the read. The compiler could then treat `accessible` as the 0 it initialized, and the load would not be a load. The bytes are assembly, and the C++ file only has an extern declaration.

**Reference**
Zircon `compute_monotonic_time` and `VDso::SetMonotonicTicksOffset` (`zircon/kernel/lib/fasttime/include/lib/fasttime/internal/time.h`, `zircon/kernel/lib/userabi/vdso.cc`). Linux `__cvdso_clock_gettime` / `vdso_calc_ns` (`lib/vdso/gettimeofday.c`) is the same shape with a `(mult, shift)` pair, which this clock does not use: `ClockSource::ticks_to_time` scales with a ratio through a 96-bit intermediate so the anchor does not have to be refreshed on a tick.

**Verification**
`system/lib/time/test:read` exits 0. `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/clock-mono.x86` passed: dbg and rel both reached the end, both logs contain `shell: clock checks passed`, host tests 152 passed. clang-tidy on `clock.cpp`, `anchor_page.cpp` and `read_test.cpp` reported nothing on the new lines after the one naming suppression on the assembly symbol. The first boot failed with `userabi: the vDSO has a writable segment at +0x3000`; `-z norelro` removed that segment and the second boot passed.

**Deliberately not done**
`accessible` stays 0, so every read on these images still traps. Turning it on means enrolling an invariant TSC as the steady clock's source, which changes every `now()` in the kernel, and QEMU's TCG does not report one. `deadline_after`, `nanosleep` and a finite deadline on `object_wait_one` are still separate calls.

**How it was found**
None.

**The user's prediction**
The user asked for a timeline variable in the vDSO, loaded volatile. The page that landed is the anchor, not a nanosecond. On the machines this check boots the flag is off and the read traps, which is what `shell: clock checks passed` shows: the value moves, and it moves because of the trap.

**Three questions you should be able to answer**
- Why does the reader retry when the sequence is odd, or when the two loads differ?
- Why would scaling `rdtsc` by an anchor pinned to the HPET not be a time?
- Why is the sequence word stored with `store_u64` rather than `write()`?
