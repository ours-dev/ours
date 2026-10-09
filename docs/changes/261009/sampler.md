# sampler changes

## The shell stress-tests a session's stop, restart and release, and the dev shell takes its Sampling resource from /svc (2026-10-09)

**Files**
- `system/app/shell/shell_tests.cpp` — `check_sampler_stress()`: 16 sessions at the shortest period, each run twice while this thread makes system calls and spins in user code; every read checked record by record; called from `check_sampler()`.
- `system/app/shell/shell.cpp` — as a component (dev), the shell takes the SAMPLING resource from `/svc/ours.resource.Sampling`, so `check_sampler()`'s session half runs on dev too.

**Symptom**
None, a new check. Before it, nothing exercised a session's stop, restart or release while writers were running, and on dev the shell skipped every session check: it had no startup handle for SAMPLING, which only userboot hands out (dbg_1).

**Cause**
The writers' gate (`Session::ACTIVE`, b7e29da2) is correct by argument: every writer loads ACTIVE and writes inside one interrupts-off section, and `stop_locked()` clears ACTIVE and then waits on a `sync_exec()` IPI to every CPU, which a CPU takes only outside that section (`docs/changes/261007/thread-sampler.md`). An argument is what proves it; a test can only show it broken. This one is built to show three ways it could break:
- A writer that finished after the barrier, or a read under one: a record with a wrong magic, depth, CPU, flag or pid, or a header whose count disagrees with what was copied.
- A restart that did not empty the buffers: a record whose timestamp is not after every record of the run before.
- Closing a running session: the release stops it under the writers and gives up the claim, and the next create must succeed.

**Change**
Each session runs twice, and the second run of every other session ends by closing the running session:

```cpp
for (auto session = 0; session < SESSIONS && ok; ++session) {
    // sampler_create ...
    for (auto run = 0; run < RUNS_PER_SESSION && ok; ++run) {
        // sampler_start, then system calls and user arithmetic
        if (session % 2 == 1 && run == RUNS_PER_SESSION - 1) {
            ok &= ours::handle_close(handle) == Status::Ok; // the release, under the writers
            closed = true;
            break;
        }
        // sampler_stop, sampler_read, then every record:
        //   ... && record.timestamp > previous_latest;
    }
}
```

The first version, written on 2026-10-07, created a new session for every run. Its timestamp check then guarded nothing: a new session's buffers are fresh, `attach()` empties them, and the reset in `Session::start()` never ran on a buffer with records in it. Removing that reset passed the boot. Running each session twice is what makes the restart, and so the check, real.

The dev shell's fetch, beside its Debug one:

```cpp
if (auto const fetched = ours_resource::get(ours_resource::SAMPLING_PROTOCOL); fetched.is_ok()) {
    sampling = fetched.unwrap();
}
```

The check was held back from 2026-10-07 until the boot CPU's LINT0 was masked (8297816a): before that, a slow dbg boot took up to 56 s to run the shell's checks, and the stress test's extra 9 s pushed some over `boot_check.py`'s 60 s timeout.

**Reference**
None, this project's own test; the barrier it tests is the one argued in `docs/changes/261007/thread-sampler.md`.

**Verification**
- clang-format clean on the changed lines; clang-tidy (`out/bringup.x86`) reports nothing on them.
- In `.worktrees/wip-proc-self` at baseline `9b97121c` plus only this change: build ok, `dbg PASS 35 passed, 3 skipped, 0 failed`, `rel PASS 33 passed, 8 skipped, 0 failed, 6/6 required` (9 skipped before: dev's session checks now run), host tests 144 passed. `shell: sampler stress: 32 runs in 16 sessions, 59 records checked` on dbg, 58 on rel.
- With the reset in `Session::start()` removed: `rel FAIL`, `shell: sampler stress: run 1 record 0 is not a whole record of this run`. With the first version (a session per run) the same mutation passed.
- Not measured: whether it ever meets a writer mid-write. Timers fire only on the 10 ms tick, so a run of a millisecond or two takes at most one sample per CPU, and a stop lands on a write in roughly 0.2% of runs.

**Deliberately not done**
- Longer runs (at least three ticks each, about a second more per boot) and an on-demand stress program with thousands of runs: the user kept this a boot-time smoke check (2026-10-09) until timers fire at their own deadlines; `docs/repairs/261009/sampler.md` item 1.

**How it was found**
None, a new check. The weakness of its first version was found by mutation: removing `m_recorder.reset()` from `Session::start()` and booting rel.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why did the first version's timestamp check guard nothing, and what about a fresh session made the restart's reset unreachable?
2. Why can this test show the stop barrier broken but never show it correct, and what does show it correct?
3. Why does the check meet a writer mid-write so rarely today, and what has to change in the kernel before it can?
