# kernel-commands outstanding repairs

## 1. `mutex.stress` calls a slow run stuck on the dbg image

**Severity**
Low: a false failure of a test, on the dbg image, when the host is busy.

**Symptom**
`mutex.stress` (`kernel/commands/mutex_stress.cpp`, 20000 rounds by 3 threads by default) reported "STUCK -- 0/3 workers finished after 20 s" on the dbg image twice on 2026-10-03, while the host's load average was 5 to 7. The workers had not stopped: the log shows them handing the mutex over about 900 times a second per CPU, and they finished after the report. A quiet host runs the same test in about 18 s, and a HEAD built alongside behaved the same, so no change caused it.

**Cause**
`STUCK_AFTER` is a fixed 20 s on the run's total time. Its comment says the default run takes about three seconds under QEMU's TCG. That is no longer true of the dbg image, with its assertions and lock checking, which needs about 18 s: a 10% margin, which a loaded host eats. A bound on total time also cannot tell a slow run from a stuck one, which is the only question it is there to answer.

**Change**
Call the run stuck when no worker has finished a round for some seconds, not when the whole run passes a fixed time. Each worker bumps a shared count per round; the poller records when the count last moved and reports STUCK only after it has not moved for, say, 5 s. Keep an overall cap only as a backstop, well above any real run. Correct the comment's timing. Ruled out: raising the fixed bound, which only moves the cliff.

**Reference**
Linux's hung-task detector (`kernel/hung_task.c`) and soft-lockup watchdog (`kernel/watchdog.c`) both judge progress, not total time: a task in uninterruptible sleep that has not been switched for a timeout, a CPU that has not touched its timestamp for one.

**Acceptance**
- `mutex.stress` passes on dbg while the host is kept busy (two parallel builds).
- A variant whose workers stop handing over (a debug knob that makes one hold the mutex forever) is still reported STUCK within the progress timeout.
- `ours-verify` passes.
