# gktl-options: every command declares its words, and one grammar runs the tree

**Date**: 2026-11-14

## Delivered

The declared-options builder in gktl (`kernel/lib/gktl/include/gktl/options.hpp` with its reporting half `report.hpp`), the host suite `options-test` (22 cases, gktl at 96.3% lines), and every command that parses words migrated to it, in four commits:

- 27e384cc — the builder, the reporting half, the host suite, `parse_u64`
  promoted from ktest (which now delegates to it).
- e68e5a06 — the benches and the simple commands; `bench::Run` takes the `all`
  flag instead of argv; the five hand-rolled digit parsers deleted; the old
  word forms swept from the driver scripts.
- 6071637d — cpu, lockup and log: verbs became options; the argc-overloaded
  `log` grammar retired; kshell_run's lockup judge follows the new forms.
- d84981e3 — the ktest runner: `sel=` names the selection, `filter=` is the
  death run's glob, the runner options are declared options, `pass_through()`
  keeps the tests' words; `RunPlan` stays data, its parse machinery is gone;
  the design doc's grammar block carries the supersession note.

The change record is `docs/changes/261114/gktl-options.md`, three sections.
Two repair items were filed: the console's silent 16-word truncation
(`docs/repairs/261114/kshell-word-cap.md`) and the timer-lateness shuffle
flake (`docs/repairs/261114/timer-forward-lateness.md`).

## Verification

- `boot_check.py --out out/bringup.x86`: build ok, dbg PASS 46/5/0, rel PASS
  44/8/0 (7/7 required), host-tests 184/0 — options-test among them.
- cov_host `kernel/lib/gktl`: 96.3% lines (337/350), above the 95% gate.
- Every migrated command smoked on target in its new word form (the list in
  the change record's second section); the rejection path's pinned line and
  generated usage seen on the console.
- `ktest sel=all` 83/80/3 twice; `ktest sel=list` order identical to
  baseline's; the composition `sel=all shuffle repeat=2 shard=0/1` at
  baseline's exact numbers; `sel=death` boots fresh per death test and the
  death test panics as expected; `sel=Ktest.Matcher*` filters.
- `test_kshell_run.py` and `test_cov_kernel.py` green.

## Deviations from the design

- The ktest command's options and parse live in namespace-scope storage, not
  on the handler frame: the first shuffle run wedged in the hotplug test's
  offline deadline, deterministically, and the only run-path difference from
  baseline was the builder's few kilobytes of frame under a frame that hosts
  the whole run on a 16 KiB stack. The Options class comment now states the
  rule for long-hosting commands.
- `dump-frame` is not migrated — it parses nothing (the plan's own table
  said so).

## Found along the way

- `choices({"a","b"})` with a brace list dangles: the list's backing array
  dies with the statement, and the acceptance check read it dead. Caught by
  the first host run; `choices` now takes a span the command owns.
- `defaults_to("time")` bound to the bool overload through
  pointer-to-bool — a footgun the compile caught; the char const* overload
  now spells the string meaning explicitly.
- The stale `test_commands` case in `test_cov_kernel.py`, left behind by the
  migration round, testing a function that no longer exists — removed.
- The console's silent 16-word truncation, reachable only through the
  builder's own TooManyWords-not-firing observation — filed, not fixed
  (`docs/repairs/261114/kshell-word-cap.md`).
- The timer-lateness shuffle flake — filed
  (`docs/repairs/261114/timer-forward-lateness.md`).

## Not done

- The console repair itself (report the truncation instead of dropping) —
  claimable on its own.
- The timer flake's remedy (skip under emulator or an order-independent
  assertion) — measurement first, per the repair item.
- A range constraint on number options — two commands' handler-side range
  checks phrase their ranges better than a framework constraint would.
