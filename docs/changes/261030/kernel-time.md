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

## Title: The lockup reporter's wait and the PC reset's settle time are durations, not counts that assume a 1 GHz counter (2026-10-30)

**Files**
- `kernel/lib/lockup_detector/src/report.cpp` — `ANSWER_WAIT_TICKS = u64{2'000'000'000}` becomes `ANSWER_WAIT = ours::Milliseconds(2'000)`; `answer_wait_ticks()` turns it into `arch::Tick` counts with `ours::time::ticks_per_second()`, and `wait_for_answers()` computes that bound once before it spins; the file comment and the function comment name `ANSWER_WAIT`
- `kernel/platform/pc/power.cpp` — `SETTLE_TICKS = usize{100'000'000}` becomes `SETTLE_TIME = Milliseconds(100)`, converted by `settle_ticks()` the same way

**Symptom**
Nothing on QEMU's x86, whose TSC runs at 1 GHz: the bounds are the same counts as before (checked below). Anywhere else the same count is a different time. The lockup reporter's wait was 2 s at 1 GHz, 0.4 s at a 5 GHz TSC (its own comment says so), and would be 200 s on QEMU's RISC-V board, whose counter runs at 10 MHz: a CPU that never answers its NMI would hold the report, and so the panic, for more than three minutes. The reset's settle time was 100 ms at 1 GHz and 20 ms at 5 GHz (its comment: "between 20 and 100 ms on anything this runs on"), so on a fast machine the next reset method was tried five times sooner than on QEMU.

**Cause**
A count of `arch::Tick` is a time only on the counter it was measured on, and both constants were written on the one counter anyone had: the 1 GHz TSC QEMU gives the guest. Each comment even did the conversion for two machines, "2 s ... 0.4 s at a 5 GHz one" and "between 20 and 100 ms", which documents a fivefold spread as if it were a property of the design. What these two numbers mean is a duration with a reason behind it: the reporter waits for CPUs that will never answer, so it needs a bound that is long on the scale of an NMI (microseconds) and short on the scale of a person waiting for a panic; the reset needs the time a chipset takes to act on a request. Neither reason has a unit of counter in it.

The same mistake was made, and fixed, for logz4's panic lock bound in `b62a424f`: `PANIC_LOCK_TICKS` became `PANIC_LOCK_WAIT`, an `ours::Milliseconds`. These two were left because they had no answer for a rate that is not known. They have one now, since `ours::time::ticks_per_second()` exists, and the answer differs from logz4's on purpose. logz4 gives up at once while the rate is 0, because then only the boot CPU runs and a lock it finds held is its own, so no wait can help. Here a wait is the whole point: the reporter waits for other CPUs and the reset waits for a chipset. The one machine where the rate stays 0 with every CPU running is a PC whose local APIC timer calibration failed, and there the old count is the best guess anybody had, so the rate is taken to be 1 GHz: that PC waits exactly as long as it did before.

**Change**
Before and after, `report.cpp`:

```cpp
// before
constexpr auto ANSWER_WAIT_TICKS = u64{2'000'000'000};
// ...
while (!all_answered(targets) && arch::Tick::get().count - start < ANSWER_WAIT_TICKS) {

// after
constexpr auto ANSWER_WAIT = ours::Milliseconds(2'000);
constexpr auto ASSUMED_TICKS_PER_SECOND = u64{1'000'000'000};

auto answer_wait_ticks() -> u64 {
    auto const milliseconds = static_cast<u64>(ANSWER_WAIT.count());
    auto const rate = ours::time::ticks_per_second();
    if (rate == 0) {
        return ASSUMED_TICKS_PER_SECOND / 1000 * milliseconds;
    }
    return rate / 1000 * milliseconds;
}
// ...
auto const bound = answer_wait_ticks();
while (!all_answered(targets) && arch::Tick::get().count - start < bound) {
```

`power.cpp` has the same shape with `SETTLE_TIME` and `settle_ticks()`. The division by 1000 comes first, the order `panic_lock_ticks()` uses: it drops at most 999 counts of a rate that is itself a measurement, and the product cannot overflow whatever the duration. The durations are the old counts at 1 GHz, 2 s and 100 ms, the values verified under QEMU: of the ranges the old comments gave (0.4 to 2 s, 20 to 100 ms) they are the longer end, which is the safe side for both (a settle cut short tries the next method on a machine that was about to reset; a report wait cut short calls a live CPU silent).

Rejected: one shared helper that turns a duration into counts. Three places convert now (logz4's, this one, power.cpp's) and they disagree about rate 0, so a helper would take the policy as a parameter and read worse than three lines each; not decided here. Also rejected: giving up at once at rate 0, as logz4 does, which would make the reporter print "did not answer" for CPUs it never waited for on a PC with a failed calibration.

**Reference**
Zircon's lockup detector keeps its thresholds as durations (`ZX_MSEC(...)`, `zircon/kernel/lib/lockup_detector/lockup_detector.cc:116-118`) and turns them into counts where it compares, with `DurationToTicks()` (`lockup_detector.cc:64`, `timer_get_ticks_to_time_ratio().Inverse().Scale(duration)`). It has no case of an unknown rate, because the timer layer sets it before anything else runs; the 1 GHz fallback is this project's own, for the PC whose calibration failed. Zircon's `reboot()` (`zircon/kernel/platform/pc/power.cc:43`) falls through a similar sequence of methods (the microarchitecture's reset, then the keyboard controller) with no time-based settle of its own, so the settle time has no counterpart to follow.

**Verification**
- x86 `ours-verify`: build ok (out/bringup.x86, 0.6s); dbg PASS shell finished at 5.5s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.7s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 152 passed, 0 failed.
- riscv phys check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.
- The bounds, read off a dbg build with one temporary `log::info` in each function (removed before the commit; `git grep` finds none): `lockup nmi` printed `answer wait 2000000000 counts at 1000000000 Hz`, the old constant exactly, and `crash panic` (which ends in `platform_reboot()`) printed `settle 100000500 counts at 1000005000 Hz`, 100 ms of that boot's calibrated TSC. With the rate forced to 0 in the same build, the two printed `2000000000` and `100000000`, the old constants: so the failed-calibration PC waits as before. The forced-0 build was not committed either.
- `kshell_run.py --out out/bringup.x86`: `"lockup status" "lockup spin 1 200" "timershare.test"` all ok; `"lockup nmi"`, `"lockup spin 2"`, `"lockup spin all"`, `"crash panic-locked"` and `"crash panic"` each ok (one report or panic banner each). None of them exercises the bound itself, since every CPU answers its NMI; the bound is read off as above, not timed.
- `llvm-nm -C` of the x86 dbg vmours: no `arch_tick_hz`; `ours::time::ticks_per_second()` present, out of line; the file-local `settle()` is still there.
- `format_cpp.py --check` clean on both files; `clang-tidy -p out/bringup.x86` reports nothing in either.

**Deliberately not done**
- The durations are the 1 GHz values. Whether a real 5 GHz machine should now wait 2 s instead of 0.4 s for a CPU that never answers, and 100 ms instead of 20 ms between reset methods, is a decision about real hardware this tree has not run on; the numbers are the ones that were tested.
- No shared duration-to-counts helper, no change to logz4's rate-0 behaviour (see above).
- `ASSUMED_TICKS_PER_SECOND` is written twice, once per file, because a shared one would have to live in the time subsystem and say "assume" for everyone.

**How it was found**
`arch-platform-seams` I22, "Generic code assumes a GHz counter", which counted riscv's cost at the lockup reporter: 200 s at the `virt` board's 10 MHz.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does logz4's panic lock give up at once while the rate is 0, and these two waits assume 1 GHz instead?
- What did the reporter's wait come to on a 10 MHz counter, and what would that have done to a panic on riscv?
- On which machines do the two bounds now differ from the old constants, and in which direction?

## Title: set_ticks_per_second() keeps the first rate in every build, the variable is checked to be lock-free, and its comments say what it is made of (2026-10-30)

**Files**
- `kernel/kernel/time/ticks_per_second.cpp` — the setter's `exchange` becomes a `compare_exchange_strong` from 0, so the first rate stays in every build; a `static_assert` that `std::atomic<u64>` is always lock-free; the variable's comment is rewritten to say "loaded, and set once with a compare-exchange" and to give the real reason a plain `u64` is wrong
- `kernel/include/ours/time/ticks_per_second.hpp` — the setter's comment says a debug build asserts on a second call and any build keeps the first rate

**Symptom**
Latent. Nothing calls the setter twice. Three things were wrong in what `58383d44` said and promised:
- The header says "once it answers non-zero the answer does not change", and the setter did not keep that promise in a release build. `DEBUG_ASSERT(previous == 0, ...)` compiles out there, and the `exchange` before it stores unconditionally, so a second call would replace the rate under readers that had already used the first.
- The variable's comment, and the earlier record's sentence "a `std::atomic<u64>` that is only loaded and stored", described the getter and not the setter. The setter is a read-modify-write, `xchgq` on x86, which is locked and a full barrier. The earlier record explained the exchange two paragraphs later, so it contradicted itself.
- The comment said the compiler "may split the store". That is overstated on the targets this runs on.

**Cause**
The assertion was doing a job the code should have done. A `DEBUG_ASSERT` is a statement about what the author believes cannot happen; it is not a behaviour. When a contract is written in the header ("does not change"), the code has to keep it in the build that ships, and an assertion can only report a break in the build that has it. `exchange` stores whatever it is given, so the contract held only if no one ever broke it, which is the case the contract exists for. A compare-exchange from 0 stores only if the variable is still 0, so a later call changes nothing whatever the build: the property is in the operation, and the assertion becomes what it should be, a loud report in debug builds that someone called twice.

The strong form is not a style choice. `compare_exchange_weak` is allowed to fail spuriously while the variable still holds the expected value; on riscv and arm, where an atomic read-modify-write is a load-reserved/store-conditional pair, it does fail when the reservation is lost (an interrupt between the two). A first call that failed that way would leave the rate at 0 and trip the assertion, in a debug build, or in a release build silently leave the rate unset for good. Weak is for retry loops; this is not one.

On "the compiler may split the store". On x86-64, riscv64 and arm64 an aligned 64-bit store is one instruction, and no compiler has a reason to split it. What is actually wrong with a plain `u64` is formal: a handler reading an object the interrupted code is writing is a data race, and a data race is undefined behaviour ([intro.races]) whatever the hardware does with the bytes. The practical consequence is not a torn value but the optimiser's licence: with no atomic, it may assume nothing else writes the object while this code reads it, so it may keep an earlier load in a register across `while (ticks_per_second() == 0) {}`. A torn store is real only on a target with no native 64-bit store (32-bit arm), which this does not run on. The conclusion, use an atomic, was right; the reason given for it was half wrong.

The `static_assert` states the other premise of the NMI argument. "A lock-free atomic is the one object the language lets a handler read while the code it interrupted writes it" holds only for a lock-free atomic. A `std::atomic<u64>` on a target without 64-bit atomics is emulated with a lock the compiler's runtime library keeps, and then an NMI that lands inside the writer finds that lock held by its own CPU: exactly the deadlock the atomic was chosen to avoid, and only visible as a hang in an NMI. A premise that costs one line to check at compile time should be checked.

Also corrected here: the earlier record said a release build "keeps the `exchange` (one `xchg`, once per boot)". It keeps a `lock cmpxchgq` now; it is still one locked instruction, once per boot, so the cost argument is unchanged.

**Change**
Before:

```cpp
auto set_ticks_per_second(u64 const rate) -> void {
    DEBUG_ASSERT(rate != 0, "the counter's rate cannot be 0");
    [[maybe_unused]]
    auto const previous = TICKS_PER_SECOND.exchange(rate, std::memory_order_relaxed);
    DEBUG_ASSERT(previous == 0, "the counter's rate is already {} Hz; it is set once", previous);
}
```

After:

```cpp
static_assert(std::atomic<u64>::is_always_lock_free,
              "NMI and panic readers cannot take a lock");

auto set_ticks_per_second(u64 const rate) -> void {
    DEBUG_ASSERT(rate != 0, "the counter's rate cannot be 0");
    auto expected = u64{0};
    [[maybe_unused]]
    auto const first = TICKS_PER_SECOND.compare_exchange_strong(
        expected, rate, std::memory_order_relaxed, std::memory_order_relaxed);
    DEBUG_ASSERT(first, "the counter's rate is already {} Hz; it is set once", expected);
}
```

On failure `compare_exchange_strong` writes the value it found into `expected`, so the assertion message still prints the rate that was set first. The getter is unchanged.

Rejected:
- Keeping the `exchange` and fixing only the wording. The wording would then be right and the header's promise would still be a debug-build property.
- A load, then a store. Two CPUs calling at once could both see 0 and both store, and the assertion would see neither.
- A compare-exchange that retries or returns a status. The setter has one caller per boot; a second call is a bug to report, not a case to handle.

**Reference**
Zircon `timer_set_ticks_to_time_ratio()` (`zircon/kernel/kernel/timer.cc:77`) is the shape: `DEBUG_ASSERT(gTicksPerSecond == 0)` and then an unconditional store, so in a release build a second call there overwrites. This change departs from it on that point on purpose, and keeps its other shape (the getter reads a variable, the platform calls the setter once). Its variable is a plain `uint64_t`, which the earlier entry already records as the second departure.

**Verification**
- x86 `ours-verify`: build ok (out/bringup.x86, 0.6s); dbg PASS shell finished at 5.5s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.7s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 152 passed, 0 failed.
- riscv phys check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.
- Disassembly with `llvm-objdump -d -C` of the two images. The with-tests image, which has no assertions, has the whole setter as `xorl %eax, %eax; lock cmpxchgq %rdi, TICKS_PER_SECOND(%rip); retq`: the first rate wins and a later call falls through. The dbg image has the same `lock cmpxchgq` followed by a `jne` to the assertion's failure path. The getter is still `movq TICKS_PER_SECOND(%rip), %rax; retq`.
- The assertion, exercised once with a temporary second call `time::set_ticks_per_second(tsc_hz + 1)` after the real one in `apic_calibrate_clock()` (reverted before the commit; `git grep` finds none): the dbg boot printed `Assertion failed on first with message: the counter's rate is already 999995022 Hz; it is set once`, the first rate and not the second. The release behaviour is read off the disassembly above, not run.
- `kshell_run.py --out out/bringup.x86 "lockup status" "crash panic-locked"`: both ok.
- `format_cpp.py --check` clean on both files. `clang-tidy -p out/bringup.x86` reports nothing in `ticks_per_second.cpp`.

**Deliberately not done**
- No unit test of the once-only property in a release configuration: the dbg boot shows the assertion and the disassembly shows the release code, and a host test would need the kernel's `DEBUG_ASSERT` off and on in one binary.
- The getter is not touched.

**How it was found**
An independent review of `58383d44`, `146a0ece` and `b023807f` read the setter's disassembly (`xchgq` where the comment promised a plain `mov`) and then read what the release build does with a second call.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can `DEBUG_ASSERT(previous == 0)` not make "the answer does not change" true, and what does the compare-exchange do that the assertion cannot?
- Why must this be `compare_exchange_strong` and not `compare_exchange_weak`, and what would a spurious failure leave behind in a release build?
- If the atomic were not lock-free, which context would deadlock, on which CPU, and why can only a compile-time check catch it before then?
