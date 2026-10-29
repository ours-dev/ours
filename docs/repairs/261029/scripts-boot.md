# scripts-boot outstanding repairs

## 1. qemu_debug.py in one worktree kills another worktree's live debug QEMU, because every worktree defaults to gdb port 1234 and the port is cleared by killing its holder

**Severity**
Medium: it ends another session's debugging under it, but only when two sessions run `qemu_debug.py` (or the VS Code debug task) on the default port at once.

**Symptom**
Latent, not seen yet. Session A in one worktree runs `scripts/qemu_debug.py` and is stopped at a breakpoint in lldb. Session B in another worktree starts `scripts/qemu_debug.py` without `PORT`. B prints `killing stale process(es) on port 1234: <pid>`, A's QEMU gets SIGTERM, and A's lldb loses its target mid-session. B's boot is fine, so nothing tells B what it did.

**Cause**
It is the same kind of hazard as the temp directories fixed on 2026-10-29 (`docs/changes/261029/scripts-boot.md`): one fixed per-machine name, here a TCP port, used by every worktree. `scripts/qemu_debug.py:249` defaults `port` to `1234`, and `clear_port()` (lines 203-222) sends SIGTERM to every pid `lsof -ti tcp:<port>` reports before QEMU starts. Its docstring assumes whatever holds the port is a stale QEMU or lldb from this user's own interrupted session, which was true with one worktree; with several worktrees on one machine the holder may be another session's live debugger. The VS Code task `kernel: stop qemu` in `ours.code-workspace` (line 174) kills whatever holds 1234 the same way, and the two launch configurations there attach to `localhost:1234` (lines 86 and 103). `.agents/skills/ours-verify/references/debugging.md` tells an agent to pick its own `PORT`, which helps only those who read it, as `--out out/x86-<lane>` did for the temp directories.

**Change**
Recommended: clear only a holder that is this build directory's own stale QEMU, and refuse otherwise. `qemu_debug.py` starts `qemu_boot.py` without `WORK`, so its QEMU's command line names `scratch_dir.path("ours-boot", out)`. `clear_port()` reads each holder's command line (`ps -o command= -p <pid>`) and kills it only if it is a `qemu-system-*` naming that directory; for any other holder it exits with the pid, its command and "pick another PORT". The default stays 1234, so the VS Code configurations keep working; the `kernel: stop qemu` task gets the same check.

Other options:
- Derive the default port from the same digest of the repo root, for example `1234 + int(digest, 16) % 1000`. Ruled out as the whole fix: the VS Code configurations hard-code 1234 and cannot compute it, and 13 worktrees among 1000 ports still collide about one time in thirteen, after which the killing resumes.
- Bind port 0 and let the system pick a free port, printed on the ready line. Ruled out for the same VS Code reason, and a stale QEMU from this session would then be left running instead of cleared.

**Reference**
None, this project's own tooling.

**Acceptance**
- With `qemu_debug.py` running and stopped in worktree A, `qemu_debug.py` with the default port in worktree B exits non-zero naming A's QEMU pid, and A's lldb session is still attached.
- A stale `qemu_debug.py` QEMU left behind in the same worktree and build directory is still cleared, and the new one reaches `gdbstub ready on localhost:1234`.
- The VS Code `kernel: stop qemu` task leaves a QEMU of another worktree alone.
