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
