# scripts-boot outstanding repairs

## 1. Two worktrees that boot at once share one ESP image and one log

**Severity**
Medium: a run's verdict can come from another worktree's kernel, or its log can hold another run's output, whenever two sessions verify at once, which the shared worktree setup makes routine.

**Symptom**
On 2026-10-26 the log of a `kshell_run.py` run in `.worktrees/wip-proc-self`, `$TMPDIR/ours-kshell-bringup.x86/dbg.log`, held, after the commands it typed, a stretch of another session's (`vmosnapshot.test`, `pagesource.test`, `pager.test`) with timestamps going backwards. An earlier run in the same session timed out with no verdict, possibly for the same reason.

**Cause**
`scripts/kshell_run.py` names its directory after the build directory's last component only, `Path(tempfile.gettempdir()) / f"ours-kshell-{out.name}"`, and `boot_check.py` does the same with `ours-verify-{out.name}`. Every worktree builds into `out/bringup.x86`, so every worktree gets the same directory. It holds the log, the monitor log, and `WORK=esp-<image>`, where `scripts/qemu_boot.py` builds the ESP image QEMU boots from (`build_esp()`) and copies the firmware variables. A second run rebuilds the ESP image and truncates the log under a QEMU that is still using them.

**Change**
Name the directory after the whole build path, for example a short hash of `out.resolve()` beside `out.name`, in both scripts, so each worktree and build has its own. A lock file that makes the second run wait was ruled out: sessions would serialize on each other's boots for no reason.

**Reference**
None, this project's own tooling.

**Acceptance**
Start `boot_check.py` in two worktrees at once: each log holds only its own boot, and both verdicts match runs made one at a time.
