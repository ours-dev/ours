# kernel-panic outstanding repairs

## 2. The serial logger's lock can keep a panic from printing anything

**Severity**
Medium: not reached today, but if it is, the panic prints nothing and the machine just stops.

**Symptom**
Latent, cannot be triggered today. If the serial logger's lock is held when a panic starts, the panicking CPU spins on it forever in its first write. Neither the ring replay nor the message ever appears.

**Cause**
`SerialLogger::write()` (`kernel/lib/logz4/kernel.cpp`) takes a test-and-set lock with interrupts disabled around the polled COM1 output, and the panic path writes through the same logger. The lock can be held at panic time in two ways:
- The panicking CPU itself was inside `write()`, and an exception or NMI there led to `panic()`.
- `IpiService::suspend_others()` is best effort by design: it waits a bounded time and goes on. A CPU that holds the lock with interrupts off and never answers keeps it held.

**Change**
A panic mode in logz4, entered at the start of `vpanic()`. In it, the serial output does not wait for the lock: it either try-locks and writes anyway after a bounded wait, or bypasses the lock entirely. Interleaving with a CPU that did not stop is acceptable; silence is not. Optionally, `emergency_write()` takes the same unlocked path.

**Reference**
- Linux sets `oops_in_progress` through `bust_spinlocks(1)` in `panic()`. Its 8250 console write then only try-locks the port (`serial8250_console_write()`), and `console_flush_on_panic()` flushes without the console lock.
- Zircon `platform_panic_start()` calls `dlog_panic_start()`, after which the debuglog stops queueing and writes fall through to the kernel console and serial directly.

**Acceptance**
A test panics while the serial logger's lock is held deliberately, from a kshell command that takes it and then panics. It still prints the banner and message on dbg; `ours-verify` passes.

## 3. Two CPUs that panic at once both replay the ring and print

**Severity**
Low: the output is doubled and interleaved; nothing hangs.

**Symptom**
If two CPUs panic close together, each disables its own interrupts and asks the other to stop, which it cannot take. Each then waits out `suspend_others()`'s spin bound and goes on, so two replays and two messages come out interleaved, line by line.

**Cause**
`vpanic()` has no "a panic has already started" guard. Each CPU behaves as if it were the only one.

**Change**
An atomic flag claimed at the start of `vpanic()`. The first CPU to claim it does the report. Any later one halts at once, or prints only its own one-line message after a short marker, without replaying the ring.

**Reference**
Zircon `platform_panic_start()` guards the banner and the dump with `static ktl::atomic<int> panic_started`. Linux `panic()` uses `panic_cpu` (`atomic_try_cmpxchg`) and sends a second panicking CPU into `panic_smp_self_stop()`.

**Acceptance**
A test that makes two CPUs panic together shows one banner and one replay on dbg.

## 4. The replay repeats lines the console already showed, with nothing marking where it starts

**Severity**
Low: noise, and a report that is hard to read.

**Symptom**
After the logger is installed, every line that reaches the ring has also been written to the console as it happened. So a panic's replay prints those same lines a second time, straight after them in the log and with no boundary, and a reader cannot tell where the live log ends and the replay begins. Only lines from before the logger was installed are new in a replay; they reached the ring and, except for Error lines, nothing else.

**Cause**
`dump_recent()` replays every surviving record unconditionally. It cannot tell which records already reached the console, and the panic path adds no marker around the replay. Depends on nothing. The panic message now comes before the replay (docs/changes/261003/kernel-panic.md), so the replay is the last thing a panic prints.

**Change**
Bracket the replay with marker lines (for example "--- recent log ---" and "--- end of recent log ---"). Then either replay only the records that never reached a logger, or limit the replay to the last N lines and say so in the marker; record in the ring whether a line was written live, so the dump can choose. Keep the full replay for the case where no logger was ever installed (an early panic), which is the case the ring exists for at panic time.

**Reference**
Linux does not replay the kernel log onto the console at panic: the console has printed it already, and `kmsg_dump(KMSG_DUMP_PANIC)` sends it to persistent storage instead (pstore, item 5). Zircon's `BluescreenInit()` leaves a "replay debuglog?" note and does not replay.

**Acceptance**
A panic after boot prints the replay between its markers and without the lines the console already showed, or as a bounded tail that says so. An early panic, with no logger, still replays everything; `ours-verify` passes.

## 5. No crash record survives the panic

**Severity**
Low: nothing reads one today, because a panic halts and never reboots; it matters once a panic reboots the machine.

**Symptom**
Latent, cannot be triggered today. A panic leaves its report only on the serial console. Nothing stores the message, the registers, the backtrace or the log tail anywhere a later boot could read them.

**Cause**
There is no crash-log area and no reader for one. `vpanic()` ends in `arch::halt()`, so the question has not arisen.

**Change**
When panic gains a reboot option: reserve a small region of RAM the next boot leaves untouched (QEMU keeps guest RAM across a warm reset), write a header, the message and the log tail into it at panic, and hand it to user space on the next boot. Design it before building; it is a feature, not a fix.

**Reference**
- Zircon's crashlog (`zircon/kernel/lib/crashlog/`, `crashlog_to_string()`) is written into a persistent RAM buffer and handed to user space after the reboot.
- Linux pstore with the ramoops backend (`fs/pstore/ram.c`) receives `kmsg_dump(KMSG_DUMP_PANIC)` and shows the previous boot's log under `/sys/fs/pstore`.

**Acceptance**
After a panic and a warm reboot, the next boot can read the previous panic's message from the reserved region.
