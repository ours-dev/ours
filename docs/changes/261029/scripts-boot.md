# scripts-boot changes

## Title: Every boot script names its temp directory for the build directory's whole path, so two worktrees' out/bringup.x86 no longer share one (2026-10-29)

**Files**
- `scripts/scratch_dir.py` — new: `path(prefix, out)`, the one naming rule, `$TMPDIR/<prefix>-<out's name>-<first 10 hex digits of the SHA-1 of out's resolved path>`
- `.agents/skills/ours-verify/scripts/boot_check.py` — the x86 mode's log directory is `scratch_dir.path("ours-verify", out)` instead of `ours-verify-<out's name>`
- `.agents/skills/ours-verify/scripts/riscv_boot.py` — `log_dir()` calls the helper; the directory it names is the same as before
- `scripts/kshell_run.py` — `ours-kshell-...` through the helper
- `scripts/cov_kernel.py` — `ours-covkernel-...` through the helper, which also resolves the `out` this script leaves unresolved
- `scripts/qemu_boot.py` — the default `WORK` is `scratch_dir.path("ours-boot", OUT)` instead of the fixed `/tmp/ours-boot`, so `qemu_debug.py` and manual boots get it too
- `.agents/skills/ours-verify/SKILL.md` — the rule, and the x86, riscv and kshell log paths it gives; the gotcha about sharing an out dir says what else is shared and that two worktrees need no care
- `.agents/tasks/riscv-boot-chain/PLAN.md` — the Test plan no longer asks every lane for `--out out/x86-<lane>`
- `docs/repairs/261026/scripts-boot.md` — deleted: its only item is fixed here

**Symptom**
Runs in two worktrees that boot `out/bringup.x86` at once read and wrote one temp directory. On 2026-10-26, the dbg.log of a `kshell_run.py` run in `.worktrees/wip-proc-self` held, after the run's own commands, a stretch of another session's output with timestamps going backwards. On 2026-10-28 a `boot_check.py` run in `.worktrees/feat-riscv` found its `host-tests.log` replaced by another worktree's: 148 test headers instead of its 152, one of them a test that tree does not have. On 2026-10-29 the one directory `$TMPDIR/ours-verify-bringup.x86` held a `dbg.log` that began by booting `.worktrees/feat-pmm-layering/out/bringup.x86/kernel.dev.dbg_1.obi` and a `rel.log` that began by booting `.worktrees/scratch-conn-dump/out/bringup.x86/kernel.dev.obi`. So the `logs` line a run printed could point at another run's boot.

No verdict was seen to cross, but one could: a simulation ran the real `qemu_boot.build_esp()` in two processes on one `WORK`, the second starting 0-80 ms after the first, 150 pairs. In 15 the first run's QEMU would have opened an ESP image holding the second worktree's kernel, in 12 a torn image, and 72 runs crashed while assembling the image, which `boot_check.py` reports as `QEMU exited (rc 1) before the shell finished -- reset or triple fault?`: a tooling race blamed on the kernel.

**Cause**
The objects:
- The temp directory, `tempfile.gettempdir()`, which on macOS is `$TMPDIR`: one per user, so every session and every worktree on the machine shares it.
- The run's directory under it, named `ours-verify-<out's name>` (`ours-kshell-`, `ours-covkernel-` for the other scripts), and `/tmp/ours-boot` for `qemu_boot.py` without a `WORK`. Every worktree builds into `out/bringup.x86`, so the last component is the same in every worktree, and so was the directory.
- In that directory, two kinds of things: each boot's logs (`dbg.log`, `rel.log`, the monitor logs, `host-tests.log`), and each boot's `WORK`, `esp-dbg/` and `esp-rel/`, where `qemu_boot.py` assembles the ESP image (`esp.img`) and copies the firmware (`code.fd`, `vars.fd`) that QEMU boots from.

What one boot does to its `WORK`, in order: `build_esp()` removes the whole directory, recreates its subdirectories (without `exist_ok`), copies in the shim and the kernel image and checks the copy's digest against the source, then runs grub-mkimage, mformat and mcopy to build `esp.img`. `main()` copies the firmware in and execs QEMU, which opens `esp.img`, `code.fd` and `vars.fd` by name. Measured on this host, `build_esp()` takes 23-52 ms, and QEMU reaches its drives 10-50 ms after the exec.

Why a removal after QEMU has opened its files is harmless. A name in a directory is only a link to a file (an inode). `open()` follows the name once, and from then on the descriptor refers to the file itself, not to the name. Removing a directory tree unlinks names; a file whose last name is gone lives on until its last descriptor closes. So when run B removes `WORK` after run A's QEMU has opened its three files, A's QEMU keeps reading and writing them through its descriptors, and the new files B creates under the same names are different files. (This item as first filed on 2026-10-26 said the opposite; it was corrected on 2026-10-29.)

Why a removal before that is not. The danger is the window from A's own removal of `WORK` until A's QEMU has opened its drives, some 30-100 ms. If B, booting the same tag, starts inside it:
- A's own steps trip over B's: a copy into a directory B just removed (FileNotFoundError), a `mkdir` of one B already recreated (FileExistsError), mformat or mcopy on a file B replaced (CalledProcessError), or the digest check failing with "changed while it was being copied". A's boot ends as a FAIL that names the kernel.
- Or A's steps all succeed and A's QEMU then opens the `esp.img` B has just rebuilt: it boots B's kernel. A's log still begins with the `qemu-boot: booting` line naming A's image, because `identify()` printed it before the swap. QEMU takes a write lock on a raw image (a second `qemu-system-x86_64` on one file exits with `Failed to get "write" lock`), so B's QEMU, opening that same file, fails. One run silently judges the other worktree's kernel, and the other fails for nothing.

Why the logs crossed but the verdicts did not. `Boot.read()` opens `<tag>.log` with `"wb"`, which truncates it. A second run opening the same name truncates the same file, and both keep writing at their own offsets, so the file mixes the two runs. `host-tests.log` is rewritten whole when a run's tests finish, so the last run to finish wins. But no verdict is read back from a file. Each run judges its boot from the stdout pipe of the QEMU it started itself, a pipe no other process holds, and its host tests from results it keeps in memory. The files are only a copy of what the pipe carried, so the copy could be wrong while the judgement was right.

The riscv mode already had the right name: `riscv_boot.log_dir()` adds a digest of the resolved build path. But `boot_check.py` did not use it for x86, and `kshell_run.py` and `cov_kernel.py` live in `scripts/`, which cannot import from `.agents/skills/` without a path hack.

**Change**
The riscv rule moves to one helper in `scripts/`, which `boot_check.py` already puts on `sys.path` for `host_tests`, and every site calls it. Before:

```python
# boot_check.py, kshell_run.py, cov_kernel.py
logdir = Path(tempfile.gettempdir()) / f"ours-verify-{out.name}"
logdir = Path(tempfile.gettempdir()) / f"ours-kshell-{out.name}"
logdir = Path(tempfile.gettempdir()) / f"ours-covkernel-{out.name}"
# qemu_boot.py
work = Path(os.environ.get("WORK") or "/tmp/ours-boot")
```

After:

```python
# scripts/scratch_dir.py
def path(prefix: str, out: Path) -> Path:
    out = Path(out).resolve()
    digest = hashlib.sha1(str(out).encode()).hexdigest()[:10]
    return Path(tempfile.gettempdir()) / f"{prefix}-{out.name}-{digest}"

# the callers
logdir = scratch_dir.path("ours-verify", out)     # boot_check.py; riscv_boot.log_dir() too
logdir = scratch_dir.path("ours-kshell", out)     # kshell_run.py
logdir = scratch_dir.path("ours-covkernel", out)  # cov_kernel.py
work = Path(os.environ.get("WORK") or scratch_dir.path("ours-boot", out))  # qemu_boot.py
```

- The helper resolves the path itself. `cov_kernel.py` joins the repo root and `--out` without resolving, and `qemu_boot.py` takes `OUT` from the environment, relative or absolute; one build directory spelled two ways must get one directory.
- The name keeps `out.name` for a person reading `$TMPDIR`, and adds the digest for uniqueness: the whole path as a directory name would be long and full of separators. Ten hex digits are 40 bits, far more than a few worktrees need.
- The riscv directory's name does not change: `boot_check.py` already hands `riscv_boot` a resolved path, and resolving it again gives the same string.
- The name is the same on every run, so SKILL.md and a person can still say where the last run's log is, and a run writes over its own previous logs instead of adding a directory.

Rejected:
- Calling `riscv_boot.log_dir()` from `boot_check.py` alone: a one-line fix for the verify skill that leaves `kshell_run.py` and `cov_kernel.py` colliding.
- A lock file that makes the second run wait: sessions would serialize on each other's boots for no reason.
- `tempfile.mkdtemp()` per run: the log path would no longer be predictable, and a directory per run would pile up in a `$TMPDIR` that already holds over 3000 entries.
- Logs and ESP inside the build directory: each x86 boot assembles a 64 MiB sparse `esp.img` that does not belong among GN's outputs, and the riscv mode and SKILL.md had already chosen `$TMPDIR` plus a digest.

**Reference**
None, this project's own tooling. For comparison only, neither is a counterpart:
- Zircon's `scripts/run-zircon` boots the build's images in place (`-kernel`, `-initrd`) and opens the UEFI variable store with `snapshot=true`, so it never makes a per-run scratch directory that could collide.
- Linux's kunit tool writes its log into the build directory: `get_outfile_path()` in `tools/testing/kunit/kunit_kernel.py`, opened by `run_kernel()`, with the default build directory `.kunit` from `get_default_build_dir()` in `tools/testing/kunit/kunit.py`.

**Verification**
In `.worktrees/feat-riscv`, `$TMPDIR` is `/var/folders/n_/20ryw2px3052ggtjf0xpsc8h0000gn/T`, shortened to `$TMPDIR` below, and the worktree's path to `<wt>`.

- `python3 -m py_compile` on the six changed scripts and `qemu_debug.py`: clean. `--help` of `boot_check.py`, `kshell_run.py`, `cov_kernel.py` and `qemu_debug.py` prints its usage. `qemu_boot.main()` run up to `build_esp()` with `build_esp` replaced by a print: the default `OUT`, `OUT=out/bringup.x86` and `OUT=<wt>/out/bringup.x86` all give `WORK $TMPDIR/ours-boot-bringup.x86-0e294bd573`, and a given `WORK` is kept.
- The riscv directory before and after: the old formula and `riscv_boot.log_dir()` both give `$TMPDIR/ours-verify-bringup.riscv-b64e068d07`.
- `ours-verify`, x86, default `--out`, exit 0. `0e294bd573` is the first 10 hex digits of the SHA-1 of `<wt>/out/bringup.x86`:

```text
build      ok (<wt>/out/bringup.x86, 0.3s)
dbg        PASS  shell finished at 6.0s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end
rel        PASS  shell finished at 4.6s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end
host-tests PASS  152 passed, 0 failed · 0.8s · log $TMPDIR/ours-verify-bringup.x86-0e294bd573/host-tests.log
logs       $TMPDIR/ours-verify-bringup.x86-0e294bd573/{dbg,rel}.log
```

- `ours-verify`, `--out out/bringup.riscv`, exit 0; `b64e068d07` is the SHA-1 prefix of `<wt>/out/bringup.riscv`:

```text
riscv      PASS  handoff ok at 0.1s · reached: handoff ok · initrd fake.obi
logs       $TMPDIR/ours-verify-bringup.riscv-b64e068d07/riscv.log
```

- Two runs at once whose build directories share a name but not a path, standing in for two worktrees: `out/concurrency/bringup.x86`, configured with `tools/set.py bringup.x86 --out out/concurrency/bringup.x86` (its `args.gn` identical to `out/bringup.x86`'s) and built. Its build has one failing edge, `//tests/dist-smoke:with_rename_dist`, which fails in every build directory deeper than `out/<name>` (`docs/repairs/261029/build-dist.md` item 1, found by the review); the images and host tests are not behind it, so the runs here use `--no-build`. Two `boot_check.py --no-build` runs, one per directory, started 1.3 ms apart: both exit 0, dbg PASS (44 passed, 4 skipped) and rel PASS (41 passed, 8 skipped, 6/6 required) in each, neither with `QEMU exited`. Their `logs` lines name `ours-verify-bringup.x86-0e294bd573` and `ours-verify-bringup.x86-3280fec54b`, the second the SHA-1 prefix of `<wt>/out/concurrency/bringup.x86`. Each `dbg.log` and `rel.log` holds one `qemu-boot: booting` line, naming its own directory's image (`<wt>/out/bringup.x86/kernel.dev.dbg_1.obi`, `<wt>/out/concurrency/bringup.x86/kernel.dev.dbg_1.obi`, and the same for `kernel.dev.obi`), and each `host-tests.log` has 152 `=== ` headers, its run's 152 passed and 0 failed.
- Two `kshell_run.py --out <dir> vmosnapshot.test` runs at once, 1.2 ms apart: both `vmosnapshot.test: ok` (241 checks), logged to `ours-kshell-bringup.x86-0e294bd573` and `ours-kshell-bringup.x86-3280fec54b`, each `dbg.log` booting its own directory's image.
- Two `cov_kernel.py` runs at once whose build directories share a name but not a path. `out/cov-kernel` was configured and built by `cov_kernel.py --no-tests` itself, which runs `tools/set.py bringup.x86 --out <out> --variant coverage-kernel` for a directory with no `build.ninja` and then builds it (47.9 s, then a boot, exit 0, log `$TMPDIR/ours-covkernel-cov-kernel-f96e606ca1/dbg.log`). `out/concurrency/cov-kernel` was configured the same way by hand and built with `ninja -k 0`, for the dist-smoke edge above. Two `cov_kernel.py --no-build --no-tests` runs, one per directory, started 1.5 ms apart: both exit 0 after 7.8 s, each with one boot, a 215080-byte profile and a report. Their `log` lines name `ours-covkernel-cov-kernel-f96e606ca1/dbg.log` and `ours-covkernel-cov-kernel-b3327914f8/dbg.log`, the SHA-1 prefixes of `<wt>/out/cov-kernel` and `<wt>/out/concurrency/cov-kernel`; each directory holds its own `esp-dbg/`, and each `dbg.log`'s one `qemu-boot: booting` line names its own directory's `kernel.dev.dbg_1.obi`. This bullet first said `cov_kernel.py` was not run because "it needs the coverage build", which was wrong: the script makes that build itself. The review found it, and the runs above replace it. The helper gives `ours-covkernel-cov-kernel-f96e606ca1` for its default `out/cov-kernel` and for `out/../out/cov-kernel` alike, and `scripts/test_cov_kernel.py`, which imports it and through it `kshell_run` and `scratch_dir`, passes its 17 tests.
- `qemu_debug.py` with `PORT=47123`, which starts `qemu_boot.py` without `WORK`: it reaches `gdbstub ready on localhost:47123`, and its QEMU holds `code.fd`, `esp.img` and `vars.fd` open in `$TMPDIR/ours-boot-bringup.x86-0e294bd573`; SIGTERM ends it and frees the port.
- `grep -rn 'gettempdir()) / f"ours-' scripts .agents/skills/ours-verify/scripts` finds nothing, and `qemu_boot.py` no longer names `/tmp/ours-boot`. A wider search of `scripts/` and `.agents/skills/` for `gettempdir`, `/tmp/ours-` and `out.name` in a directory name found only the helper and `.agents/skills/skill-creator/scripts/run_loop.py`, whose report file carries a timestamp and belongs to no boot.
- Not verified: two real worktrees at once (for `boot_check.py`, `kshell_run.py` and `cov_kernel.py` alike, a second build directory of the same name stood in for one), and the old race was not re-run against the new names; the simulation's numbers above are from when the item was filed.

**Deliberately not done**
- Two sessions that share one worktree and one build directory still share one temp directory, and one build. SKILL.md already forbids that ("use `--out out/<your-lane>`"), and it is a separate problem.
- The old directories (`$TMPDIR/ours-verify-bringup.x86`, `$TMPDIR/ours-kshell-bringup.x86`, `/tmp/ours-boot`) are not removed: sessions on worktrees that predate this change still boot from them, and removing them could take files from under a boot in progress.
- `docs/changes/261028/ours-verify.md` still says the x86 mode keeps its old name; change records are append-only, and this entry is the correction.

**How it was found**
From the logs themselves: a `dbg.log` whose timestamps went backwards partway through, a `host-tests.log` with the wrong number of headers and a test its tree does not have, and, decisively, the first `qemu-boot: booting` line of `dbg.log` and of `rel.log` in one directory naming two different worktrees. The ESP half came from reading `build_esp()` step by step and then running it for real: two processes on one `WORK`, 150 pairs at offsets from 0 to 80 ms, counting which runs crashed and which QEMU would have opened whose image; and from timing `build_esp()` and QEMU's first open of its drives. A second `qemu-system-x86_64` started on the same raw image showed the write lock.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does removing `WORK` after the other run's QEMU has opened its drives do no harm, while removing it a few tens of milliseconds earlier can boot the wrong kernel?
- For days the logs crossed and no verdict did: where does a verdict come from, and why was that source each run's own when the log file was not?
- Why is the directory named for a digest of the build directory's path, rather than made fresh by `mkdtemp()` for each run or put inside the build directory?
