# kernel-top outstanding repairs

## 1. On dbg, work placed on CPU 0 sometimes runs fifty to a hundred times slower, with the debug threads printing

**Severity**
Medium: no result is wrong, but how long the dbg boot's shell checks take swings from about 29 to 45 s between runs, and a check that does a fixed amount of work under a time-based measurement (`profile`) was starved to one sample short of failing (fixed on its side, `docs/changes/261005/prof.md`, 2026-10-05).

**Symptom**
`boot_check.py --out out/sampler` on dbg, three runs on 2026-10-05 whose logs were kept, host load average about 11 on 15 cores with another session's QEMU running:

- The profile check (10,000 `vmo_create`/`handle_close` pairs and 20 million additions in user code, under a sampling session) took 0.6 s in one run and 19.4 s and 13.3 s in the other two. Its samples show the slow runs on CPU 0 and the fast one on CPU 1, each with its CPU to itself: the other CPUs held the two debug threads and an idle thread.
- In the slow runs the kernel samples of the workload are spread over the ordinary system-call path (`MemCache::free`, `MemCache::NodeCache::take_object`, `ProcessDispatcher::current`) with no hotspot, so the code did not wait on anything: every instruction was slower.
- The other CPUs did not slow down: the debug threads' log rate stayed at about 17,000 lines a second throughout.
- Which checks are slow changes per run: one run had only profile slow; another had process_create, thread_create, handle transfer, port observer limit, handle quota and sampler each one to five seconds slow, and profile fast.

**Cause**
Not known. The debug threads `Sample1` and `Sample2` (`kernel/top/main.cpp`, the `gibberish` lambda) print `I'm Thread N` in a loop with no sleep, so two CPUs write COM1 about 650,000 times a second between them, each byte a port read and a port write. The leading hypothesis is that this costs CPU 0 alone under QEMU's TCG: each port write goes through QEMU's serial device, which may kick the vCPU that ISA interrupts are routed to (CPU 0) out of its translated code even though the guest never enables a UART interrupt. That would slow every instruction on CPU 0 and nothing on CPU 1, as observed. Unconfirmed; the serial logger's lock was ruled out for the kernel part, because the slow samples do not sit at `SerialLogger::write()`.

**Change**
Measure first. Boot dbg a few times with the debug threads removed and compare each check's duration with the runs above; then, to separate the CPU from the threads, boot with them kept and the shell's work pinned to CPU 0 and then to CPU 1. If the threads are the cause, the fix is to stop them flooding the log: remove them, or make each sleep between lines. Their output was the first sign of preemptive round-robin (`docs/changes/260903/scheduler.md`); whether something should keep that role is the user's call.

**Reference**
None, this project's own debug code.

**Acceptance**
Five dbg boots in a row each finish the shell's checks within a few seconds of one another, and each prints `profile: ... 0 dropped`.
