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

## Title: only the first CPU to panic prints a report; a later one leaves its message in the ring and stops (2026-10-03)

**Files**
- `kernel/top/panic.cpp` — `panic_started` and `claim_report()` elect the reporter; a CPU that loses logs its message into the ring only (`RingOnly`) and stops in `stop_this_cpu()`
- `kernel/commands/crash.cpp` — `crash panic-smp`: every online CPU meets in a `sync_exec()` task and panics at the same moment
- `scripts/kshell_run.py` — after a crash command's first lines the console is read for `PANIC_SETTLE` (10 s) more, and the banner must have come out exactly once
- `docs/repairs/261003/kernel-panic.md` — item 3 removed

**Symptom**
With `crash panic-smp` on the old code, all four CPUs printed a full report. Both images showed four banners within 10 s. In the rel log the reports came one after another: cpu0's banner, message and replay, then cpu3's, then cpu2's, then cpu1's, each replay ending with the messages of every report before it. Nothing hung, but the first report, the one that matters, was followed by three more that looked just like it.

**Cause**
`vpanic()` had no notion that a panic might already be under way. Each panicking CPU behaved as if it were the only one:
- It disabled its own interrupts, so it could not take the others' suspend IPIs.
- It called `suspend_others()`, which sent IPIs nobody could take and then waited out its whole bound. The CPUs left that wait at slightly different moments, which is why the reports came out one after another rather than mixed.
- Each then printed a banner, a message and a full replay.

The repair item expected line-by-line interleaving. The serial lock keeps lines whole, and the staggered exits from `suspend_others()` turned that into four whole reports instead. Either way the report is not one report, and a tool counting banners counts four panics.

**Change**
One atomic flag, claimed right after interrupts go off:

```cpp
std::atomic<bool> panic_started{false};

auto claim_report() -> bool {
    return !panic_started.exchange(true, std::memory_order_relaxed);
}
```

```cpp
arch::AutoInterruptDisabler local_irq_disabler{};
if (!claim_report()) {
    RingOnly ring_only;
    log::vlog(ring_only, logz4::Level::Error,
              std::string_view("panicked while another panic was under way: {}"),
              std::make_format_args(message));
    stop_this_cpu();
}
logz4::enter_panic_mode();
quiesce_machine();
```

The pieces, and why each is enough:
- **The election is relaxed.** The read-modify-writes of one atomic form a single total order (its modification order) whatever memory order each uses, so exactly one `exchange()` reads false. The flag publishes nothing else, so it needs no acquire or release.
- **A loser prints nothing, but its message is not lost.** It logs into a sink that drops every line (`RingOnly`), so the line goes into the record ring and nowhere else. The winner's replay then shows it in time order. The ring needs no lock (`ring::append()` claims a slot by CAS), so this is safe from any CPU.
- **A loser stops the way a suspended CPU does.** `stop_this_cpu()` clears its Online bit and halts, as `x86_handle_ipi_suspend()` does. It has interrupts off and will never take the winner's IPI, so without the bit `suspend_others()` would wait out its whole bound for it.
- **The ring line reaches the replay through the Online mask.** The loser appends to the ring, then clears its bit with a release CAS. `suspend_others()` reads the mask with an acquire, and returns only once the bit is clear. So by the time the winner replays, the loser's slot is committed and visible.
- **The claim comes before panic mode and before quiesce.** A loser must not stop other CPUs or touch the console at all.

A panic inside the panic path, on the winning CPU itself, also loses the election: its message goes to the ring and the CPU stops. Before, it would have started the whole report again from the top, and if the report itself was what faulted, it would recurse until the stack ran out.

The test, `crash panic-smp`, gives every online CPU a `panic_together()` task through `IpiService::sync_exec()`: the others run it from their interrupt handlers, and this CPU runs it after the IPIs are out. Each task counts itself in and spins until all have arrived, then panics, so the panics start within a few instructions of each other. Without the rendezvous they would start an IPI's latency apart, and the first CPU could stop the others before they reached `panic()`. The rendezvous is unbounded on purpose: panicking anyway after a timeout would let the test pass without the panics having overlapped.

Options considered:
- Print the loser's message live, as Zircon's `vpanic()` does after `platform_panic_start()`. Rejected: it lands somewhere in the middle of the winner's report, and the ring puts it in the replay in its right place for free.
- Let a nested panic on the same CPU carry on, as Linux's `panic()` does when `panic_cpu` is already this CPU. Rejected for now: if the report is what faulted, it faults again.

**Reference**
- Zircon `platform_panic_start()` (`zircon/kernel/platform/pc/power.cc`): `static ktl::atomic<int> panic_started`, and `panic_started.exchange(1) == 0` guards `dlog_bluescreen_init()`, the banner.
- Linux `panic()` (`kernel/panic.c`): `atomic_try_cmpxchg(&panic_cpu, &old_cpu, this_cpu)`; a CPU that loses to another CPU calls `panic_smp_self_stop()`, which spins for ever.

**Verification**
- With `claim_report()` forced to true (the guard off), `kshell_run.py "crash panic-smp"`: FAIL on both images, "4 panic banners within 10s".
- With the guard, `crash panic-smp`: ok on both images. On rel, cpu3 won. The replay held the three losers' lines, each "panicked while another panic was under way: crash: ...", from cpu2, cpu1 and cpu0, at TSC 4543156000, 4543162000 and 4543187000. After them came cpu3's own message at 4543365000. One banner, one replay. On dbg, cpu1 won, and cpu3, cpu2 and cpu0 left their lines in the same way.
- `crash panic` and `crash panic-locked`: still ok on both images, now also checked for a single banner.
- `boot_check.py --out out/kernel-panic`: dbg PASS (19 passed, 0 error lines), rel PASS (13 passed, 10 skipped, 0 error lines).
- clang-format clean; clang-tidy reports nothing in the changed files.

**Deliberately not done**
- A nested panic on the winning CPU leaves its message in the ring only, and the report it interrupted is cut short. A one-line note on the console for that case would need the winner's CPU number, and a second guard against the note itself faulting.
- `suspend_others()` still bounds its wait in spins, so under QEMU a CPU that never answers costs it seconds (the previous entry measured a spin at about 640 ns). It was not needed here, because losers now clear their own Online bit.
- The losers' lines are not marked in the replay beyond their text. Bracketing the replay is repair item 4.

**How it was found**
None for the problem, which was recorded by reading the panic path (commit 59a0e665). The four-reports-in-a-row shape was seen by running `crash panic-smp` with the guard disabled, which is also how the test was shown to fail without the fix.

**The user's prediction**
None. The user asked for the code first and the reasoning afterwards, because time was short.

**Three questions you should be able to answer**
- Why is a relaxed `exchange()` enough to elect exactly one reporter, when the CPUs race with no other synchronisation?
- How does a losing CPU's message get into the winner's replay, and which release and acquire make sure it is visible there?
- Why must a losing CPU clear its own Online bit, and what would the winner do if it did not?
