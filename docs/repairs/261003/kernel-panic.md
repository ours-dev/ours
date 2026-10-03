# kernel-panic outstanding repairs

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
