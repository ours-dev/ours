# lockup-detector changes

## `lockup status` shows every CPU's heartbeat and every checker's notes, and six boots put the closest a healthy CPU came to the threshold at 0 checks of 10 (2026-10-08)

**Files**
- `kernel/lib/lockup_detector/include/lockup_detector/heartbeat_note.hpp` — a note keeps two high-water marks beside its count: `most_unchanged()`, the longest run of checks without a tick, and `fewest_ticks()`, the fewest ticks the target took between two checks (none until two checks have seen it ticking).
- `kernel/lib/lockup_detector/BUILD.gn` — `:heartbeat_note` reaches `//system/lib/ustl:headers`, for `ustl::Option`.
- `kernel/lib/lockup_detector/test/heartbeat_note_test.cpp` — three host tests of the marks: the longest run survives a tick; the fewest ticks waits for a second check of a ticking target; the leanest check is kept, and a check without a tick makes it 0.
- `kernel/lib/lockup_detector/include/lockup_detector/lockup_detector.hpp` — `log_status()`.
- `kernel/lib/lockup_detector/src/checker.cpp` — `log_status()`: the threshold, then one line per online CPU with its heartbeat and its notes on the others, each CPU's notes copied on that CPU by `copy_notes()` through `IpiService::sync_exec()`; a last line, `Closest`, with the most checks in a row without a tick and the fewest ticks between two checks over every note.
- `kernel/commands/lockup.cpp`, `kernel/commands/BUILD.gn` — `lockup status`.
- `scripts/kshell_run.py` — `lockup status` is done at its last line, as `lockup spin <cpu> <ms>` is at its own (`lockup_end()`, which was `spin_end()`).
- `kernel/lib/lockup_detector/README.md` — step 4: `lockup status`, and what the measurement found.
- `docs/repairs/261007/kernel-smp.md` — item 1: the watchdog is in, and how the next hang reports itself.
- `docs/repairs/261008/kernel-lib-logz4.md` — new: on the dbg image the boot CPU loses 10% to 16% of its ticks.
- `.agents/tasks/lockup-watchdog/DESIGN.md` — Staging step 4 landed with its revisions, Status Implemented; `PLAN.md` — step 4 done; `RESULT.md` — new.

**Symptom**
The threshold, 10 checks in a row without a tick, was Linux's `watchdog_thresh` taken on trust: D3 recommended it with "step 4 measures the margin". Nothing in the kernel could say how close a healthy CPU came to it. A note's count goes back to 0 at the next tick, so by the time anyone could look, the evidence was gone; and the notes are private to the CPU that keeps them, read only by that CPU's own tick, and printed only in a report, which a healthy boot never makes.

**Cause**
A threshold is a bet on two distributions that must not overlap: how long a healthy CPU can go without a tick, and how long a stuck one does. The stuck side is easy, since a CPU spinning with interrupts off takes no tick at all, for good. The healthy side is what needs data, and four things shape what the data has to be:

- **What can make a healthy CPU miss its ticks.** A tick is the local APIC timer's interrupt. A CPU with interrupts off cannot take it; the APIC holds one pending interrupt per vector, so a CPU that keeps interrupts off for three periods takes one interrupt when it turns them back on and loses two ticks. Under QEMU there is a second way: a vCPU that the host does not run, while the virtual clock goes on, loses its periods in the same way. Every spin lock, and the console's lock, is held with interrupts off, so a healthy CPU loses ticks whenever it waits on one for longer than a period.
- **Why a whole-machine stall cannot fire, and a one-CPU stall can.** A check comes every 100 of the checking CPU's own ticks (D3). If the host stops the whole VM, the checker's ticks stop too, so it counts no checks and nothing ages. Only a stall of the target while the checker goes on counts against the target: one vCPU starved while the others run, or one CPU with interrupts off while the others tick. That is the case the measurement has to look at, and why it is taken alone and with a second QEMU on the host: the second QEMU competes for the host's cores, which is how a vCPU gets starved, and it is also what the hang's own record suggests may widen the race (`docs/repairs/261007/kernel-smp.md`, option (b)).
- **The count alone is too coarse to show a margin.** `unchanged()` counts whole checks in which the target took no tick at all. A CPU that took one tick in a second scores 0, the same as one that took a hundred. On a healthy boot every CPU scores 0, which says the threshold was never in danger, but not by how much. So the note also keeps the fewest ticks the target took between two checks: about 100 when it ticks as steadily as its checker, fewer when it loses periods, 0 when a whole check went by without one, and then the longest run of such checks is the count to compare with the threshold. The first check that sees a target ticking sees every tick since the target's tick started, which is not one check's worth, so the fewest counts from the second.
- **Who may read a note.** A note is plain data with one writer and one reader, both its own CPU, in its tick with interrupts off (D2): no lock and no atomic, and the file comment's argument rests on that. `lockup status` runs on whatever CPU the shell is on, and reading another CPU's notes from there, while that CPU's tick writes them, is a data race: undefined behaviour in C++, and in practice a torn pair of fields. There were three ways out. (a) Make the note's fields atomics: the pure, host-tested class would carry atomics only for the sake of a status line, and its comment that "a note needs no lock and no atomic" would no longer be true. (b) Keep a second, atomic copy of the marks for readers: two places would hold the same fact, and they could disagree. (c) Ask each CPU for a copy of its own notes, made on that CPU. (c) was taken: `IpiService::sync_exec()` runs `copy_notes()` on the target in its IPI handler, with interrupts off, as the tick runs, so on the CPU that owns the notes the copy and the tick never overlap; on the calling CPU, `sync_exec()` runs the copy itself with interrupts off for the same reason. The target clears its bit in the request with a release after the copy, and the caller waits on it with an acquire, so the caller sees the whole copy. The notes keep one writer and one reader at a time, both on their own CPU, which is the invariant D2 rests on.
- **What (c) costs.** `lockup status` waits for each CPU to take its IPI. A CPU with interrupts off holds it up: during `lockup spin 2 8000`, until the spin ends. A CPU that is stuck never answers, but the waiting CPU waits with interrupts on, so it keeps ticking and checking, and the detector reports the stuck CPU after its 10 s; the status cannot hang the machine silently. That needs the detector on: with `kernel.lockup.threshold=0` nothing would end such a wait, and no check has run, so no note has been written and there is nothing to copy. So with the checker off the status says so and asks no CPU (added in review). It copies one CPU's notes at a time, about half a kilobyte on the shell thread's stack at 16 CPUs, rather than all of them at once.

**Change**
The note, before:

```cpp
    constexpr auto check(u64 const heartbeat, u32 const threshold) -> bool {
        if (heartbeat != m_heartbeat) {
            m_heartbeat = heartbeat;
            m_unchanged = 0;
            return false;
        }
        if (heartbeat == 0) {
            return false;
        }
        ++m_unchanged;
        return threshold != 0 && m_unchanged >= threshold;
    }
```

After:

```cpp
    constexpr auto check(u64 const heartbeat, u32 const threshold) -> bool {
        if (m_heartbeat != 0) {
            auto const ticks = heartbeat - m_heartbeat;
            if (m_fewest_ticks.is_none() || ticks < m_fewest_ticks.value()) {
                m_fewest_ticks = ticks;
            }
        }
        if (heartbeat != m_heartbeat) {
            m_heartbeat = heartbeat;
            m_unchanged = 0;
            return false;
        }
        if (heartbeat == 0) {
            return false;
        }
        ++m_unchanged;
        if (m_unchanged > m_most_unchanged) {
            m_most_unchanged = m_unchanged;
        }
        return threshold != 0 && m_unchanged >= threshold;
    }
```

The status reads every CPU's notes on that CPU:

```cpp
    auto copy_notes(void *const context) -> void {
        *static_cast<priv::Notes *>(context) = ours::mm::CpuLocal::access(NOTES);
    }
    ...
    ours::Cpu::online_mask().for_each([&closest](CpuNum const checker) {
        auto notes = priv::Notes{};
        ours::IpiService::sync_exec(ours::CpuMask::of(checker), &copy_notes, &notes);
        log_notes(checker, notes, closest);
    });
    closest.print(threshold);
```

What it prints, from the dbg boot below: each note as now/at most/fewest.

```text
lockup status: a CPU is reported at 10 checks in a row without a tick; a check comes every 100 ticks of the checking CPU's own
lockup status: per CPU, the ticks it has taken, then its notes on each other CPU: checks in a row without a tick now/at most/fewest ticks between two checks
lockup status: cpu0 has taken 1307 ticks; its notes: cpu1 0/0/100 cpu2 0/0/100 cpu3 0/0/100
lockup status: cpu1 has taken 1526 ticks; its notes: cpu0 0/0/72 cpu2 0/0/99 cpu3 0/0/99
lockup status: cpu2 has taken 1525 ticks; its notes: cpu0 0/0/72 cpu1 0/0/99 cpu3 0/0/99
lockup status: cpu3 has taken 1525 ticks; its notes: cpu0 0/0/73 cpu1 0/0/100 cpu2 0/0/99
lockup status: closest to the threshold: at most 0 checks in a row without a tick (cpu1 as cpu0 saw it), threshold 10; at fewest 72 ticks between two checks (cpu0 as cpu1 saw it)
```

The default stays 10: the data below gives no reason to move it.

**Reference**
- Zircon `lockup_status()` (`zircon/kernel/lib/lockup_detector/lockup_detector.cc:607`), behind `k lockup status` (`cmd_lockup()`, `:715`): it prints each CPU's last heartbeat, its age and its `max_gap`. `max_gap` (`zircon/kernel/lib/lockup_detector/include/lib/lockup_detector/state.h:57`) is a per-CPU atomic that `HeartbeatLockupChecker::PerformCheck()` raises whenever it sees a larger gap (`lockup_detector.cc:248-251`). Zircon can keep it as one shared atomic because one checker at a time holds the per-target claim (`current_checker_id`); ours has every CPU check every other on private notes (D2), so the high-water marks live in each checker's notes and the status asks each checker for its own. Zircon's gap is a time; ours is counted in checks and ticks, as the threshold is (D3).
- Linux has no counterpart for the hard-lockup margin: `is_hardlockup()` (`linux/kernel/watchdog.c:183`) compares `hrtimer_interrupts` with `hrtimer_interrupts_saved` and keeps no history; only the threshold itself is visible, as `/proc/sys/kernel/watchdog_thresh`.

**Verification**
- Build: `prebuilt/ninja/ninja -C out/bringup.x86` clean; clang-tidy reports nothing in the changed files; the note's host test, 9 tests, passes.
- `scripts/kshell_run.py --out out/bringup.x86 "lockup status"` on dbg: `lockup status: ok`, with the output above.
- The checker-off path (added in review), on dbg built with a temporary `kernel.lockup.threshold=0` in its cmdline, not committed: `lockup status: ok`, its two lines `lockup status: the checker is off (kernel.lockup.threshold=0, or InitLevel::Platform not reached)` and `lockup status: closest to the threshold: no notes yet`; that no IPI goes out is read from the code, whose early return comes before any `sync_exec()`, not measured.
- `boot_check.py`: dbg PASS (shell finished at 12.9 s, 35 passed), rel PASS (4.1 s, 33 passed, 6/6 required, `pc-ps2: key a` and `pc-ps2: key A`), host-tests PASS (143), no `lockup:` line in either log and no `nmi` sent.
- The measurement, 6 boots. dbg alone is the `kshell_run.py` run above. The others ran a build with temporary code, not committed, that logs `lockup status` every 15 s from a kernel thread (rel has no kernel shell), through a scratch driver that boots the images at once with `scripts/qemu_boot.py` and stops each at the first status after the shell's end marker. The host is a 15-core Mac; no other QEMU ran, and its load average was 2.6 before and 5.0 after the pairs. Each status covers the whole boot from the checker's arming (about 3.5 s) to the guest time given.

| Boot | Image | On the host at the same time | End marker (guest s) | Status at (guest s) | Most checks in a row without a tick | Fewest ticks between two checks | Ticks taken, cpu0 / others |
|---|---|---|---|---|---|---|---|
| 1 | dbg | nothing | 17.5 | 18.6 | 0 | 72 (cpu0, by cpu1) | 1307 / 1525-1526 |
| 2 | rel | nothing | 3.6 | 18.3 | 0 | 99 | 1529 / 1502-1503 |
| 3 | dbg | boot 4 | 34.0 | 48.8 | 0 | 56 (cpu0, by cpu1) | 3930 / 4509-4510 |
| 4 | rel | boot 3 | 4.2 | 18.6 | 0 | 99 | 1529 / 1502-1504 |
| 5 | dbg | boot 6 | 13.5 | 18.9 | 0 | 75 (cpu0, by cpu1) | 1386 / 1506-1507 |
| 6 | dbg | boot 5 | 13.4 | 18.7 | 0 | 72 (cpu0, by cpu1) | 1344 / 1506-1507 |

- What the numbers say. No CPU went even one check without a tick, in any boot, alone or loaded: the threshold needs 10 in a row. The leanest second any CPU had still held 56 of its 100 ticks. Every lean second is the boot CPU's on the dbg image; on rel no CPU took fewer than 99, and on dbg cpu1 to cpu3 never fewer than 99. So the margin is not thin anywhere, and nothing in the data argues for moving the default: lower would buy a report a few seconds sooner at the cost of margin nobody has measured against a real host stall, and the scripts' 15 s silence `nmi` is set to come after the detector's 10 s.
- Boot 3 took 34 s to its end marker, against 13 s to 18 s for the other dbg boots, and had the leanest second. Its driver read QEMU's output a line at a time, and the dbg image writes 20,000 to 30,000 lines a second through a UART that it polls with the console lock held and interrupts off (`SerialLogger::write()`, `kernel/lib/logz4/kernel.cpp`). A reader that falls behind may hold that UART up, so the second pair's driver read raw chunks, as `kshell_run.py` does. Whether the reader or the load made boot 3 slow is not established; it is kept as data.
- Not measured: a host under heavier load than one other QEMU (a build at the same time), more than one boot per case, real hardware.

**Deliberately not done**
- The threshold was not changed (above).
- The boot CPU's lost ticks on dbg were not chased: 10% to 16% of its ticks over a boot, and up to 28% of one second (44% in boot 3), while cpu2 and cpu3, which print every `I'm Thread` line, lose none. Recorded as `docs/repairs/261008/kernel-lib-logz4.md`, with the measurement to take first; performance work goes through the user.
- `lockup status` does not print a note's last-seen heartbeat: the CPU's own heartbeat is on its line, and the last-seen count is a moment earlier than that.
- No time unit in the status: the threshold is counted in checks and ticks (D3), and a time would read as a promise the counts do not make.
- The open question on round-robin TCG against MTTCG stays open; the measurement did not need its answer.

**How it was found**
None.

**The user's prediction**
Skipped: on 2026-10-07 the user took every decision as recommended ("直接上") and skipped the predictions. The DESIGN's own expectation for a healthy boot was 0 or 1 checks in a row without a tick, and "far below 10" under load; every boot measured 0.

**Three questions you should be able to answer**
1. Why does `lockup status` copy each CPU's notes in an IPI on that CPU, rather than reading them from the shell's CPU, and what makes the copy visible to the caller once `sync_exec()` returns?
2. A pause of the whole VM counts no checks, but one vCPU starved by the host can. Why the difference, and which of the two did the loaded boots test?
3. Every boot scored 0 checks in a row without a tick. What does the fewest-ticks figure add that the count cannot, and why is it 0 exactly when the count has been at least 1?
