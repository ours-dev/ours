# scripts-boot changes

## Each build directory gets its own log and ESP directory, so two worktrees can verify at once (2026-10-30)

**Files**
- `scripts/boot_logdir.py` — new: `log_directory(prefix, out)`, the one place the name is made.
- `scripts/kshell_run.py` — takes its directory from `log_directory("ours-kshell", out)`.
- `scripts/cov_kernel.py` — takes its directory from `log_directory("ours-covkernel", out.resolve())`, and no longer imports `tempfile`.
- `.agents/skills/ours-verify/scripts/boot_check.py` — takes its directory from `log_directory("ours-verify", out)`.
- `.agents/skills/ours-verify/SKILL.md` — says where kshell logs go now.
- `docs/repairs/261026/scripts-boot.md` — deleted; its one item is this one.

**Symptom**
On 2026-10-26 the log of a `kshell_run.py` run in one worktree held, after the commands it had typed, a stretch of another session's commands, with timestamps running backwards. An earlier run in the same session timed out with no verdict, possibly for the same reason. On 2026-10-29 the same thing cost a verification of the AP trampoline fix a round: a `NUMA=1` run passed, but its `dbg.log` held no `Bootstrap GDT` line and no CPU coming online, which is what another session's boot starting over the same file would leave. The runs were repeated with a private `TMPDIR`, where the logs were complete.

**Cause**
The objects involved:

- A build directory. Every worktree builds into `out/bringup.x86`, so the last component of its path is the same in all of them.
- A log directory under `$TMPDIR`, which a script made from that last component alone: `ours-verify-bringup.x86`, `ours-kshell-bringup.x86`, `ours-covkernel-cov-kernel`. Every worktree on the machine got the same one.
- What lives in it: the serial log and the monitor log, which the driver script writes (QEMU's serial port is its stdout, which the script reads through a pipe), and `esp-<image>/`, the directory with the FAT image QEMU boots from, which `scripts/qemu_boot.py` builds there (`WORK`).

What happens when two worktrees run the same script at once:

1. Run 1's script starts `qemu_boot.py`, which builds `esp-dbg/` and then execs QEMU; QEMU opens the image and the firmware files at startup and keeps them open. The script opens `dbg.log` with `open(..., "wb")` and writes into it what it reads from QEMU's stdout.
2. Run 2 starts. Its `open(..., "wb")` truncates the same file. Run 1's script goes on writing at the offset it had reached, since nothing is opened for append. The file now has a gap of NUL bytes where run 1's earlier output was, run 1's new lines overwrite run 2's, and what is left reads as two boots interleaved, with timestamps going backwards.
3. Run 2's `qemu_boot.py` also deletes and rebuilds `esp-dbg/`. A QEMU already running keeps its open files and is not disturbed. One still between its own `build_esp()` and starting QEMU can find the directory gone or half built, or boot run 2's files.

The verdict is judged from the live pipe, not read back from the file, so a passing boot stays passing. What the collision spoils is the log as evidence, which is how it was found: nothing said the run had failed. The 2026-10-26 run that timed out with no verdict may have been the window in step 3; that was not shown.

The skill's own rule is that two sessions must not build or boot from the same out directory. That was never the situation here: the sessions had different out directories, in different worktrees, and shared a log directory only by accident of how it was named. The rule was being kept and the collision happened anyway.

**Change**
Before, in `boot_check.py` (`kshell_run.py` had the same line with `ours-kshell-`, and `cov_kernel.py` with `ours-covkernel-`):

```python
logdir = Path(tempfile.gettempdir()) / f"ours-verify-{out.name}"
```

After, with `log_directory()` in `scripts/boot_logdir.py` and the three scripts calling it:

```python
tag = hashlib.sha256(str(out).encode()).hexdigest()[:10]
return Path(tempfile.gettempdir()) / f"{prefix}-{out.name}-{tag}"
```

The name is made from the whole resolved path of the build directory, with the last component kept in front so that a listing of `$TMPDIR` still says which build a directory is for. Ten hex digits are 40 bits, far more than the handful of build directories one machine has. `out` is resolved before the call in both scripts, so two spellings of one path give one directory.

The function lives in `scripts/` beside `host_tests.py`, which `boot_check.py` already imports from there, rather than being written out in each script: the scripts cannot come to name the directory differently, and the next script that wants a log directory has one to call. `cov_kernel.py`, the other caller of `kshell_run.Console`, was missed by the first version of this change and found in review; its `out` is resolved at the call, not where it is first made, so the rest of that script's paths are unchanged.

Rejected: a lock file that makes the second run wait. Sessions would serialize on each other's boots for no reason, when the only shared thing is a name.

**Reference**
None, this project's own tooling.

**Verification**
- Both scripts compile; `log_directory()` gives two different names for `/x/wt1/out/bringup.x86` and `/x/wt2/out/bringup.x86`, and the same name twice for one path.
- The acceptance in the repair item, with two build directories both named `bringup.x86` under different parents (the second a copy of the images from the hotplug-off build, so that their digests differ), using the default `$TMPDIR`:
  - `boot_check.py` started in both at once: both pass both images (dbg 44 passed, rel 41 passed and 6/6 required), in `ours-verify-bringup.x86-2fa9e6d4e6` and `ours-verify-bringup.x86-b53dc0c047`. Each log holds its own kernel's digest and not the other's.
  - `kshell_run.py pmzone.test` started in both at once: both print `pmzone.test: ok`, in `ours-kshell-bringup.x86-2fa9e6d4e6` and `ours-kshell-bringup.x86-b53dc0c047`, and each log holds its own digest only. The suffixes match the verify run's for the same paths, as they should.
- `cov_kernel.py` compiles and its directory is made by the same function, but it was not run end to end: that needs a coverage build, and the change to it is the one line.
- Not measured: a run against the old naming, to show the interleaving. It is shown by the 2026-10-26 and 2026-10-29 logs above and by the code, not reproduced here.

**Deliberately not done**
- `scripts/qemu_boot.py` still defaults `WORK` to `/tmp/ours-boot` when it is run by hand, which every worktree shares. The two scripts above always pass their own, so only a hand run or `qemu_debug.py` is exposed. A separate decision, since a hand run has no build directory to name it after.
- The old `ours-verify-bringup.x86` and `ours-kshell-bringup.x86` directories already in `$TMPDIR` are left; nothing reads them now. Each distinct build directory now leaves one directory of its own, where before they all shared one.
- No check in the scripts for two runs against one build directory, which the skill already forbids.

**How it was found**
From the logs. The 2026-10-26 one had timestamps running backwards, which no single boot can produce; the 2026-10-29 one was a passing run whose log lacked the boot lines that run must have printed (the author's own check for them came back empty). In both, the build directories were different and the log directory was the same, which put the fault in the naming and not in the boots.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Two sessions used different out directories in different worktrees and still corrupted each other's logs. What did they share, and which line of which operation turned the sharing into corruption?
2. Why keep `out.name` in the directory name when the hash alone would already be unique?
3. Why does `log_directory()` expect an already resolved path, and what would two spellings of one path do if it did not?
