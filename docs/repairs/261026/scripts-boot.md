# scripts-boot outstanding repairs

## 1. Runs in two worktrees that share a build-directory name share one temp directory, with its ESP images and its logs

**Severity**
Medium: crossed logs happen whenever two worktrees verify at once, which the shared worktree setup makes routine; a wrong verdict needs two boots with the same tag to start within about 0.1 s of each other.

**Symptom**
First filed on 2026-10-26 and rewritten on 2026-10-29 with the evidence below and a corrected cause.

On 2026-10-26, the dbg.log of a `kshell_run.py` run in `.worktrees/wip-proc-self` (`$TMPDIR/ours-kshell-bringup.x86/dbg.log`) contained, after the run's own commands, a stretch of another session's output with timestamps going backwards. On 2026-10-28 a review run of `boot_check.py` in `.worktrees/feat-riscv` found its `host-tests.log` replaced by another worktree's: 148 test headers instead of its 152, and one of them names a test that tree does not have. On 2026-10-29 the one directory `$TMPDIR/ours-verify-bringup.x86` holds runs from two different worktrees. Its `dbg.log` (written 00:00) begins `qemu-boot: booting .../.worktrees/feat-pmm-layering/out/bringup.x86/kernel.dev.dbg_1.obi`. Its `rel.log` (written 00:12) begins `qemu-boot: booting .../.worktrees/scratch-conn-dump/out/bringup.x86/kernel.dev.obi`. So the `logs` line that `boot_check.py` prints can point at another run's output, and anyone debugging a FAIL from it reads someone else's boot.

The verdict itself has not been seen to cross, but it can. A simulation ran the real `qemu_boot.build_esp()` in two processes on one WORK directory, the second starting 0-80 ms after the first, for 150 pairs. In 15 of them, the first run's QEMU would have opened an ESP image that held the second worktree's kernel. In 12, it would have opened a torn image. 72 runs crashed while assembling the image. A crash shows up in `boot_check.py` as `QEMU exited (rc 1) before the shell finished -- reset or triple fault?`, which blames the kernel for a tooling race.

**Cause**
Each script names its temp directory after the last component of the build directory only:
- `.agents/skills/ours-verify/scripts/boot_check.py:588`: `ours-verify-{out.name}`
- `scripts/kshell_run.py:634`: `ours-kshell-{out.name}`
- `scripts/cov_kernel.py:326`: `ours-covkernel-{out.name}`

All of them sit under `tempfile.gettempdir()`, which is the one per-user `$TMPDIR` that every session and worktree share. Every worktree builds into `out/bringup.x86`, so every worktree gets the same directory. `scripts/qemu_boot.py:183` goes further: its default `WORK` is the fixed `/tmp/ours-boot`, and `scripts/qemu_debug.py:324-328` starts it without a `WORK`. The riscv mode already avoids this. `riscv_boot.log_dir()` (`.agents/skills/ours-verify/scripts/riscv_boot.py:91-98`) adds `hashlib.sha1(str(out).encode()).hexdigest()[:10]` of the resolved build path (`boot_check.py:577` resolves it). But `boot_check.py` does not use that helper for x86, and `kshell_run.py` and `cov_kernel.py` live in `scripts/`, where they cannot reach it.

The shared directory holds two kinds of things.

The logs. `Boot.read()` opens `<tag>.log` with `"wb"` (`boot_check.py:414`). A second run opening the same path truncates the same file, and the two readers keep writing at their own offsets, so the file mixes both runs. `host-tests.log` is rewritten whole when a run's tests finish (`boot_check.py:283`), so the last run to finish wins. The monitor logs are rewritten the same way (`boot_check.py:500`). None of this changes a verdict, because verdicts come from QEMU's stdout pipe (`boot_check.py:412-417`) and from in-memory test results (`boot_check.py:279-282`), not from the files.

The ESP. Each boot passes `WORK=<dir>/esp-<tag>` (`boot_check.py:321,348`; `kshell_run.py:175`). `qemu_boot.build_esp()` then:
1. removes `WORK` (`qemu_boot.py:113-114`);
2. recreates its directories without `exist_ok` (116-117);
3. copies in the shim and the image and checks the copy's digest (119-131);
4. runs grub-mkimage, mformat and mcopy into `WORK/esp.img` (136-156).

`main()` then copies the firmware into `WORK` (200-201) and execs QEMU, which opens `esp.img`, `code.fd` and `vars.fd` by path.

Once QEMU has opened them, a second run's removal only unlinks the files, and the running QEMU keeps reading and writing the unlinked files through the descriptors it already holds. (This item as first written on 2026-10-26 said otherwise.) The danger is the window from one run's removal of `WORK` until its QEMU opens its drives. Measured on this host, `build_esp()` takes 23-52 ms and QEMU reaches its drives in 10-50 ms. If a second run with the same tag enters that window, two things can happen:
- The first run hits FileNotFoundError, FileExistsError or CalledProcessError, or `die()` reports that the image "changed while it was being copied". The boot ends as a FAIL that blames the kernel.
- The first run's QEMU opens an `esp.img` that the second run has rebuilt, and boots the other worktree's kernel. Its log still begins with the `qemu-boot: booting` line naming its own image, because `identify()` printed that line before the swap. QEMU on this host takes a write lock on its image (a second `qemu-system-x86_64` on the same raw file exits with `Failed to get "write" lock`). So when this happens, one run silently judges the wrong kernel and the other fails spuriously.

The documented workaround, a build directory of one's own (`.agents/tasks/riscv-boot-chain/PLAN.md`, the Test plan's first item; `docs/changes/261028/ours-verify.md:32`), does not help anyone who runs the default `out/bringup.x86`. This item depends on no other item.

**Change**
Move `riscv_boot.log_dir()`'s naming into one helper under `scripts/`. `boot_check.py` already puts `scripts/` on `sys.path` (line 69) for `host_tests`. The helper takes a prefix and the build directory, resolves the directory itself, and returns `tempfile.gettempdir() / f"{prefix}-{out.name}-{sha1(str(out.resolve()))[:10]}"`. Then:
- Have `riscv_boot.log_dir()`, `boot_check.py:588`, `kshell_run.py:634` and `cov_kernel.py:326` call it. `cov_kernel.py:305` does not resolve its `out`, which is why the helper must.
- Give `qemu_boot.py`'s default `WORK` the same treatment, keyed on its `OUT`, so that manual and `qemu_debug.py` boots in two worktrees stop sharing `/tmp/ours-boot`.
- Update the paths that `.agents/skills/ours-verify/SKILL.md` gives (lines 84-85 for the riscv log, line 134 for kshell logs) to the one rule, and drop the `--out out/x86-<lane>` reason from the Test plan of `.agents/tasks/riscv-boot-chain/PLAN.md`.
- Leave `docs/changes/261028/ours-verify.md` alone, since change records are append-only, and record the fix in a new entry.

Calling `riscv_boot.log_dir()` from `boot_check.py` alone would be a one-line fix for the verify skill, but it was ruled out: it leaves `kshell_run.py` and `cov_kernel.py` colliding, and those cannot import from `.agents/skills/` without a path hack.

Other options ruled out:
- A lock file that makes the second run wait: sessions would serialize on each other's boots for no reason.
- `tempfile.mkdtemp()` per run: the log path would no longer be predictable, so neither SKILL.md nor a person could say where the last run's log is, and a directory per run would pile up in a `$TMPDIR` that already holds over 3000 entries.
- Logs and ESP inside the build directory, which keeps them per worktree the way Linux's kunit keeps `test.log` in its build directory: it was not chosen, because each x86 boot assembles a 64 MiB sparse `esp.img` that does not belong among GN's outputs, and the riscv mode and SKILL.md already chose `$TMPDIR` plus a digest.

Two sessions that share one worktree and one build directory still share one directory. That is the existing rule at SKILL.md:236-237, which already forbids it, and it is out of scope here.

**Reference**
None, this project's own tooling.

For comparison only, neither is a counterpart:
- Zircon's `scripts/run-zircon` boots the build's images in place (`-kernel`/`-initrd`, lines 303-308) and passes the UEFI variable store with `snapshot=true` (line 300), so it never makes a per-run scratch directory that could collide.
- Linux's kunit tool writes its log into the build directory: `get_outfile_path()` in `tools/testing/kunit/kunit_kernel.py` (lines 196-197), opened by `run_kernel()` at line 385, with the default build directory `.kunit` from `get_default_build_dir()` in `tools/testing/kunit/kunit.py` (lines 343-346).

**Acceptance**
These checks show it is fixed:
- `python3 .agents/skills/ours-verify/scripts/boot_check.py` in one worktree prints `logs  $TMPDIR/ours-verify-bringup.x86-<10 hex digits>/{dbg,rel}.log`. The digits equal `sha1` of that worktree's resolved `out/bringup.x86`, the same rule `riscv_boot.log_dir()` follows. The riscv run (`--out out/bringup.riscv`) still prints its `ours-verify-bringup.riscv-<hex>/riscv.log`.
- Start `boot_check.py` with the default `--out` in two worktrees at the same moment:
  - the two `logs` lines name different directories;
  - the first `qemu-boot: booting` line of each `dbg.log` and `rel.log` names that worktree's own `out/bringup.x86` image;
  - the number of `=== ` headers in each `host-tests.log` equals the passed plus failed count that its own run printed;
  - neither run fails with `QEMU exited ... before the shell finished`.
- `scripts/kshell_run.py --out out/bringup.x86 <test>` and `scripts/cov_kernel.py` started in two worktrees at once log to two different `ours-kshell-bringup.x86-<hex>` and `ours-covkernel-bringup.x86-<hex>` directories.
- `grep -rn 'gettempdir()) / f"ours-' scripts .agents/skills/ours-verify/scripts` finds no call left outside the helper, and `qemu_boot.py` no longer defaults `WORK` to `/tmp/ours-boot`.
- Both x86 images and the riscv chain pass `ours-verify`.
