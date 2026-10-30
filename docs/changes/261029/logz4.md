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
