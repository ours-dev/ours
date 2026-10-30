# logz4 changes

## Title: The bootstrap console's byte output and its counter's rate become two platform calls, and the rest of it builds for every architecture (2026-10-29)

**Files**
- `kernel/lib/logz4/kernel.cpp` — no `#if defined(ARCH_X86)` any more: `SerialLogger::write()` hands its bytes to `platform::debug_console_write()`, the tick period and `log_console_waits()` read `arch_tick_hz()`, and the panic-mode lock bound is `PANIC_LOCK_WAIT_MS`, a time, turned into counts by `panic_lock_ticks()`; the empty `#else` branch is gone
- `kernel/platform/include/ours/platform/debug_console.hpp` — declares `debug_console_write()` beside `debug_console_read()`
- `kernel/platform/include/ours/platform/timer.hpp` — declares `arch_tick_hz()`
- `kernel/platform/pc/debug_console.cpp` — defines `debug_console_write()`: COM1, polled, the loop that was in logz4
- `kernel/platform/pc/timer.cpp` — new: defines `arch_tick_hz()` as the TSC rate the local APIC calibration measured (`apic_tsc_hz()`)
- `kernel/platform/pc/BUILD.gn` — compiles `timer.cpp`
- `kernel/lib/logz4/BUILD.gn` — depends on `//kernel/platform:headers`
- `kernel/include/ours/start.hpp` — `init_bootstrap_logging()` and `log_console_waits()` no longer say they are empty on other architectures

**Symptom**
Latent on x86, which logs exactly as before. On any other architecture the kernel would have printed nothing at all: the whole of `kernel/lib/logz4/kernel.cpp` was inside `#if defined(ARCH_X86)`, and its `#else` made `init_bootstrap_logging()`, `hold_console_lock_forever()` and `log_console_waits()` empty functions. A riscv vmours would have run `start_kernel()` with no logger installed, and its panics would have gone to `system/lib/logz4`'s weak default `emergency_write()`, which drops what it is given (`src/init.cpp:42-47`): no line, no link error, nothing.

**Cause**
The objects:
- `SerialLogger`, logz4's console sink. Its job has four parts: a test-and-set lock that keeps one line's bytes together on the wire; a per-CPU `WriteAccount` of how long writes waited and kept interrupts off; panic mode's bounded wait for that lock; and putting the bytes on the device.
- The device: on a PC, COM1, a 16550 UART at I/O port 0x3F8, written with `outb` once its line-status register says the transmitter is empty.
- `arch::Tick::get()`, the free-running counter every timestamp here is read from: the TSC on x86, `rdtime` on riscv. Its rate differs by machine and is known only at run time: on a PC from the local APIC timer's calibration at `InitLevel::Irq + 4` (`apic_tsc_hz()`), on riscv from the device tree's `timebase-frequency`.

Why it was a mistake. Of the four parts, only the last is about x86, and so is one input to the accounting, the counter's rate. The lock, the account and the panic bound are the same code on every machine. Guarding the whole file by architecture made three generic mechanisms x86-only to keep two x86 lines out of other builds, and the `#else` stubs hid the gap: every other architecture linked and ran silently instead of failing to link. That is the one combination a bring-up cannot afford, a kernel that runs and says nothing.

The panic bound had the same problem in a quieter form. `PANIC_LOCK_TICKS` was 250 million counts of `arch::Tick`, which its comment explained as "50 ms at a 5 GHz TSC and 250 ms at a 1 GHz one". A count is only a time on one counter. On QEMU's riscv `virt` board the counter runs at 10 MHz, and the same 250 million counts would be 25 seconds of a panicking kernel waiting for a lock.

**Change**
The two machine-specific parts become calls of the platform seam, `kernel/platform/include/ours/platform`, which already held the read side of this very console (`debug_console_read()`, COM1 in `pc/debug_console.cpp`, `NotSupported` on riscv):

```cpp
// ours/platform/debug_console.hpp
auto debug_console_write(char const *s, usize n) -> void;

// ours/platform/timer.hpp
[[nodiscard]]
auto arch_tick_hz() -> u64;
```

Before, in `SerialLogger::write()`:

```cpp
auto const is_transmit_empty = [&]() { return arch::inb(COM1 + 5) & 0x20; };
for (usize i = 0; i < n; ++i) {
    while (!is_transmit_empty()) {
    }
    arch::outb(COM1, static_cast<u8>(s[i]));
}
```

After:

```cpp
ours::platform::debug_console_write(s, n);
```

with the same loop, unchanged, in `kernel/platform/pc/debug_console.cpp`. `debug_console_write()` takes no lock and leaves interrupts alone, so the logger's lock and interrupts-off stretch are still the logger's, around the call, as they were around the loop.

The panic bound is now a time, converted with the rate:

```cpp
constexpr auto PANIC_LOCK_WAIT_MS = u64{250};
constexpr auto ASSUMED_TICK_HZ = u64{1'000'000'000};

auto panic_lock_ticks() -> u64 {
    auto const hz = ours::arch_tick_hz();
    if (hz == 0) {
        return ASSUMED_TICK_HZ / 1000 * PANIC_LOCK_WAIT_MS;
    }
    return hz / 1000 * PANIC_LOCK_WAIT_MS;
}
```

250 ms, and 1 GHz while the rate is unknown, were chosen so that x86 under QEMU, whose TSC runs at exactly 1 GHz (`APIC timer: ... TSC 1000000000 Hz` in the boot log), waits exactly what it waited before, 250 million counts, both before and after the calibration. On a 5 GHz machine the wait after calibration grows from 50 ms to 250 ms, paid at most once per panic. The limit of the fallback is written down beside it: before a platform knows its rate the wait is 250 million counts whatever the counter, 25 s on a 10 MHz one. On a PC only the boot CPU runs before the calibration, so a held lock then is one the boot CPU took itself, by panicking inside a write, and the whole wait is lost either way; a platform with a slow counter should answer `arch_tick_hz()` from its first log line.

Where the two calls live, and what was rejected:
- On the platform seam, not in the arch layer, because the kernel's debug console is the platform's: a PC's COM1 is a PC device, not an x86 one, and which UART a board has is the board's. That is also where the read side already was. `debug_console_write()` is in `ours::platform` beside `debug_console_read()`; `arch_tick_hz()` is in `ours/platform/timer.hpp`, the platform's timer header, in its namespace `ours`.
- `arch_tick_hz()`, not Zircon's name `ticks_per_second()`: in this tree a "tick" is also the scheduler's periodic tick (`time::HZ`), and `tick_period()`, right beside the caller, is "one scheduler tick in counter counts". A name that says which counter it means cannot be read as `HZ`.
- Defined per platform (`pc/timer.cpp` returns `apic_tsc_hz()`), rather than as a generic variable that the platform's calibration sets once (Zircon's `ticks_per_second()` returns `gTicksPerSecond` in `zircon/kernel/kernel/timer.cc`, set by the platform's timer code). The variable would add generic state and a setter that x86's calibration must remember to call, for one caller; the function makes each platform answer for itself and needs nothing new in the generic kernel. Both read the same value; moving to the variable later changes no caller.
- `debug_console_write()` with a pointer and a length, not one byte per call: logz4 hands a whole line, and the length is what the UART loop iterates over anyway.

riscv defines neither call yet, and its vmours does not link yet either; step 1b writes riscv's (`.agents/tasks/riscv-kernel/PLAN.md`).

**Reference**
Zircon keeps the kernel's debug console on the platform layer: `platform_dputs_thread()` and `platform_dputs_irq()`, declared in `zircon/kernel/include/platform/debug.h` and defined in `zircon/kernel/platform/debug.cc` over the UART driver phys hands over, with `platform_dputc()` built on them. The counter's rate is `ticks_per_second()`, declared in `zircon/kernel/include/platform/timer.h:157` and defined in `zircon/kernel/kernel/timer.cc:105`. Linux's early console has the same split: the generic `printk` machinery owns the locking and buffering, and each `struct console`'s `write` callback (for the PC's UART, `serial8250_console_write()`) only moves bytes.

**Verification**
- x86 `ours-verify`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (152 passed).
- The x86 dbg vmours defines the same symbols as before, plus exactly two: `ours::arch_tick_hz()` and `ours::platform::debug_console_write(char const*, unsigned long)` (`llvm-nm -C --defined-only`).
- `kshell_run.py "lockup status" "crash panic-locked"`: both ok. `lockup status` prints each CPU's console account in milliseconds, so the rate reaches it (cpu0: 9313 lines, 162 waits, 2.635 ms in all). `crash panic-locked` panics while holding the console lock: the last line before the wait is stamped 6.192 s and the panic's message 6.442 s, the 250 ms bound, after which the other CPUs' lines come out interleaved, as panic mode intends.
- riscv phys check: PASS, `handoff ok at 0.1s`.
- riscv probe: 145 failing compiles, one more than before, and the new one is the point: `kernel/lib/logz4/kernel.cpp` is compiled for riscv now instead of being preprocessed to three empty functions, and it stops at `'ours/arch/cpu_local.hpp' file not found` like the rest. `clang -H` of it opens nothing under `kernel/arch/x86`.
- `gn check` on `//kernel/lib/logz4/*` and `//kernel/platform/*`: OK. clang-format clean on the changed lines (it regrouped `kernel.cpp`'s include block, which the removed `#if` had split in two); clang-tidy reports nothing in the changed files.

**Deliberately not done**
- riscv's `debug_console_write()` and `arch_tick_hz()`: step 1b, with the console decision put to the user (SBI putchar or the device tree's UART).
- `arm`'s platform has neither call either. Its kernel does not build for far more basic reasons, and nothing in this plan builds it.
- `arch::Tick`'s field is still called `tsc` on every architecture. A rename touches every caller of the counter and is not part of this change.

**How it was found**
The research for step 1 (2026-10-29) read every `#if defined(ARCH_*)` in the code vmours links; this was the only one in generic code, and its `#else` branch was empty.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why is an `#else` branch of empty stubs worse for a bring-up than no `#else` at all?
- Which parts of `SerialLogger` are about the machine, and why does the lock stay in logz4 rather than move into `debug_console_write()`?
- Why is a panic-wait bound written as a count of `arch::Tick` wrong on a second architecture, and what does the new code do before the counter's rate is known?

## Title: arch_tick_hz()'s contract says what the console logger needs of it: no lock, no CPU-local state, and a rate known before a second CPU starts (2026-10-29)

**Files**
- `kernel/platform/include/ours/platform/timer.hpp` — `arch_tick_hz()`'s comment says where the logger calls it from, what it may therefore not do, and when a platform must know the rate
- `kernel/platform/pc/timer.cpp` — the PC's definition says it is known before any other CPU starts, and stays 0 if the calibration failed

**Symptom**
Latent, cannot be triggered today: the PC's definition is a load of a variable the APIC calibration writes once, and riscv has no definition yet. It would be triggered by step 1b's riscv definition if it read the device tree's `timebase-frequency` through anything that takes a lock or reads CPU-local storage: a panic inside a console write would then deadlock on that lock, or fault before CpuLocal is set up, and print nothing.

**Cause**
The objects:
- `arch_tick_hz()`, the platform's answer to "how fast does `arch::Tick` count".
- logz4's `SerialLogger::write()`, which reads it through `tick_period()` for every line, and `acquire()`, which reads it through `panic_lock_ticks()` in panic mode.

Why it was a mistake. The previous entry made logz4 call `arch_tick_hz()` from the hardest context the kernel has: interrupts off, inside an NMI handler or a panic, and from the first line of the boot, before `InitLevel::CpuLocal`. Its sibling `debug_console_write()`, written in the same change, says in its contract exactly what that context forbids ("no memory, no CPU-local storage, no interrupt, no lock"); `arch_tick_hz()`'s contract said only what the number means. The requirement on a platform's timing of the rate, that it be known before a second CPU runs, was written only in logz4's private comment above `ASSUMED_TICK_HZ`. A contract is what the next implementer reads; a caller's comment in another directory is not. Step 1b is the next implementer.

**Change**
Before, the end of the comment above `arch_tick_hz()`:

```cpp
/// It is what turns a span measured in arch::Tick counts into a time. A
/// caller that may run before the rate is known -- a panic in early boot --
/// has to have an answer for 0.
```

After, two paragraphs follow it:

```cpp
/// The kernel's console logger (kernel/lib/logz4) calls it for every line it
/// writes, with interrupts off, from an NMI or a panic, and from the first
/// line of the boot, before CPU-local storage exists. So it takes no lock,
/// allocates nothing and reads no CPU-local state.
///
/// A platform that can measure the rate knows it before any CPU but the boot
/// CPU starts: while it answers 0, only the boot CPU runs. A platform that
/// could not measure it answers 0 for good (a PC whose local APIC timer
/// calibration failed).
```

The review's suggested wording said "it answers non-zero before any second CPU starts" without the exception. That is false on a PC: `init_bsp_apic_timer()` returns early when `apic_calibrate_clock()` fails, `TSC_HZ` stays 0, and the other CPUs are started at `InitLevel::Platform` all the same. So the contract names the one way a platform may answer 0 with several CPUs running, and the PC's definition says it can happen there.

**Reference**
Zircon's `ticks_per_second()` (`zircon/kernel/include/platform/timer.h:157`) is a read of `gTicksPerSecond`, which `timer_set_ticks_to_time_ratio()` (`zircon/kernel/kernel/timer.cc`) sets once, early, from the platform's timer code; that is the same "a load of a value set once" this contract asks for, there by construction rather than by a written rule. Its name and shape here, against Zircon's, are put to the user separately (`.agents/tasks/riscv-kernel/PLAN.md`).

**Verification**
- x86 `ours-verify` and the riscv phys check: in the commit message; a comment-only change.
- `git grep -n arch_tick_hz`: three callers, all in `kernel/lib/logz4/kernel.cpp` (`tick_period()`, `panic_lock_ticks()`, `log_console_waits()`), each in one of the contexts the comment names or a milder one.

**Deliberately not done**
- The name: whether it stays `arch_tick_hz()` or becomes `ticks_per_second()` or a `platform_` name, and whether it is a per-platform function or a generic variable the platform sets, is a decision for the user, before step 1b writes riscv's definition.

**How it was found**
Review of step 1a: the reviewer compared `arch_tick_hz()`'s contract with the contexts logz4 calls it from and with `debug_console_write()`'s contract.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why may `arch_tick_hz()` not take a lock, when it is only ever read?
- What goes wrong in a panic if riscv's `arch_tick_hz()` reads a CPU-local copy of the rate?
- Why can a PC answer 0 after its other CPUs have started, and what does the contract say about it?

## Title: A panic does not wait for the console lock while the counter's rate is unknown, and the bound is typed as a duration (2026-10-29)

**Files**
- `kernel/lib/logz4/kernel.cpp` — `panic_lock_ticks()` answers 0 while `arch_tick_hz()` does; `ASSUMED_TICK_HZ` is gone; `PANIC_LOCK_WAIT_MS`, a `u64`, becomes `PANIC_LOCK_WAIT`, an `ours::Milliseconds`; includes `<ours/time/types.hpp>` for it

**Symptom**
On x86, a panic inside a console write before the local APIC calibration (`InitLevel::Irq + 4`) printed its message 250 ms after the line it interrupted; it now prints at once. Nothing else on x86 changes: after the calibration the bound is 250 ms, as before. On riscv, once step 1b defines `arch_tick_hz()` from the device tree, the same panic before the device tree is read would have waited 250 million counts of a 10 MHz counter, 25 seconds, before printing anything.

**Cause**
The objects:
- `SerialLogger::m_lock`, the test-and-set lock that keeps one line together on the wire, and `acquire()`, which in panic mode waits for it at most `panic_lock_ticks()` counts and then gives up for good (`m_abandoned`).
- `arch_tick_hz()`, the counter's rate, 0 until the platform knows it. Its contract (previous entry) says that while it answers 0 only the boot CPU runs, the one exception being a PC whose calibration failed.

The events, in a panic before the rate is known:
1. The boot CPU is inside `write()`, holding `m_lock`, when it faults (or an NMI arrives on it) and the handler panics.
2. The panic's first line calls `write()` again on the same CPU. `acquire()` finds `m_lock` taken.
3. The only CPU running is this one, so the holder is this CPU's own outer `write()`, which will never run again. No wait can free the lock.
4. The old code then waited 250 million counts (`ASSUMED_TICK_HZ`, 1 GHz, times 250 ms) before giving up.

Why it was a mistake. The 1 GHz assumption was chosen so that x86 under QEMU, whose TSC runs at exactly 1 GHz, waited exactly as it had before the change made the bound a time. That preserved a wait whose own comment said it is "lost" whenever it is taken: the earlier entry argued the wait cannot help before the rate is known and kept it anyway. A wait that can only be lost is pure delay, and it is a delay measured in counts, which is the very thing the previous change set out to remove: on a 10 MHz counter it is a hundred times longer than on x86. The right bound when no wait can help is none.

The bound's unit was in its name (`_MS`) and in the arithmetic, not in its type. The kernel has a typed duration for exactly this (`ours::Milliseconds`, `ustl::chrono`), as the tree's other time constants use it (`INIT_TO_STARTUP` and `STARTUP_TIMEOUT` in `arch/x86/kernel/mp.cpp`, the intervals in `commands/hotplug_test.cpp`).

**Change**
Before:

```cpp
constexpr auto PANIC_LOCK_WAIT_MS = u64{250};
constexpr auto ASSUMED_TICK_HZ = u64{1'000'000'000};

auto panic_lock_ticks() -> u64 {
    auto const hz = ours::arch_tick_hz();
    if (hz == 0) {
        return ASSUMED_TICK_HZ / 1000 * PANIC_LOCK_WAIT_MS;
    }
    return hz / 1000 * PANIC_LOCK_WAIT_MS;
}
```

After:

```cpp
constexpr auto PANIC_LOCK_WAIT = ours::Milliseconds(250);

auto panic_lock_ticks() -> u64 {
    return ours::arch_tick_hz() / 1000 * static_cast<u64>(PANIC_LOCK_WAIT.count());
}
```

A rate of 0 now gives a bound of 0, and `acquire()` with a bound of 0 tries the lock once: the first failed test-and-set finds `elapsed >= 0`, sets `m_abandoned` and returns false, so the panic writes without the lock at once and every later write tries it once and does not wait.

The one case the new bound handles worse: a PC whose APIC calibration failed starts its other CPUs with the rate still 0. A panic there that finds the lock held by another, live CPU no longer waits for that CPU to finish its line, so the panic's first line may come out interleaved with it, up to 512 bytes. Before, it waited 250 million TSC counts, 50 to 250 ms. Interleaved output can be read; that machine has already logged `APIC calibration: ...` as an error; and a bound in counts would be a guess there anyway.

Rejected:
- Keeping a fallback rate but choosing a slower one: any assumed rate is wrong on some counter, and before the rate is known the wait cannot help on any platform but a failed PC.
- Waiting only when more than one CPU is online, read from `Cpu::online_mask()`: it would serve only the failed-calibration PC, and adds a read of CPU state to the panic path for it.
- Having riscv hard-code its rate so it is known from the first line: the rate is the board's (`timebase-frequency`), and step 1b has no reason to guess it once the wait no longer depends on it.

**Reference**
None, this project's own design: the panic-mode bound is logz4's (see the first entry in this file). The duration type is `std::chrono::milliseconds`, which `ustl::chrono::Milliseconds` aliases.

**Verification**
- x86 `ours-verify` and the riscv phys check: in the commit message.
- `kshell_run.py "lockup status" "crash panic-locked"`: both ok. `lockup status` still prints each CPU's account in milliseconds (cpu0: 9270 lines, 149 waits, 3.106 ms in all). `crash panic-locked`, which panics after the calibration with the console lock held: the last line before the wait is stamped 6.076 s and the panic's message 6.327 s, the 250 ms bound, as before this change.
- The bound of 0 before the calibration: not measured, since nothing in the tree panics inside a write that early; by reading `acquire()`, as above.
- clang-tidy reports nothing in `kernel/lib/logz4/kernel.cpp`; clang-format clean on the changed lines.

**Deliberately not done**
- The name and shape of `arch_tick_hz()`: put to the user before step 1b (`.agents/tasks/riscv-kernel/PLAN.md`).

**How it was found**
Review of step 1a: the reviewer read the old fallback's comment ("the whole wait is lost") against the 25 s it computes on riscv's 10 MHz counter, and checked in `local_apic.cpp` that the PC's rate is written before any other CPU starts. Checking that, this change found the failed-calibration case, in which it is not.

**The user's prediction**
None

**Three questions you should be able to answer**
- Before the counter's rate is known, who can be holding the console lock when a panic's first line asks for it, and why does that make any wait useless?
- What does `acquire()` do with a bound of 0, step by step, and what does `m_abandoned` do to every later line?
- On a PC whose APIC calibration failed, what can a panic's output look like now that it could not before, and why was that accepted?
