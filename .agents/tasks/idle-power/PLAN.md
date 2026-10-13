# idle-power: the steps from a measured baseline to tickless and deep idle

**Date**: 2026-10-13
**Status**: Accepted

## Request
"做省电吧" (2026-10-13), then "b和ii": deep idle and tickless both, tickless after Zircon (DESIGN.md).

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Measurement before change | Counters and a command first; change first | Counters first | Not given | Counters first, 2026-10-13 |
| Concurrency homework | Write the argument now; later | Now | Deferred, as for cpu-hotplug: why the halt clock's sequence count lets another CPU read a halt in progress without a torn or double count (`idle_thread.cpp`, `HaltClock`) | Later, 2026-10-13 |
| The two "gibberish" threads `labour_routine()` starts on every image (`kernel/top/main.cpp`), which keep two CPUs from ever halting | Delete them; start them only under a boot option; keep them | A boot option, off by default | Not given | Delete them (ii), 2026-10-13 |

## Steps
Each step is introduced to the user, with its background, before it starts.

1. Measurement: per-CPU counters for local timer interrupts and idle entries, idle time accounted around the halt only, and a kshell command reporting them over a window; the baseline recorded. One commit.
2. The lockup heartbeat moves off the tick onto a per-CPU one-second timer, cancelled and re-armed as a CPU goes offline and comes back (lockup-watchdog D1 (b)). One commit.
3. Later steps are introduced one at a time.

Each step: clang-format and clang-tidy on its lines, `ours-verify`, its kshell tests, a mutation where a test guards a line, a change record, and a commit of only its paths.

## Files
- Step 1: `kernel/lib/gktl/include/gktl/counter.hpp`, `kernel/kernel/time/timer_queue.cpp`, `kernel/kernel/task/idle_thread.cpp`, their headers, `kernel/commands/idle.cpp`, `kernel/commands/BUILD.gn`.
- Step 2: `kernel/lib/lockup_detector/src/checker.cpp`, `src/report.cpp`, `include/lockup_detector/lockup_detector.hpp`, `README.md`, `kernel/kernel/time/tick_sched.cpp`, `kernel/kernel/time/BUILD.gn`, `kernel/kernel/task/idle_thread.cpp`, `kernel/kernel/task/BUILD.gn`, `scripts/kshell_run.py`.

## Test plan
The measurement command on a quiet dbg image before and after every step, its numbers in each change record.
