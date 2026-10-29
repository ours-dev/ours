# ours-verify outstanding repairs

## 1. boot_check.py keeps its logs and boot directories under a name that two worktrees share

**Severity**
Low: nothing is wrong when one check runs at a time, and the skill tells sessions not to boot from one out directory. It is a trap for the parallel steps of a task that each have a worktree of their own.

**Symptom**
Latent, cannot be triggered by a single run. Two worktrees that run `boot_check.py` at the same time with the default `--out out/bringup.x86` write `rel.log`, `dbg.log`, `host-tests.log`, the monitor logs and the two boot directories `esp-dbg` and `esp-rel` into the same directory, `$TMPDIR/ours-verify-bringup.x86`. Each run's logs overwrite the other's while it reads them, and each run's boot directory is the other's, so a PASS or a FAIL can be the verdict on the other worktree's image. On 2026-10-29 the check in `.worktrees/pi-root-experiment` printed the same `logs` directory as the check in `.worktrees/pi-root` had a minute before. They ran one after the other, so nothing was lost; the plan of `protocol-identity` runs its steps 2 and 3 in parallel worktrees, which is where it would be.

**Cause**
`main()` in `.agents/skills/ours-verify/scripts/boot_check.py` names the directory `ours-verify-{out.name}`: the last component of the build directory (`bringup.x86`), not the directory itself. Every worktree configured by `scripts/new_worktree.py` has an `out/bringup.x86`, so the names collide across worktrees. The skill's gotcha ("use `--out out/<your-lane>`") is about two sessions in one worktree; it does not cover two worktrees that both took the default.

**Change**
Name the directory after the whole resolved build directory, so that the same name in two worktrees is two directories: a short digest of `out`'s absolute path beside its name, `ours-verify-bringup.x86-<digest>`. `scripts/kshell_run.py` keeps its logs in `$TMPDIR/ours-kshell-<out>/` by the same rule and has the same trap; fix both together, and update the one path the skill's text gives (`.agents/skills/ours-verify/SKILL.md`, "Kernel tests": `$TMPDIR/ours-kshell-<out>/`).

Ruled out: asking each session to pass its own `--out`, which is what the skill's gotcha says today and which a worktree made by `scripts/new_worktree.py` does not do by default.

**Reference**
None, this project's own tooling.

**Acceptance**
Two `boot_check.py --no-build` runs started together from two worktrees print two different `logs` directories, and each verdict is read from its own worktree's image. `scripts/kshell_run.py` names its directory by the same rule, and the skill's text says so.
