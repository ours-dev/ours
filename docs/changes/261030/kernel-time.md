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

## Title: Comments say when ticks_per_second() is 0 and which "ticks per second" is which (2026-10-30)

**Files**
- `kernel/lib/lockup_detector/src/report.cpp` — `ASSUMED_TICKS_PER_SECOND`'s comment names the second time the rate is 0: a report made before the calibration has run
- `kernel/platform/pc/power.cpp` — the same, for a reset made before the calibration, which a panic early in the boot reaches
- `kernel/include/ours/time/cfg.hpp` — `HZ`'s comment says it is not the rate of `arch::Tick` and points to `ticks_per_second()`

Comments only; no statement changes (an assertion below a moved comment reports a different line number).

**Symptom**
None; a comment was incomplete and a name was ambiguous.

**Cause**
The two fallback comments (and the Cause paragraph of the second entry above) said the rate stays 0 only on "a PC whose local APIC timer calibration failed". That is the case where it is 0 forever. It is also 0 for the first part of every boot: `apic_calibrate_clock()` runs at `InitLevel::Irq + 4`, and anything that reads the rate earlier gets 0. Two readers can run in that window:
- `platform_reboot()`, reached by a panic early in the boot. Its `settle()` takes the 1 GHz branch, exactly as it did with the old constant, so behaviour is unchanged; the comment gave an incomplete reason for it.
- The lockup reporter. Its targets are `Cpu::online_mask()` minus itself, and before the calibration only the boot CPU is online (the rate "is known before any CPU but the boot CPU starts"), so the target mask is empty and `wait_for_answers()` returns at once; the 1 GHz bound is never spent. The comment now says so, rather than implying the wait is long.

A fallback that is documented for one cause invites a reader to delete it when that cause is judged rare. Stating both causes says why it must stay: the early-boot one is not rare, it is every boot until the calibration.

The second ambiguity is a name. Three things in this tree are counted "per second": `arch::Tick` counts (the CPU's own counter, `ticks_per_second()`), the periodic tick (`HZ`, the rate of the tick device), and a `ClockSource`'s counts. `ticks_per_second.hpp` already said "It is not HZ"; `cfg.hpp` said "Ticks per second: the rate the periodic tick runs at" with no pointer back, so a reader who started at `HZ` and wanted the counter's rate had no way to learn there was another.

Corrections to the earlier entries in this file, which are append-only and so are corrected here:
- The Verification of the second entry says `crash panic` printed `settle 100000500 counts at 1000005000 Hz`. That line comes from a temporary log call at the top of `settle()` and has to beat QEMU's reset, which the write to port 0xCF9 that precedes it triggers; a review that repeated the experiment five times (one with `--reboot`) never saw the line, though it did see the reporter's bound (`answer wait 1999990000 counts at 999995000 Hz`) and, with the getter forced to 0, `2000000000`. So the settle line is a race against the reset, not a measurement to rely on. `settle_ticks()` has the same arithmetic as the reporter's bound (`rate / 1000 * 100`: 1,000,005,000 / 1000 * 100 = 100,000,500), which is what the number was, and the failed-calibration branch is read from the code, not run.

**Change**
Before and after, `power.cpp` (the others have the same shape):

```cpp
// before
/// The TSC's rate, in counts per second, that settle() takes it to be
/// while time::ticks_per_second() does not know it: a PC whose local APIC
/// timer calibration failed. 1 GHz is the rate the old count was written
/// for, so such a PC settles exactly as long as it did.

// after
/// The TSC's rate, in counts per second, that settle() takes it to be
/// while time::ticks_per_second() does not know it: a PC whose local APIC
/// timer calibration failed, and a reset made before the calibration has
/// run, which a panic early in the boot reaches. 1 GHz is the rate the
/// old count was written for, so such a PC settles exactly as long as it
/// did.
```

`cfg.hpp` gains: "Not the rate of arch::Tick, the CPU's own counter, whose counts are not ticks of this kind; that is ticks_per_second() in ours/time/ticks_per_second.hpp."

**Reference**
None, this project's own design: the comments follow the contract in `ticks_per_second.hpp`.

**Verification**
- x86 `ours-verify`: build ok (out/bringup.x86, 4.1s); dbg PASS shell finished at 5.8s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 5.0s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 152 passed, 0 failed.
- riscv phys check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.
- `git diff` of the commit shows comment lines only; `format_cpp.py --check` is clean on the three files.

**Deliberately not done**
- The three-way naming overlap ("ticks") is not renamed; `HZ` and `ticks_per_second()` are the user's names (2026-10-30).
- No shared duration-to-counts helper, and no change of what any of these does at rate 0: that is a design question, put to the user with this round's findings.

**How it was found**
An independent review of `58383d44`, `146a0ece` and `b023807f`: it re-ran the experiments in the second entry's Verification, and read the comments against `apic_calibrate_clock()`'s `InitLevel`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- For how much of every boot is `ticks_per_second()` 0 on a PC whose calibration works, and who can read it in that window?
- Why does the lockup reporter spend none of its 1 GHz fallback bound before the calibration, while `platform_reboot()` does spend its settle time?
- `HZ` and `ticks_per_second()` are both "per second"; what does each count?

## Title: ours::time::ticks_in() turns a wait written as a time into counts of arch::Tick in one place, with its overflow and its answer for an unknown rate decided (2026-10-30)

**Files**
- `kernel/include/ours/time/ticks_per_second.hpp` — `ticks_in(Milliseconds) -> u64`, inline, beside `ticks_per_second()`; includes `ours/syscall/time.hpp` for `Milliseconds` and `ffl/saturating.hpp` for the clamped product
- `kernel/lib/logz4/kernel.cpp` — `panic_lock_ticks()` is `ours::time::ticks_in(PANIC_LOCK_WAIT)`; a 0 still means "try the lock once and give up"
- `kernel/lib/lockup_detector/src/report.cpp` — `ASSUMED_TICKS_PER_SECOND` (1 GHz) becomes `ANSWER_WAIT_FALLBACK_COUNT = u64{2'000'000'000}`; `answer_wait_ticks()` is `ticks_in(ANSWER_WAIT)` with `if (ticks == 0) { ticks = ANSWER_WAIT_FALLBACK_COUNT; }`
- `kernel/platform/pc/power.cpp` — the same for the reset: `SETTLE_FALLBACK_COUNT = u64{100'000'000}` replaces `ASSUMED_TICKS_PER_SECOND`
- `kernel/kernel/time/test/BUILD.gn`, `kernel/kernel/time/test/ticks_in_test.cpp` — a host test of `ticks_in()`, 11 cases, with its own `ticks_per_second()`
- `BUILD.gn` — the test is in `//:default`
- `.agents/skills/ours-kernel-design/references/task.md`, `.agents/tasks/riscv-kernel/PLAN.md` — the decision is recorded; step 3 says which readers go through `ticks_in()`

**Symptom**
None: every rate the callers see gives the same count as before (checked below, on QEMU and with the rate forced to 0). What was wrong is the shape. Three places turned a duration into counts, each with its own copy of the arithmetic `rate / 1000 * milliseconds` and its own policy for a rate of 0: logz4's panic lock (0 counts, give up at once), the lockup reporter's wait (assume 1 GHz) and the PC reset's settle (assume 1 GHz). The next reader of the rate, riscv's step 3 onwards, would have written a fourth.

**Cause**
A count of `arch::Tick` is a time only on the counter it was measured on, so the previous three commits (`b62a424f`, `b023807f` and the ones around them) turned the constants into durations. What they left behind was the conversion itself, written out three times, and the part of it that is a decision rather than arithmetic: what to do while the rate is not known. That decision differs between the callers for a reason, which is why a helper that hid it would have been wrong:
- logz4's panic lock gives up at once. While the rate is 0 only the boot CPU runs, so a lock a write finds held is this CPU's own (a panic inside `write()`, or an NMI that interrupted one). It never lets go, so no wait can help; waiting 250 ms for it, as the 1 GHz assumption did, was the bug `b62a424f` removed.
- The reporter and the reset wait because waiting is the point: one for CPUs that may answer an NMI, the other for a chipset to act on a request. A bound of 0 counts there would end the wait before it started, so each needs a count to fall back on. The old counts, written when the only counter anyone had was QEMU's 1 GHz TSC, are the best guess anybody has.

So the helper answers the one thing it knows, 0 for "I cannot say", and the caller writes what 0 means for it. Two shapes were rejected. `ticks_in(duration, fallback)` would put the policy in the signature, and logz4 would pass a fallback of 0, which reads as a wait of nothing instead of a decision to give up. A helper that assumed 1 GHz itself would give logz4 back the 250 ms panic wait.

The second decision is the overflow. The product of the rate in kilohertz and the milliseconds is a u64 multiplication, so it can wrap, and a wrapped bound is the worst answer a wait can get: a very long wait becomes a short one, in the loops (`now - start >= bound`) that are least able to afford it, panic and reboot paths. What a u64 holds:

| rate | largest duration whose count is exact |
|---|---|
| 10 MHz (QEMU's riscv `virt`) | 1.8e15 ms, 58 000 years |
| 1 GHz (QEMU's x86) | 1.8e13 ms, 584 years |
| 10 GHz | 1.8e12 ms, 58 years |
| the largest u64 | 1000 ms |

Read the other way: a minute is exact up to a rate of 3.07e17 Hz and the reporter's 2 s up to 9.2e18 Hz, thirty million to nine hundred million times a 10 GHz counter. So no caller can reach the edge. Three ways to say what happens there were weighed: wrap (rejected above), a `DEBUG_ASSERT` (compiled out of the release image, and an assertion in the panic path that is already panicking), and saturate. Saturating is the one that keeps a property a reader can state: a longer duration is never fewer counts. At the top it answers the largest u64, which a loop of the form `now - start >= bound` never reaches, so the wait does not end; that is the honest result of asking for a wait the counter cannot count, and the header says a caller that takes its duration from outside the code has to limit it first. The clamp is `ffl::saturating_mul<u64>()`, which is already in every kernel switchset's include path (`build/switch/kernel_switchset.gni`) and built on `__builtin_mul_overflow`; `std::mul_sat` would have been the standard spelling, but the host build of the test does not find it (`no member named 'mul_sat' in namespace 'std'`), and a header the kernel and the host test both include has to compile in both.

The arithmetic keeps its old form, `rate / 1000 * milliseconds`, on purpose. Dividing first keeps the product no larger than the count it stands for, so it overflows only when the answer does not fit; `rate * milliseconds / 1000` would wrap while the answer still fit (at 10 GHz after 21 days). The price is that the rate is rounded down to whole kilohertz: the count falls short by less than one count per millisecond of the duration, under 0.1% for a counter of 1 MHz and up, nothing for 10 MHz and 1 GHz, and a rate under 1 kHz answers 0 as well (none exists here). The test pins it, with `1'000'999` Hz giving the same count as `1'000'000`. Keeping it means the value is the one the callers computed before, for every rate they can see, which is what let the verification below compare two numbers instead of arguing about a tolerance.

**Change**
The header, with the body in full:

```cpp
[[nodiscard]]
inline auto ticks_in(Milliseconds const duration) -> u64 {
    constexpr auto MILLISECONDS_PER_SECOND = u64{1'000};
    if (duration <= Milliseconds::zero()) {
        return 0;
    }
    auto const per_millisecond = ticks_per_second() / MILLISECONDS_PER_SECOND;
    return ffl::saturating_mul(per_millisecond, static_cast<u64>(duration.count()));
}
```

It is inline, a load and a multiply with the clamp, so it keeps the getter's contract (no lock, no allocation, no CPU-local state; callable from an NMI or a panic). A duration that is not positive answers 0 rather than being cast to u64, where -1 ms would have been the longest wait there is. The parameter is `Milliseconds`: a `Seconds` converts implicitly, a `Nanoseconds` does not compile, so a caller that wants finer than a millisecond has to say how to round.

The three callers, before and after:

```cpp
// logz4/kernel.cpp, before
return ours::time::ticks_per_second() / 1000 * static_cast<u64>(PANIC_LOCK_WAIT.count());
// after
return ours::time::ticks_in(PANIC_LOCK_WAIT);

// lockup_detector/src/report.cpp, before
auto const milliseconds = static_cast<u64>(ANSWER_WAIT.count());
auto const rate = ours::time::ticks_per_second();
if (rate == 0) {
    return ASSUMED_TICKS_PER_SECOND / 1000 * milliseconds;
}
return rate / 1000 * milliseconds;
// after
auto ticks = ours::time::ticks_in(ANSWER_WAIT);
if (ticks == 0) {
    ticks = ANSWER_WAIT_FALLBACK_COUNT;
}
return ticks;
```

`power.cpp` is the same as the reporter with `time::ticks_in(SETTLE_TIME)` and `SETTLE_FALLBACK_COUNT`. The comments that said when the rate is 0 (before `apic_calibrate_clock()`, or after a failed calibration) moved onto the constants, which now hold a count and no longer a rate: `ANSWER_WAIT_FALLBACK_COUNT` is "the count this used to be, written for a 1 GHz counter".

One difference exists, for a rate in 1..999 Hz only: the old reporter and reset computed 0 counts there and ended the wait at once; they now take the fallback. No counter this kernel can run on has such a rate.

Two decisions came with it, both the user's (2026-10-31, "暂时先同意", taking the recommendations): the reporter keeps 2 s and the reset 100 ms, and x86 keeps its two copies of the rate (`ApicTimerEvent::TSC_HZ` with `apic_tsc_hz()`, and `ticks_per_second()`) for now; the second is a repair item in `docs/repairs/261030/x86-apic.md`.

**Reference**
Zircon has the same two problems and answers them in its ratio type rather than a helper per call: `TimerQueue::ConvertMonotonicTimeToRawTicks()` (`zircon/kernel/kernel/timer.cc:106`) turns a time into raw ticks through `timer_get_ticks_to_time_ratio().Inverse().Scale<affine::Ratio::Round::Up>()`, and `affine::Ratio::Scale()` (`zircon/system/ulib/affine/include/lib/affine/ratio.h:50`) returns `kOverflow` or `kUnderflow` instead of wrapping, so saturation is the reference's choice too. It differs in two ways: it keeps an exact rational and rounds up, where this keeps whole kilohertz and rounds down (to keep the old values); and it has no unknown-rate state, since `timer_set_ticks_to_time_ratio()` (`timer.cc:77-83`) asserts both terms are non-zero and the ratio is set before anything converts. Here the rate is 0 for the first part of every boot, which is why 0 is part of the contract.

**Verification**
- Host test: `out/bringup.x86/host/obj/kernel/kernel/time/test/ticks_in`, 11 cases, all pass. They cover a rate of 0, QEMU's x86 rate (`1'000'005'000`: 250 ms is `250'001'250` counts, 2 s `2'000'010'000`, 100 ms `100'000'500`), riscv `virt`'s 10 MHz, the kilohertz rounding, a rate under 1 kHz, a duration of 0 and a negative one (`Milliseconds::min()` too), 10 GHz for a minute and for a year, the 58-year edge at 10 GHz (`1'844'674'407'370` ms is exact and one more saturates), the edge at the largest rate (1000 ms exact, 1001 ms saturated), `Seconds(2)` equal to `Milliseconds(2'000)`, and that a longer duration is never fewer counts across the edge, at four rates.
- x86 `ours-verify`: build ok (out/bringup.x86, 0.9s); dbg PASS shell finished at 5.5s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.8s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed (152 before the new test).
- riscv phys check (`boot_check.py --out out/bringup.riscv`): PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.
- `kshell_run.py --out out/bringup.x86 "lockup status" "timershare.test" "crash panic-locked"` and `"lockup nmi"`: all ok. In the dbg log of `crash panic-locked`, the last `CPU[3] suspended` line is at `[7405805000` and the panic message at `[7655983000`: 250.18 ms, the 250 ms bound as before.
- The three bounds against the old arithmetic, from temporary log lines (reverted before the commit; the built image holds no `TEMP` string). Each printed the new function's value next to the old expression evaluated from the same read of the rate, in the same boot. The panic lock's line is in `log_console_waits()`, since a log call inside the lock-waiting path would wait on the lock it is measuring; the reporter's is in `wait_for_answers()`; the settle's is at the top of `platform_reboot()`, before the write to port 0xCF9 that triggers QEMU's reset (the earlier experiment put it at the top of `settle()`, which runs after that write).

| bound | rate (Hz) | `ticks_in()` path | old arithmetic |
|---|---|---|---|
| logz4 panic lock, 250 ms | 999995045 | 249998750 | 249998750 |
| reporter's wait, 2 s | 999994999 | 1999988000 | 1999988000 |
| reset's settle, 100 ms | 999995007 | 99999500 | 99999500 |

- The same with the rate forced to 0 (a throwaway build whose `ticks_per_second()` returned `0 * TICKS_PER_SECOND.load(...)`, reverted; the old expressions are the 1 GHz fallback for the last two, `rate / 1000 * ms` for the first):

| bound | new | old arithmetic |
|---|---|---|
| logz4 panic lock | 0 | 0 |
| reporter's wait | 2000000000 | 2000000000 |
| reset's settle | 100000000 | 100000000 |

  With the rate at 0, `crash panic-locked` gave up at once: 0.66 ms between the last `suspended` line (`[6198435000`) and the panic message (`[6199096000`), against 250 ms with the rate known.
- `format_cpp.py --check` and `format_gn.py --check` clean on the changed files; `clang-tidy -p out/bringup.x86` reports nothing on `ticks_per_second.hpp`, `logz4/kernel.cpp`, `report.cpp`, `power.cpp` and `ticks_in_test.cpp`.
- Not measured: the code size of the inline function at three call sites (the arguments are constants, but the clamp's overflow test depends on the rate and stays), and `ticks_in()` on riscv, which has no setter call until step 3.
- A correction to the second entry of this file, which said the settle line races QEMU's reset and could not be read. It does when it sits after the write to 0xCF9, as that one did. Placed before it, it is in the log. The other reason it looked missing: the kshell log (`dbg.log`) is a binary file to `grep`, which prints nothing for a match in it unless given `-a`.

**Deliberately not done**
- `rate * milliseconds / 1000`, or a 128-bit product, for an exact count: the value would change by up to one count per millisecond, and the point of this change is that it does not.
- A fallback inside the helper, or a second overload with one: see Cause.
- `tick_period()` and `log_console_waits()` in logz4 still divide the rate by `HZ` and by a million. They turn counts into a time, or take one tick's length; neither is a duration turned into a bound.
- Units other than milliseconds. Every caller has milliseconds; a finer unit is added when a caller needs it, with its own rounding.
- The 1 GHz fallback counts did not move into the header. They are a guess about a machine the header cannot name.
- x86's second copy of the rate: the repair item in `docs/repairs/261030/x86-apic.md`.

**How it was found**
None. The three conversions, and what each does for a rate of 0, were listed by the review of `58383d44` to `5485510b`, and the user decided their open questions on 2026-10-31.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does `ticks_in()` answer 0 for an unknown rate instead of the 1 GHz count, and what would logz4's panic path cost if it assumed 1 GHz? Why can the reporter not simply use 0 as well?
- Why is the product `rate / 1000 * milliseconds` and not `rate * milliseconds / 1000`: at 10 GHz, after how long does the second wrap while its answer would still fit, and what does the first one give up?
- A bound saturated at the largest u64 meets a loop `while (now - start < bound)`. What does the loop do, and why is that a better failure than a wrapped bound, for a wait inside a panic?

## Title: The reporter's and the reset's fallback counts are tied to their durations by a static_assert (2026-10-30)

**Files**
- `kernel/lib/lockup_detector/src/report.cpp` — a `static_assert` after `ANSWER_WAIT_FALLBACK_COUNT` says it is `ANSWER_WAIT` at 1 GHz; the constant's comment says why it is a count and what keeps it honest
- `kernel/platform/pc/power.cpp` — the same for `SETTLE_FALLBACK_COUNT` and `SETTLE_TIME`

**Symptom**
Latent, cannot be triggered today. It would show the first time someone edits `ANSWER_WAIT` or `SETTLE_TIME` without the count beside it (the riscv port is the likely occasion): a PC whose APIC timer calibration failed, or a reset made before it ran, would go on waiting the old time, and nothing would say so.

**Cause**
Before the previous entry, the fallback was *derived*: `ASSUMED_TICKS_PER_SECOND / 1000 * milliseconds`, with the duration as the one input. Editing `ANSWER_WAIT` moved both the known-rate branch and the unknown-rate branch, because there was only one number to edit. The previous entry replaced that with two named counts (`2'000'000'000` and `100'000'000`), which the user chose so that a PC without a rate waits exactly as long as it always did. That choice is right, and it has a price: a duration and the count that stands for it at 1 GHz are now two places that must agree, and the only thing connecting them was a comment. A comment is not checked by anything. This is the same shape as the repair item on x86's two copies of the rate (`docs/repairs/261030/x86-apic.md`): one fact, two owners, and the link between them is somebody's memory.

The language can check this one, because both sides are `constexpr`: the count is `ANSWER_WAIT.count()` milliseconds times 1 000 000 counts per millisecond, which is what 1 GHz means. So the link becomes a `static_assert`, and an edit of one side without the other stops compiling instead of silently changing a fallback that runs only in a panic or a reset.

**Change**
After each constant:

```cpp
// report.cpp
constexpr auto ANSWER_WAIT_FALLBACK_COUNT = u64{2'000'000'000};
static_assert(ANSWER_WAIT_FALLBACK_COUNT == static_cast<u64>(ANSWER_WAIT.count()) * 1'000'000,
              "the fallback count is ANSWER_WAIT at 1 GHz, 1 000 000 counts a millisecond");

// power.cpp
constexpr auto SETTLE_FALLBACK_COUNT = u64{100'000'000};
static_assert(SETTLE_FALLBACK_COUNT == static_cast<u64>(SETTLE_TIME.count()) * 1'000'000,
              "the fallback count is SETTLE_TIME at 1 GHz, 1 000 000 counts a millisecond");
```

Each constant's comment gained a paragraph: it is a count and not derived from the duration, so the assertion is what keeps the two from drifting apart.

Rejected: deriving the count (`ANSWER_WAIT.count() * 1'000'000`). It is one number again, but it is the `ASSUMED_TICKS_PER_SECOND` the user had removed, with the 1 GHz hidden inside an expression where a reader no longer sees that it is a guess about a machine. The named count says "this is what it used to be"; the assertion says "and it still is the duration at 1 GHz".

**Reference**
None, this project's own design. The reference's conversion (`DurationToTicks()`, `zircon/kernel/lib/lockup_detector/lockup_detector.cc:64`) has no unknown-rate state to fall back from, so there is nothing to keep in step.

**Verification**
- Both assertions hold for the real values (the files compile) and fire for a changed duration. In scratch copies built with the real compile flags (`-fsyntax-only`, nothing in the tree touched), `ANSWER_WAIT = 2'001 ms` gave `static assertion failed due to requirement 'ANSWER_WAIT_FALLBACK_COUNT == static_cast<unsigned long>(ANSWER_WAIT.count()) * 1000000'`, and `SETTLE_TIME = 101 ms` the same for the settle.
- x86 `ours-verify`: build ok (out/bringup.x86, 1.7s); dbg PASS shell finished at 5.5s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.7s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed.
- riscv phys check (`boot_check.py --out out/bringup.riscv`): PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.
- `format_cpp.py --check` clean on both files; `clang-tidy -p out/bringup.x86` prints no diagnostic located in `report.cpp` or `power.cpp` (the warnings it prints are in `x86_pagings.hpp` and other headers this change does not touch).
- Not measured: nothing at run time changed, so no boot output differs.

**Deliberately not done**
- The `1'000'000` is a literal in each file, not a shared named constant: two uses in two files, each beside the message that explains it. A header constant would be a second place to learn what it is.
- No assertion in `ticks_in()`'s header: the helper does not know the fallback; each caller owns what 0 means for it.

**How it was found**
An independent review of `5e64cbe2` noticed that the old code derived the fallback from the duration and the new code did not, and wrote both assertions as scratch copies before suggesting them.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Before `5e64cbe2`, what did one edit of `ANSWER_WAIT` move, and what does the same edit move now if the assertion is absent?
- Why can this be a `static_assert`, where a check that the count equals `ticks_in()` at some rate could not?
- Why is a fallback that only runs in a panic or a reset the worst place for two numbers to disagree quietly?

## Title: ticks_in()'s host test pins "the same value as the old arithmetic" over 24 000 rate and duration pairs, not only hand-picked ones (2026-10-30)

**Files**
- `kernel/kernel/time/test/ticks_in_test.cpp` — a twelfth case, `TheCountIsTheOldArithmeticWhereItDidNotWrapAndSaturatedWhereItDid`, and `#include <random>`

**Symptom**
Latent, cannot be triggered today. A later change to the arithmetic in `ticks_in()` that still gives the eleven hand-picked values would pass the suite and change what the three callers wait.

**Cause**
The claim the previous commit made for `ticks_in()` is not "these eleven numbers": it is that for every rate the callers see it gives what `rate / 1000 * milliseconds` gave before, and the largest u64 where that product wrapped. The eleven cases pin values worked out by hand, which is a way of checking a claim at a few points. A test that is meant to stop a regression has to hold the claim itself, and the old expression is the oracle: it is one line, and it is exactly what the claim is about.

The review that found this checked the claim with a throw-away program (2 000 000 random rates times six durations, 12 000 000 pairs, no mismatch where the old product did not wrap, and the largest u64 where it did). That program was run once and thrown away; this case keeps a smaller version where the next change will run it.

Three details of the case are about making such a test worth having:
- A fixed seed (`std::mt19937_64{20261030}`). A random test that can fail on one run in a thousand is a test nobody trusts; with a fixed seed a failure is the same failure every time, and the message names the rate and duration.
- The magnitudes are drawn first. A uniform draw over a u64 is above 2^60 fifteen times in sixteen, a rate no counter has, and nearly every pair would saturate. So the case draws a bit count from 0 to 64 and then a rate of up to that many bits, which spreads the pairs from 0 Hz to the largest u64.
- Neither half may be empty. The case counts the pairs where the old product fitted and where it wrapped, and expects more than a thousand of each, so a mistake in the generator that made every pair fit (or every pair wrap) fails the test and does not make it pass vacuously.

The old product wrapped exactly when `per_millisecond != 0 && old_count / per_millisecond != milliseconds`: dividing a wrapped product by one factor does not give back the other. That test does not use `__builtin_mul_overflow`, which is what `ffl::saturating_mul()` is built on, so the oracle and the code under test do not share the thing that could be wrong.

**Change**
One test case:

```cpp
auto const per_millisecond = rate / 1'000;
auto const old_count = per_millisecond * milliseconds;
auto const old_wrapped = per_millisecond != 0 && old_count / per_millisecond != milliseconds;
auto expected = old_count;
if (old_wrapped) {
    expected = U64_MAX;
}
EXPECT_EQ(ticks_in(Milliseconds(static_cast<Milliseconds::rep>(milliseconds))), expected);
```

for 4 000 rates and the durations 1, 100, 250, 2 000, 60 000 and 86 400 000 ms (the three callers' waits, and three longer ones that reach the edge at lower rates).

**Reference**
None, this project's own design.

**Verification**
- The host test `out/bringup.x86/host/obj/kernel/kernel/time/test/ticks_in`: 12 tests, all pass (11 before).
- The case can fail where the others cannot. With the header temporarily changed to add one count to `per_millisecond` for a rate whose remainder modulo 1000 is between 400 and 600 (a mistake none of the hand-picked rates has: they end in 000, 999, 615 or are under 1000), the eleven old cases passed (`PASSED 11 tests`) and the new one failed. The header was restored with `git checkout` and is not part of this change.
- x86 `ours-verify`: build ok (out/bringup.x86, 0.7s); dbg PASS shell finished at 5.8s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.7s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed (the count is of test programs, so a new case in an existing one leaves it at 153).
- riscv phys check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.
- `format_cpp.py --check` clean; `clang-tidy -p out/bringup.x86` prints no diagnostic located in `ticks_in_test.cpp`.

**Deliberately not done**
- A bigger sample. 4 000 rates times six durations runs in a few milliseconds; the throw-away program's 12 million pairs bought confidence once, and a test that runs on every build should stay cheap.
- Checking the three callers' bounds. They are one line over `ticks_in()` and were compared to the old arithmetic on QEMU in the previous record.

**How it was found**
An independent review of `5e64cbe2` ran the comparison itself and noted that the committed test could not catch a later change of the arithmetic that still passed its hand-picked values.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is the old expression a good oracle here, and why is `__builtin_mul_overflow` a bad one for detecting that it wrapped?
- Why does the case draw a bit count before it draws the rate, and what would a uniform draw over u64 have done to the test?
- What does the counting of fitted and wrapped pairs protect against?

## Title: Three statements that stopped matching the code are corrected: the kilohertz rounding "costs nothing", ffl being the scheduler's, and the panic lock's 1 GHz assumption in PLAN.md (2026-10-30)

**Files**
- `kernel/include/ours/time/ticks_per_second.hpp` — `ticks_in()`'s comment says what the rounding to whole kilohertz costs at a measured rate, instead of "nothing"
- `build/switch/kernel_switchset.gni`, `system/lib/ffl/BUILD.gn` — the comments on `ffl` say the scheduler is not its only user
- `.agents/tasks/riscv-kernel/PLAN.md` — the 2026-10-29 "Landed" paragraph's sentence about the panic lock's bound points at what replaced it

**Symptom**
None: comments and one plan sentence. Each one told a reader something that is false.

**Cause**
Each was true when it was written and stopped being true when a later change did not look for it:
- `ticks_in()`'s comment said the rounding down to whole kilohertz costs "nothing for the 10 MHz and 1 GHz this runs on". That holds for a nominal 10 MHz or 1 GHz, which are multiples of 1 kHz. The rate the kernel actually uses on QEMU's x86 is not nominal: it is what the calibration measured, 999 995 045 Hz in the record of `ticks_in()`'s entry. Floored to kilohertz that is 999 995 000, so 45 Hz is dropped, 45 parts per billion: the 250 ms bound is 249 998 750 counts against an exact 249 998 761.25. Tiny, but not "nothing", and a comment that says "nothing" teaches the reader that the rounding cannot be observed, which is the opposite of what the first line of the paragraph says about a counter slower than 1 MHz. (A 32.768 kHz timebase, the kind a small riscv microcontroller has, would lose 2.3% of every wait: that is the reason the comment keeps its limit "1 MHz or faster", and a note for whoever ports to such a part.)
- Two comments called `ffl` the scheduler's arithmetic. It was, until `ticks_in()` began to clamp its product with `ffl::saturating_mul()` in a header that the logger, the lockup reporter and the PC reset include. A reader who removed the scheduler from a build would conclude `ffl` could go with it, and the logger would stop compiling. The include path is not x86's: its `config` is in the unconditional part of the switchset's `prefix_configs` list, so riscv gets it too.
- `PLAN.md` step 1a's "Landed" paragraph says the panic lock bound is "250 ms, with 1 GHz assumed while the rate is unknown". That stopped being true in `b62a424f`, which the paragraph after it already records; a reader of the first sentence alone was left with the old behaviour.

**Change**
The header's paragraph:

```text
// before
// a counter of 1 MHz or faster, and nothing for the 10 MHz and 1 GHz this
// runs on. For the same reason ...
// after
// a counter of 1 MHz or faster. A nominal 10 MHz or 1 GHz loses nothing; a
// rate that a calibration measured loses its odd hertz, 45 of the
// 999 995 045 Hz that QEMU's x86 gives (45 parts per billion). For the same
// reason ...
```

The switchset's comment gained "It is also reached from ours/time/ticks_per_second.hpp, which clamps its product with ffl::saturating_mul(), so the scheduler is not its only user", and `ffl`'s `BUILD.gn` the same in one sentence. `PLAN.md`'s sentence got a parenthesis: "since `b62a424f` the lock is tried once while the rate is unknown, and the bound is `ours::time::ticks_in()`'s; history stays as written". The paragraph itself is not rewritten: a plan's "Landed" text is what was true on the day.

**Reference**
None, this project's own design.

**Verification**
- x86 `ours-verify`: build ok (out/bringup.x86, 2.3s); dbg PASS shell finished at 5.6s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.8s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed.
- riscv phys check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.
- `format_cpp.py` and `format_gn.py --check` clean on the three code and build files; `lint_docs.py --changed` clean on `PLAN.md`.
- Not measured: nothing at run time changed; the rounding figures are arithmetic on the rate in the record of the `ticks_in()` entry (999 995 045 Hz times 0.25 s is 249 998 761.25).

**Deliberately not done**
- Making the header's rounding exact. The previous record decided to keep `rate / 1000 * milliseconds` so that the values stay the ones the callers had; this change only makes the comment say what that costs.
- Moving `ffl` out of the scheduler's description altogether. Its main user is still the scheduler; the comments now say it is not the only one.
- Rewriting `PLAN.md`'s paragraph. Only a pointer was added.

**How it was found**
An independent review of `5e64cbe2` compared the three statements with the code: the rounding against the measured rate in the record's own table, the `ffl` comments against `git grep 'ffl/'`, and `PLAN.md` line 30 against `b62a424f`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is rounding to whole kilohertz free for a nominal 1 GHz and not for the rate QEMU measures, and for what kind of timebase would it become a visible error?
- Which header made `ffl` more than the scheduler's, and why is the include path for it the same on riscv as on x86?
- Why does `PLAN.md` get a pointer rather than a corrected sentence?
