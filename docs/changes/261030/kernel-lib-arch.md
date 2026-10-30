# kernel-lib-arch changes

## Title: arch::Tick's field is called count, not tsc (2026-10-30)

**Files**
- `kernel/lib/arch/include/arch/tick.hpp` — the generic `Tick` declares `usize count`, with a comment that says what the number is on each architecture
- `kernel/arch/x86/lib/arch/include/arch/x86/tick.hpp`, `kernel/arch/riscv/lib/arch/include/arch/riscv/tick.hpp`, `kernel/arch/arm/lib/arch/include/arch/arm/tick.hpp` — `Tick::get()` returns `{.count = ...}`
- `kernel/arch/x86/apic/local_apic.cpp` — three reads: the TSC-deadline programming and the two stamps of the calibration window
- `kernel/commands/timer_share_test.cpp` — eight reads
- `kernel/lib/lockup_detector/src/report.cpp` — two reads, the wait for the targets' answers
- `kernel/lib/logz4/kernel.cpp` — seven reads: the logger's clock, its write and wait accounting, and panic mode's wait for the lock
- `kernel/phys/physboot.cpp` — one read, the entropy seed
- `kernel/platform/pc/power.cpp` — two reads, the wait for a reset to take effect
- `kernel/top/crash_record.cpp` — one read, the stamp that seals the crash record
- `docs/architecture/timer/oneshot-interrupt-cost.md` — its method paragraph names the field

**Symptom**
Latent. Every user of the generic type read `.tsc`, and on riscv and arm that was `rdtime` and `cntpct_el0`, not a TSC. The 28 uses were all correct and all misleading.

**Cause**
`arch::Tick` was written on x86 first, and its one field took the name of x86's counter. riscv and arm then filled it (`return {.tsc = count};`, where the local variable one line above is already called `count`), because the type was generic and the name was not worth a change at the time.

A name is a claim, and this one made two false ones. In generic code it said "this is an x86 counter": a reader of the logger's accounting, the lockup detector's wait or the crash record's seal has to translate it, and a riscv porter reads `tsc` as an x86 thing the port has to supply or avoid. And it spent the word: `git grep -w tsc` should find the places that really are about the x86 TSC (`rdtsc`, `set_tsc_deadline()`, `apic_tsc_hz()`, `ApicTimerEvent::TSC_HZ`), and found 28 uses that were not, buried among them. A field of a generic type takes the generic meaning: the counter's raw count, in counts at `ours::time::ticks_per_second()`.

**Change**
Before and after, the generic header and x86's body:

```cpp
// arch/tick.hpp, before
struct Tick final {
    usize tsc;
    // ...

// arch/tick.hpp, after
struct Tick final {
    /// The counter's raw value: x86's TSC, riscv's `time` CSR, arm's
    /// physical counter.
    usize count;
    // ...

// arch/x86/tick.hpp, before
return {.tsc = (static_cast<usize>(high) << 32) | low};

// after
return {.count = (static_cast<usize>(high) << 32) | low};
```

Every use is `Tick::get().tsc` to `Tick::get().count` (and `now.tsc` to `now.count` in `local_apic.cpp`); no logic changed, and no line was long enough to need re-wrapping, so no other line moved. x86 names that really are about the TSC keep their names, and so does `timer_share_test.cpp`'s own vocabulary (`cost_tsc`, `busy_tsc`, `read_tsc`, `give_up_tsc`, `run->tsc_hz`, `ns_of_tsc()`'s parameter): that test runs only on x86 and measures the TSC.

**Reference**
Zircon keeps the field arch-specific on the early-boot sample, `arch::EarlyTicks` (`zircon/kernel/lib/arch/x86/include/lib/arch/ticks.h:20` has `tsc`, `zircon/kernel/lib/arch/riscv64/include/lib/arch/ticks.h:21` has `time`, `zircon/kernel/lib/arch/arm64/include/lib/arch/ticks.h:20` has `cntpct_el0` and `cntvct_el0`), and no generic code reads those fields: it goes through `current_mono_ticks()` and `ticks_per_second()` with the generic `zx_ticks_t`. Here one `Tick` serves every architecture and generic code (logz4, the lockup detector) reads the raw count itself, so the field takes a generic name, `count`; this is the user's decision of 2026-10-30, recorded in `ours-kernel-design` `references/task.md`.

**Verification**
- `git grep -n -w tsc -- '*.cpp' '*.hpp'` finds only `ns_of_tsc()`'s own parameter in `timer_share_test.cpp`, a local name.
- Pure rename, so the code must not change. Method: the build directory held the build of the parent (`58383d44`), and its two kernel images were copied aside before the edit. After the edit, `llvm-objcopy -O binary` (every loadable byte) and `llvm-objcopy --dump-section .text` of both images, sha256 of each. The dbg `vmours` (`kernel_x86.dbg.1`): `.text` 870,332 bytes, sha256 `034d60c3a48a5b3459d2b277fbd5570a226fcb0358cdce81e689e675dda996ea` before and after. `vmours.with-tests` (`kernel_x86.dbg.0`): `.text` 748,436 bytes, sha256 `29428d94f803dfd5ea5e03f6f8a1c181483011947f10fe78ce1312a9806dd2da` before and after. The loadable bytes of the two images differ in exactly 20 bytes each (of 1,696,104 and 1,532,264): offsets `0x7fa8` to `0x7fbb` and `0x7b70` to `0x7b83`, which are the `NT_GNU_BUILD_ID` descriptor of `.note.gnu.build-id` (`7f98 + 16` and `7b60 + 16`), a hash of the file's contents, debug info included, and so different once a field's name in the DWARF is.
- x86 `ours-verify`: build ok (out/bringup.x86, 2.3s); dbg PASS shell finished at 7.2s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 5.2s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 152 passed, 0 failed.
- riscv phys check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi; `physboot.cpp.o` was rebuilt, so riscv's `tick.hpp` compiled.
- arm is not built in this tree, so `arm/tick.hpp` was syntax-checked alone: `clang++ --target=aarch64-none-elf -std=c++23 -ffreestanding -fsyntax-only -DARCH_ARM=1` over a one-line user of `Tick::get().count`, with a stub `arch/types.hpp`; ok. The same with `--target=riscv64-none-elf -DARCH_RISCV=1`; ok.
- `kshell_run.py --out out/bringup.x86 "lockup status" "timershare.test" "timer.test"`: all ok (`timershare.test: ok -- 3 runs`, `timer.test: ok -- 18 checks`); `"crash panic-locked"`: ok.
- `format_cpp.py --check` clean on all eleven C++ files; `clang-tidy -p out/bringup.x86` reports nothing in the six that the build compiles (`report.cpp`, `timer_share_test.cpp`, `power.cpp`, `crash_record.cpp`, `logz4/kernel.cpp`, `local_apic.cpp`).

**Deliberately not done**
- x86's TSC names (`rdtsc`, `set_tsc_deadline()`, `apic_tsc_hz()`, `ApicTimerEvent::TSC_HZ`) and `timer_share_test.cpp`'s are not touched; they are about the TSC.
- `ANSWER_WAIT_TICKS` and `SETTLE_TICKS` still count in `arch::Tick` counts at an assumed 1 GHz; that is the next commit.
- No accessor or unit type for `count`. A bare `usize` is what 28 uses subtract and compare today; a typed count is a design question of its own.

**How it was found**
None. The rename was the user's decision of 2026-10-30; the stale name was found while recording `arch::Tick`'s rate (`arch-platform-seams` I5).

**The user's prediction**
None.

**Three questions you should be able to answer**
- A rename touched 28 lines in 11 files. What would a changed byte in `.text` have meant, and why is a differing build-id not one?
- Why does Zircon get away with an arch-specific field name on `arch::EarlyTicks`, and what is different about the readers of `arch::Tick` here?
- What does `git grep -w tsc` now find, and why is that the point of the rename?
