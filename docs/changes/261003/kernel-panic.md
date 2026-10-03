# kernel-panic changes

## Title: a panic prints a fixed banner and its message first, and the ring replay after them (2026-10-03)

**Files**
- `kernel/top/panic.cpp` — both `vpanic()` overloads print the banner, then the message, then the ring replay; the banner and the replay are named helpers
- `kernel/commands/crash.cpp` — new `crash panic` kshell command that panics on purpose, after Zircon's `crash` debug command
- `kernel/commands/BUILD.gn` — builds `crash.cpp`
- `scripts/kshell_run.py` — a `crash` command must be last, is done at the panic banner and the line after it, and is judged on the order the report starts in
- `docs/repairs/261003/kernel-panic.md` — item 1 removed

**Symptom**
On a panic the serial console first printed the whole record ring, up to 256 lines (`logz4::ring::SLOT_COUNT`) of up to 512 bytes each, and only then the one line saying what went wrong. At 115200 baud, about 11.5 KB/s, that is roughly 2 s at 100 bytes a line and up to 12 s at full lines before the reason. Nothing marked where a panic report began, so a tool reading the log could only look for the first error-level line, and the replay can contain error lines of its own.

**Cause**
The old order was chosen so that the context read top to bottom like a log, with the reason at the bottom. That is the right order for reading a log and the wrong one for a panic report, for two reasons:
- A panic report is read for its reason. Everything else is context that only matters once you know what you are looking for.
- The replay is a long, polled serial write made from a machine that is by definition broken. Each byte waits for the UART's transmit-empty bit, and each line goes through `SerialLogger::write()` and its lock (repair item 2). If any of that stalls, everything after the stall is lost. With the message last, the one line that mattered was exactly the one most likely to be lost. Whatever is printed first is what survives.

The missing banner is the second half of the same problem. An error-level line is not a panic: the boot check and `kshell_run.py` flag every error line, and a replay can repeat old ones. Without a fixed line that only a panic prints, a harness cannot tell "the kernel panicked" from "something logged an error", and it cannot count panics either (repair item 3 needs to).

**Change**
Before, in both overloads:

```cpp
arch::AutoInterruptDisabler local_irq_disabler{};
quiesce_machine();

logz4::dump_recent();
log::vlog(logz4::get_global_logger(), logz4::Level::Error, fmt, args);
```

After:

```cpp
arch::AutoInterruptDisabler local_irq_disabler{};
quiesce_machine();

print_banner();
log::vlog(logz4::get_global_logger(), logz4::Level::Error, fmt, args);
print_context();
```

`print_banner()` writes `"\nOURS KERNEL PANIC\n"`. Three choices in it:
- The leading newline ends any half-written line. The kernel shell writes its prompt and echo a byte at a time outside the logger, so the console is often mid-line when a panic starts.
- It is written straight to the sink, not logged. A logged line also goes into the record ring, and the replay that now follows the message would print the banner a second time at its end; a tool counting banners would count two panics. The sink is chosen the way every logz4 line chooses it: the installed logger, or `logz4::emergency_write()` while none is installed (an early panic).
- The message, by contrast, is still logged through `log::vlog()`. That keeps its `[time cpuN error]` prefix, which is how the boot check spots a panic and which tells the reader which CPU panicked and when. The cost is that the message is in the ring too, so the replay ends with it once more (see Deliberately not done).

`quiesce_machine()` stays ahead of the banner. Linux and Zircon both print before they stop the other CPUs, but here the dbg image's other CPUs log thousands of lines a second, and a line of theirs could land between the banner and the message. The suspended CPUs each print one "CPU[n] suspended" line before the banner; that is other CPUs' output, not the panicking CPU's context.

The test is a kshell command, `crash panic`, which calls `ours::panic()` with a fixed message. It is named after Zircon's `crash` debug command, whose sub-commands (`crash assert`, `crash deref`, ...) are the model for what later items add. `scripts/kshell_run.py` learned what a crash command is: it must be the last command, it is done once the banner and the line after it have arrived (there is no prompt to wait for), and it passes only if that line is an error-level line and the CPU named in its prefix printed no non-debug line between the command and the banner. The last check is what makes the old order fail: the replay contains the panicking CPU's own earlier lines.

Rejected: a banner logged at Error level. It would carry a prefix and be found by the existing error-line checks, but it would also be in the ring and be replayed, which defeats counting.

**Reference**
- Linux `panic()` (`kernel/panic.c`) prints `"Kernel panic - not syncing: %s\n"` with `pr_emerg()` before `dump_stack()`, before `kmsg_dump(KMSG_DUMP_PANIC)` and before `smp_send_stop()`.
- Zircon `platform_panic_start()` (`zircon/kernel/platform/pc/power.cc`) calls `dlog_bluescreen_init()`, which prints `"\nZIRCON KERNEL PANIC\n\n"` (`DLog::BluescreenInit()`, `zircon/kernel/lib/debuglog/debuglog.cc`) with a warning that external tools match the string. `vpanic()` (`zircon/kernel/top/debug.cc`) then prints the message. Zircon does not replay its debuglog at all.
- Zircon's `crash` command is in `zircon/kernel/debugcommands/debugcommands.cc`.

**Verification**
- `boot_check.py --out out/kernel-panic`: build ok (28.9 s); dbg PASS, shell finished at 20.7 s, 19 passed, 0 error lines; rel PASS, shell finished at 3.7 s, 13 passed, 10 skipped, 0 error lines.
- `kshell_run.py --out out/kernel-panic --image rel "crash panic"`: ok. The output after the command was three "CPU[n] suspended" lines, then `OURS KERNEL PANIC`, then `[4672246000 cpu1 error] crash: deliberate panic from the kernel shell`.
- The same on dbg: ok, with the banner followed by `[23003525000 cpu0 error] crash: ...`; the lines before the banner were other CPUs' debug noise and their "suspended" lines.
- clang-format clean on the changed lines; clang-tidy reports nothing in `panic.cpp` or `crash.cpp` (only pre-existing warnings in included headers).
- Not run: the phys `vpanic()` (`kernel/phys/panic.cpp`), which was not changed.

**Deliberately not done**
- The replay still ends with the panic message, because the message is logged into the ring before the replay runs, and it still repeats lines the console already showed, with no marker where it starts. Both are repair item 4, which brackets and trims the replay.
- No uptime or CPU line under the banner, as Zircon prints. The message's prefix already carries both.
- The phys `vpanic()` keeps the old order. The repair was about the kernel's, and phys has no ring replay worth the reorder until it gets the same banner.
- Items 2 (the serial logger's lock) and 3 (two CPUs panicking at once) are concurrency decisions and are done separately.

**How it was found**
None. The problem was recorded by reading the panic path (commit 59a0e665).

**The user's prediction**
None. The user marked this item as not a concurrency decision.

**Three questions you should be able to answer**
- Why is the reason printed before the context, when a log reads the other way round? What can stop a replay part way through?
- Why is the banner written straight to the sink while the message goes through `log::vlog()`? What would go wrong with each the other way round?
- Why does `quiesce_machine()` still run before the banner, when Linux and Zircon print first and stop the other CPUs afterwards?

## Title: logz4 gains a panic mode, in which the serial logger stops waiting for its lock after a bound (2026-10-03)

**Files**
- `system/lib/logz4/include/logz4/log.hpp` — declares `enter_panic_mode()` and `in_panic_mode()`
- `system/lib/logz4/src/init.cpp` — defines them over one relaxed `std::atomic<bool>`
- `kernel/lib/logz4/kernel.cpp` — `SerialLogger::write()` takes its lock through `acquire()`, which in panic mode gives up after `PANIC_LOCK_TICKS`, and releases the lock only if it took it; `hold_lock_forever()` and `ours::hold_console_lock_forever()` for the test
- `kernel/include/ours/start.hpp` — declares `hold_console_lock_forever()`
- `kernel/top/panic.cpp` — both `vpanic()` overloads call `logz4::enter_panic_mode()` right after disabling interrupts, before `quiesce_machine()`
- `kernel/commands/crash.cpp` — `crash panic-locked`: take the serial logger's lock, then panic
- `docs/repairs/261003/kernel-panic.md` — item 2 removed

**Symptom**
Latent, cannot be triggered today without the new test. If the serial logger's lock was held when a panic started, the panicking CPU spun on it for ever in its first write, and the panic printed nothing at all: no banner, no message, no replay. With the new `crash panic-locked` test and the old `write()`, the machine goes silent after the command.

**Cause**
`SerialLogger::write()` serializes whole lines with a test-and-set lock, held with interrupts off while it polls COM1 a byte at a time. Every line goes through it: ordinary log lines, `emergency_write()`, and the panic's banner, message and replay. The lock protects one invariant, that one line's bytes reach the wire together. Without it, two CPUs each poll the transmitter, each see it empty, and their bytes alternate.

An ordinary lock waits for its holder because the holder will finish. At panic time that assumption fails in two ways:
- The panicking CPU holds the lock itself. It was inside `write()` when an exception (or, later, an NMI) led to `panic()`. Its first panic write spins on a lock whose holder is the spinner: a self-deadlock, with interrupts already off.
- Another CPU holds it and never lets go. It holds the lock with interrupts off, so it cannot take the suspend IPI. `IpiService::suspend_others()` gives up on it after its bound, as designed, and the panicking CPU's first write then waits for it for ever.

In both cases waiting longer cannot help, and the cost of waiting is the whole report. A lock that keeps output tidy must not be allowed to stop the output, and the panic path is where that trade reverses: interleaved output can be read, and no output cannot.

**Change**
Panic mode is one flag in the logz4 core, set by `vpanic()` before anything prints and never cleared:

```cpp
arch::AutoInterruptDisabler local_irq_disabler{};
logz4::enter_panic_mode();
quiesce_machine();
```

The flag is relaxed. It publishes nothing but itself: the panicking CPU reads it back in program order, and a CPU that sees it late just waits in the ordinary way a little longer. It comes before `quiesce_machine()` because the stopping CPUs print a "CPU[n] suspended" line on their way out. With the lock held by a dead holder they would hang in that write, never clear their Online bit, and hold `suspend_others()` up for its full bound.

`SerialLogger::write()` before:

```cpp
while (m_lock.test_and_set(std::memory_order_acquire)) {
}
// ... poll and write every byte ...
m_lock.clear(std::memory_order_release);
```

After:

```cpp
auto const locked = acquire();
// ... poll and write every byte ...
if (locked) {
    m_lock.clear(std::memory_order_release);
}
```

`acquire()` waits for ever outside panic mode, as before. In panic mode it waits at most `PANIC_LOCK_TICKS` (250 M TSC ticks: 50 ms at 5 GHz, 250 ms at 1 GHz), and on timeout sets `m_abandoned` and returns false, so the write goes ahead without the lock. Three points carry the design:
- **Only the owner releases.** A write that went ahead without the lock must not clear it. Clearing a lock someone else holds lets a third CPU in while the holder is still writing, and the holder's own clear later lets in a fourth: mutual exclusion would be gone for everyone, not just for that one write. Leaving the lock as it was found also keeps CPUs that are spinning on it in the ordinary loop stuck there, which is as good as stopped.
- **The bound is one line long.** A live holder finishes any line (at most 512 bytes, about 45 ms at 115200 baud) inside the bound, so the usual case keeps whole lines. Only a holder that outlasts a full line is treated as gone.
- **The bound is paid once.** After a timeout, `m_abandoned` makes every later panic-mode write try the lock once and not wait. Without it a self-held lock would cost one bound for each of the up to 256 replay lines.

The bound is in TSC ticks, not spins. The first version spun 10 M times, as `suspend_others()` does. Under QEMU's TCG one spin of that loop measured about 640 ns, and `crash panic-locked` on rel held the report up for 6.4 s. A spin's cost varies by more than a hundred times between cores and emulators; the TSC rate varies by about five.

Options considered:
- Bypass the lock entirely in panic mode. Rejected: when the lock is free or its holder is alive, nothing is gained by ignoring it, and the panic's lines can interleave with a live CPU's for no reason.
- Try the lock once, then write anyway (Linux's `serial8250_console_write()`). Nearly as good, since `quiesce_machine()` runs first and responsive CPUs have finished their lines and stopped by then. But the flag is global, and the stopping CPUs print their "suspended" lines at the same moment. With a single try, two of those lines interleave whenever they collide, and in the rel log two of them were formatted 14 000 TSC ticks apart.
- Make only the panicking CPU skip the lock, as Linux's `other_cpu_in_panic()` keeps other CPUs off the consoles. Not done: `write()` would have to ask which CPU it runs on, which reads CpuLocal and does not work for an early panic. The bounded wait gives the other CPUs whole lines whenever the holder is alive anyway.

`emergency_write()` goes through `g_serial.write()`, so it takes the same path with no change. The record ring has no lock to worry about: `ring::append()` claims a slot by CAS, and a panic during an append takes the next ticket, so a different slot.

The test, `crash panic-locked`, calls `ours::hold_console_lock_forever()`, which disables interrupts and takes the lock as `write()` would, then panics. This is the self-held case. On dbg it is also the other case: the other CPUs log thousands of lines a second, so they pile up spinning in the ordinary loop with interrupts off, never answer the suspend IPI, and `suspend_others()` waits out its bound.

**Reference**
- Linux `panic()` (`kernel/panic.c`) calls `bust_spinlocks(1)`, which raises `oops_in_progress`. `serial8250_console_write()` (`drivers/tty/serial/8250/8250_port.c`) then takes the port lock with `uart_port_trylock_irqsave()` instead of `uart_port_lock_irqsave()`, writes whether or not it got it, and unlocks only if it locked. `console_flush_on_panic()` flushes without the console lock.
- Zircon `platform_panic_start()` (`zircon/kernel/platform/pc/power.cc`) calls `dlog_panic_start()`, which sets `DLog::panic_` (`zircon/kernel/lib/debuglog/debuglog.cc`) so that writes stop going through the debuglog and fall through to the kernel console and serial directly.

**Verification**
- `boot_check.py --out out/kernel-panic`: dbg PASS (19 passed, 0 error lines), rel PASS (13 passed, 10 skipped, 0 error lines).
- `kshell_run.py --image rel "crash panic-locked"`: ok. The three "CPU[n] suspended" lines were formatted at TSC 4.57 G, the panic message at 4.82 G: one bound of about 0.25 s. Two of the suspended lines came out byte-interleaved, because both CPUs timed out together and wrote without the lock, which is the trade this change makes.
- `kshell_run.py --image dbg "crash panic-locked"`: ok. The banner and the message came out whole after the other CPUs' last debug lines.
- `crash panic` on both images: still ok.
- clang-format clean; clang-tidy reports nothing in the changed files.
- Not measured: the bound on real hardware.

**Deliberately not done**
- No CPU-specific bypass (see Change). The visible price is the interleaved "suspended" lines in the locked case.
- The ordinary, non-panic path is unchanged: it still waits for ever, which is right while every holder is alive.
- Two CPUs panicking at once is repair item 3, done separately.

**How it was found**
None. The problem was recorded by reading the panic path (commit 59a0e665). The first bound, 10 M spins, was replaced after `crash panic-locked` on rel took 6.4 s.

**The user's prediction**
None. The user asked for the code first and the reasoning afterwards, because time was short, so no argument was written in advance.

**Three questions you should be able to answer**
- Which invariant does the serial logger's lock protect, and which two ways can it be held by a CPU that will never release it at panic time?
- Why must a write that went ahead without the lock leave it set, and what breaks if it clears it?
- Why is the bound measured in TSC ticks and sized to one line, and why is it paid only once per panic?
