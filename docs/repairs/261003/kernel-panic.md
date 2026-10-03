# kernel-panic outstanding repairs

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
