# kernel-sync outstanding repairs

## 1. MutexStress.AllWorkersFinishBeforeTheTimeout failed once in a run twelve times slower than normal

**Severity**
Low: one failure, not reproduced; it says nothing yet about the mutex.

**Symptom**
On 2026-11-15, `ktest all` on the default machine (`feat/pmm-redesign`, while building review item M5 of `.agents/tasks/pmm-redesign/REVIEW.md`) failed `kernel/tests/mutex_stress.cpp:150`: "all three workers finish before the timeout; leaking the state". The whole run took about 628 s where it takes about 52 s. The test alone passed twice right after (4.2 s each), and two more full runs passed in normal time. The log line that says how many workers finished and who held the mutex was overwritten by those reruns before anyone read it.

**Cause**
Not established. The test gives its workers 20 s (`STUCK_AFTER`) for work that takes about 4.2 s; a guest running twelve times slower needs about 50 s, so a stalled host explains the failure. A worker parked and never woken, a lost wakeup, would look the same from outside.

**Change**
When it fails again, keep the kernel log: the `mutexstress: STUCK` line names the holder and how many workers finished. If the holder is null and the run was slow, scale `STUCK_AFTER` to how long the work takes on the guest, or skip the check under an emulator the way `bench::Run` skips hardware-only metrics; if a worker is parked behind a held or released mutex, it is a wakeup bug.

**Reference**
None, this project's own test.

**Acceptance**
`ktest sel=MutexStress.*` passes fifty runs in a row on the default and the four-node machine, or the next failure's STUCK line is read and the cause written here.
