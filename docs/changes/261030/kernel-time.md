# kernel-time changes

## Title: arch_tick_hz() becomes ours::time::ticks_per_second(), one getter over a variable a once-only setter fills (2026-10-30)

**Files**
- `kernel/include/ours/time/ticks_per_second.hpp` — new: declares `ours::time::ticks_per_second()` and `ours::time::set_ticks_per_second(u64)`; the contract that used to sit on `arch_tick_hz()` is now the getter's comment
- `kernel/kernel/time/ticks_per_second.cpp` — new: the variable, a `constinit std::atomic<u64>`, the getter's relaxed load, and the setter, which `DEBUG_ASSERT`s that the rate is not 0 and that it was 0 before
- `kernel/kernel/time/BUILD.gn` — compiles `ticks_per_second.cpp`
- `kernel/arch/x86/apic/local_apic.cpp` — `apic_calibrate_clock()` calls `time::set_ticks_per_second(tsc_hz)` on the line after it stores `ApicTimerEvent::TSC_HZ`, with the same value
- `kernel/lib/logz4/kernel.cpp` — `tick_period()`, `panic_lock_ticks()` and `log_console_waits()` read `ticks_per_second()`; comments say "the counter's rate" instead of "the platform's"
- `kernel/lib/logz4/BUILD.gn` — the comment on `//kernel/platform:headers` names one platform call, `debug_console_write()`, not two
- `kernel/platform/include/ours/platform/timer.hpp` — `arch_tick_hz()` and its contract are gone
- `kernel/platform/pc/timer.cpp` — deleted: its only function forwarded `apic_tsc_hz()`
- `kernel/platform/pc/BUILD.gn` — no longer compiles `timer.cpp`
- `.agents/tasks/riscv-kernel/PLAN.md` — step 1b's list no longer asks riscv for an `arch_tick_hz()` body; it calls the setter from the device-tree reader

**Symptom**
Latent on x86, which logs and panics exactly as before (the panic lock bound is still 250 ms; see Verification). The problem was in what step 1b would have had to write. riscv's counter rate is a number the device tree holds (`timebase-frequency`, 10 MHz on QEMU's `virt`), so `arch_tick_hz()` for riscv would have been a function in `kernel/platform/riscv` returning a value that code does not own, and that someone has to store somewhere first.

**Cause**
The objects:
- `arch::Tick::get()`, the CPU's free-running counter (the TSC on x86, `rdtime` on riscv). A count of it is not a time until it is divided by the counter's rate.
- The rate, a fact that some layer learns exactly once. On a PC the boot CPU's local APIC timer calibration measures the TSC against the clock source (`apic_calibrate_clock()`, hooked at `InitLevel::Irq + 4`). On riscv the device tree states it. Nothing else knows it.
- The readers: the console logger on every CPU, for every line, in any context.

Why the old shape was a mistake. `arch_tick_hz()` made the *reader's* side a per-platform function and left the *writer's* side to whatever the platform did. On a PC that was a forward (`pc/timer.cpp` returned `apic_tsc_hz()`, which returned `ApicTimerEvent::TSC_HZ`), so three names carried one number. Every platform would have to repeat the same three things: keep the number somewhere, write a getter, and keep the getter honest to the contract (no lock, no CPU-local state, callable with interrupts off from an NMI before CPU-local storage exists). A contract that has to be re-satisfied in every platform's body is a contract that will be broken in one of them. And the body is trivial everywhere: it is always "return the variable the rate-learning code stored". The shape that does not repeat is one variable and one getter, written once in the layer every platform links, with the setter as the only per-platform call, made by whichever layer learns the rate.

The name was wrong in a second way. `arch_tick_hz` is declared in a *platform* header, and the repo spells its seam functions by prefix (`arch_*` is defined by `kernel/arch`, `platform_*` by `kernel/platform`), so the prefix sent a porter to the wrong directory. State that belongs to no architecture or platform in particular is named by its subsystem instead: `ours::time`.

The third thing is a lesson on data races, and it decides the variable's type. Who reads and who writes, and when:
- The writer is one call, on the boot CPU, in the APIC calibration at `InitLevel::Irq + 4`.
- The readers are the logger on every CPU, on every line, so also in NMI and panic context. No other CPU reads it during the write: none runs until the boot CPU has started it, which is after the calibration. But an NMI can land on the boot CPU between any two of its instructions, including in the middle of the store.
- The language does not care that both sides run on one CPU. A read in an interrupt handler of an object the interrupted code is writing is a data race on a plain `u64`, and a data race is undefined behaviour, not "probably a stale value". In practice the compiler may split a store it considers cheap to split, or keep an earlier load in a register across a loop (`while (ticks_per_second() == 0) {}`). The one object the language lets a handler read while the code it interrupted writes it is a lock-free atomic.

So the variable is a `std::atomic<u64>` that is only loaded and stored with `std::memory_order_relaxed`. Relaxed, because the rate publishes nothing else: a reader that sees a non-zero rate goes on to use that number alone, and a reader that sees 0 has an answer for that case, so no other write needs to become visible with it. The order the other CPUs rely on (the rate is set before they start) is made by the way the boot CPU starts them. A relaxed load or store of an aligned 64-bit atomic compiles to the same single instruction a plain `u64` gets, so the atomic costs nothing and says out loud that the variable is shared; the getter in the x86 dbg vmours is `movq TICKS_PER_SECOND(%rip), %rax` and `retq`. It is `constinit`, so it is in `.bss` before any static constructor, because the logger reads it from the first line of the boot.

**Change**
Before, in `kernel/platform/pc/timer.cpp`, `kernel/platform/include/ours/platform/timer.hpp` and `local_apic.cpp`:

```cpp
// timer.hpp, a platform seam: every platform defines it
[[nodiscard]]
auto arch_tick_hz() -> u64;

// pc/timer.cpp
auto arch_tick_hz() -> u64 {
    return apic_tsc_hz();
}

// local_apic.cpp
ApicTimerEvent::TSC_HZ = tsc_hz;
```

After:

```cpp
// kernel/include/ours/time/ticks_per_second.hpp, generic
[[nodiscard]]
auto ticks_per_second() -> u64;
auto set_ticks_per_second(u64 rate) -> void;

// kernel/kernel/time/ticks_per_second.cpp
constinit std::atomic<u64> TICKS_PER_SECOND{0};

auto ticks_per_second() -> u64 {
    return TICKS_PER_SECOND.load(std::memory_order_relaxed);
}

auto set_ticks_per_second(u64 const rate) -> void {
    DEBUG_ASSERT(rate != 0, "the counter's rate cannot be 0");
    [[maybe_unused]]
    auto const previous = TICKS_PER_SECOND.exchange(rate, std::memory_order_relaxed);
    DEBUG_ASSERT(previous == 0, "the counter's rate is already {} Hz; it is set once", previous);
}

// local_apic.cpp
ApicTimerEvent::TSC_HZ = tsc_hz;
time::set_ticks_per_second(tsc_hz);
```

The previous value is read by the same `exchange` that stores the new one, instead of an assertion on a load followed by a store, so a second call is caught even if two CPUs made it at once. A release build keeps the `exchange` (one `xchg`, once per boot).

The contract moved with the declaration: the getter's comment says it takes no lock, allocates nothing, reads no CPU-local state, is callable with interrupts off from an NMI or a panic before CPU-local storage exists, answers 0 until the rate is known, answers the same value afterwards, is known before any CPU but the boot CPU starts, and stays 0 for good on a PC whose APIC timer calibration failed (`apic_calibrate_clock()` returns before the setter call). The panic lock bound is untouched in meaning: `panic_lock_ticks()` is `ticks_per_second() / 1000 * 250`, so while the rate is 0 it is 0, `acquire()` tries the lock once and gives up, exactly as after `b62a424f`.

Rejected:
- An inline getter over an exported variable. The variable would have to be visible in the header, which gives every includer the right to write it and takes away the one place the once-only assertion lives. The getter is out of line, a call, a load and a return. Not measured: a logger line ends in a polled serial write that takes at least tens of microseconds, so a call is not what that line costs.
- A plain `u64`, as the reference has. Its readers are all after the write in practice, but "in practice" is exactly what an NMI on the boot CPU is not bound by.
- `volatile`. It stops the compiler caching the load, but it is not a synchronization object in C++; it does not make the race defined.
- Acquire/release. Nothing is published through the rate, so the extra barrier on arm and riscv would buy no ordering anyone uses.
- Making `apic_tsc_hz()` the setter's caller's source of truth (reading `TSC_HZ` back through the generic getter, or deleting `TSC_HZ`). The APIC code uses `TSC_HZ` and `APIC_HZ` as a pair when it picks its enrolment rate, and `timer_share_test` reads it; they stay x86's.

**Reference**
Zircon `ticks_per_second()` (`zircon/kernel/kernel/timer.cc:105`, declared in `zircon/kernel/include/platform/timer.h:157`) reads `gTicksPerSecond`, which `timer_set_ticks_to_time_ratio()` (`timer.cc:77`) stores after `DEBUG_ASSERT(gTicksPerSecond == 0)`. The platforms are the callers: `zircon/kernel/platform/pc/timer.cc:617` (TSC), `:651` (HPET), `zircon/kernel/arch/riscv64/timer.cc:31`, `zircon/kernel/dev/timer/arm_generic/arm_generic_timer.cc:597`. No platform writes a getter. Two differences from the reference: it stores a ratio and derives the rate from it, here only the rate is stored; and its variable is a plain `uint64_t`, here a relaxed atomic, for the NMI reader above.

**Verification**
- x86 `ours-verify`: build ok (out/bringup.x86, 6.2s); dbg PASS shell finished at 5.5s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.7s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 152 passed, 0 failed.
- riscv phys check (`boot_check.py --out out/bringup.riscv`): PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.
- `llvm-nm -C` of the x86 dbg vmours before: `ours::arch_tick_hz()` and `ours::apic_tsc_hz()`. After: no `arch_tick_hz`; `ours::time::ticks_per_second()` is present, out of line (a local `t` symbol), beside `ours::time::set_ticks_per_second(unsigned long)` and `ours::time::(anonymous namespace)::TICKS_PER_SECOND` in `.bss` (a `b` symbol, so it is zero-initialised and needs no constructor); `llvm-objdump -d` of the getter shows `movq 0x65071(%rip), %rax` and `retq`, a plain load; `ours::apic_tsc_hz()` stays.
- `kshell_run.py --out out/bringup.x86 "lockup status" "timershare.test"`: both ok. `lockup status` prints each CPU's console account in milliseconds ("cpu0 wrote 9357 lines; 56 found the lock taken and waited 1.791 ms in all ..."), which it can do only when the rate is non-zero, so the logger reads what the calibration set.
- `kshell_run.py --out out/bringup.x86 "crash panic-locked"`: ok. In the dbg log the last line before the panic is `[11091990000 cpu3 info] CPU[3] suspended` and the panic message is `[11342470000 cpu0 error] crash: deliberate panic from the kernel shell`: 250.5 ms later, the 250 ms bound as before.
- `format_cpp.py --check` clean on the five C++ files, `format_gn.py --check` clean on the three GN files, `clang-tidy -p out/bringup.x86` reports nothing on the lines changed in `ticks_per_second.cpp`, `logz4/kernel.cpp` and `local_apic.cpp`.

**Deliberately not done**
- `ApicTimerEvent::TSC_HZ` and `apic_tsc_hz()` stay. The APIC uses `TSC_HZ` to enrol its TSC-deadline device, and `kernel/commands/timer_share_test.cpp:293` reads `apic_tsc_hz()`, so the number lives in two places on x86 until the rate has more than one reader outside x86. They are written on adjacent lines with the same value.
- `Tick`'s field is still called `tsc`; that is the next commit.
- The two places that assume a 1 GHz counter (`lockup_detector/src/report.cpp`'s `ANSWER_WAIT_TICKS`, `pc/power.cpp`'s `SETTLE_TICKS`) are untouched; that is the commit after next.
- No riscv setter call. Step 1b writes it in the device-tree reader.

**How it was found**
None. The shape and the name were the user's decision of 2026-10-30, recorded in `ours-kernel-design` `references/task.md` (the rate of `arch::Tick`).

**The user's prediction**
None.

**Three questions you should be able to answer**
- An NMI lands on the boot CPU while it stores the rate and the NMI handler logs a line. Why is a plain `u64` undefined behaviour here even though only one CPU is involved, and what does the atomic change in the getter's generated code on x86 (look at the disassembly in Verification)?
- Why is `memory_order_relaxed` enough for the rate when other CPUs only start after it is set, and what would have to be true of the rate for a release store to be needed?
- Why is it better for the platform to call a setter than to define a getter, and what does riscv's step 1b now have to write?
