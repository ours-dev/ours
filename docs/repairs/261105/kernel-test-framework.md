# kernel-test-framework outstanding repairs

## 1. The remaining kernel `*.test` console commands have not migrated to the kernel test interface

**Severity**
Low: the tests still run and pass, but each carries its own `Tally`/`verdict`/`ok` reporting boilerplate, none of them appears in `ktest list`, and `scripts/cov_kernel.py` still has to scrape their registrations to find them.

**Symptom**
`python3 scripts/kshell_run.py --out out/bringup.x86 'ktest list'` lists the ten migrated tests and none of the others; `kernel/commands/*_test.cpp` repeat the same reporting code; the two consumers keep a second path alive for them -- `kshell_run.py`'s `<name>: ok` rule and `cov_kernel.py`'s `GKTL_COMMAND` scrape.

**Cause**
The interface's first slice ends with a three-test pilot (design, 2026-11-05); the other 43 commands were deliberately left for an incremental migration so the pilot could prove the interface first. This is the list of that work, not a defect in one file.

**Change**
For each remaining `kernel/commands/*_test.cpp`: move it to `kernel/tests/<area>_test.cpp`, replace its `Tally`/`verdict`/final line with `KTEST` and the assertion macros, delete its `GKTL_COMMAND`, and add a `kernel_test()` target. Small and self-contained first: the command files run 108 (the three migrated ones) to 2,266 (`vmo_snapshot_test.cpp`) lines, so `vm_address_test.cpp` (135), `tick_fallback_test.cpp` (153), `vma_physical_test.cpp` and `vma_seal_test.cpp` (160) are the next natural batch. Three commands take arguments (`interrupt.test [ROUNDS]`, `port.test [ROUNDS]`, `timershare.test big`) and the interface has no per-run arguments yet: decide first whether such a test hardcodes its configuration, reads a boot option, or the `ktest` command grows an argument mechanism. `user_copy_test.cpp` and `mutex_stress.cpp` got comments on 2026-11-05 saying they have not migrated; keep those true.

**Reference**
The three tests under `kernel/tests/` that migrated on 2026-11-05 are the pattern: `sampler_claim_test.cpp`, `semaphore_test.cpp` and `vma_wx_test.cpp`. The interface is `docs/design/261105/kernel-test-framework.md`.

**Acceptance**
`ktest list` names every test; `kernel/commands` holds only commands that are not tests; `kshell_run.py`'s `<name>: ok` rule and `cov_kernel.py`'s `GKTL_COMMAND` scrape can be deleted; `python3 .agents/skills/verify/scripts/boot_check.py` passes and `python3 scripts/kshell_run.py --out out/bringup.x86 'ktest all'` reports every test passing.
