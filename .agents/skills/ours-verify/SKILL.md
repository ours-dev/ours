---
name: ours-verify
description: Build the ours kernel and boot both dev images in QEMU to prove a change works, and debug a boot that fails (breakpoints and backtraces through the lldb MCP server). Use after any change to kernel, phys, userboot, init, shell or build files, before every commit, whenever the user says verify, build and boot, "验证一下", "跑一下", "启动看看", or asks whether something still boots, and when a boot panics, asserts or hangs ("panic 了", "卡住了", "看下调用栈"). A clean compile is not verification in this repo.
---

# Verify an ours change

Done means it builds, **both** images boot, and the shell finishes its
self-checks. "It compiles" is not done, and neither is booting one image.

## Run it

From the repo root:

```bash
python3 .agents/skills/ours-verify/scripts/boot_check.py
```

It builds `out/bringup.x86` with `prebuilt/ninja`, boots `kernel.dev.dbg_1.obi`
and `kernel.dev.obi` side by side, stops QEMU once each verdict is in (about
25 seconds end to end), and prints one line per image. Exit code 0 means both
passed. Paste those lines to the user as the evidence.

On rel it also types keys: once the PS/2 keyboard driver logs `pc-ps2: ready`,
it sends `sendkey a` and `sendkey shift-a` to QEMU's monitor, which shares
stdio with COM1 behind Ctrl-A c, and requires `pc-ps2: key a` and
`pc-ps2: key A`. That is the one check of a device's interrupt reaching a
user-space driver.

Flags: `--out out/<dir>` (configured automatically if new), `--no-build`,
`--only dbg|rel`, `--timeout N`. `SMP=1`, `NUMA=1` and the other
`scripts/qemu_boot.py` variables pass through.

For C++ edits, `cpp-write-and-verify` (clang-format and clang-tidy) comes
first; this skill checks behaviour, that one checks the source.

## Kernel tests

The kernel's `*.test` commands (`//kernel/commands`) do not run at boot.
After a change to what one covers, run it on the dbg image, after the build:

```bash
python3 scripts/kshell_run.py --out out/<dir> vmosnapshot.test
```

Only the dbg image has a kernel shell. Since the interactive user shell
(`ead5d12f`), the rel image's COM1 belongs to the user shell
(`kernel.shell=false`), which takes `vmosnapshot.test` for a program in
`bin/` and answers "no such program". dbg is also the image with the
assertions a test relies on.

It boots the image in `--out` without building it, types the commands once
the boot ends, and exits 0 if every one passed: no FAILED or error line, and
a test's own `<name>: ok` line. A misspelt test name is a FAIL, after
`--command-timeout`. Logs go to `$TMPDIR/ours-kshell-<out>/`.

## Reading a failure

| Summary says | Meaning | Next step |
|---|---|---|
| `error-level line` | panic or failed ASSERT | read the printed context; for a backtrace, break on `vpanic` with the `lldb` MCP tool: [references/debugging.md](references/debugging.md) |
| dbg FAIL, rel PASS | an assertion caught it; rel ran past it with assertions compiled out | fix the assertion's cause, not the assertion |
| both FAIL at the same point | nothing is masked by an assertion | debug the shared stage shown in `reached:` |
| `no "--- end ---" within Ns` | hang or livelock | `reached:` shows the last stage; rerun with `SMP=1` to test for an SMP race |
| `no "<line>" within Ns` | the shell finished, but a line the image requires (`REQUIRED` in `boot_check.py`) never came: on rel, `routing_checks: passed`, from the root's test component, or a `pc-ps2:` line, from the keyboard driver | read the `init:` and `pc-ps2:` lines in `rel.log`: a component that did not start, or a use that routes nowhere or cannot be served, is logged there, and so is a driver that could not take its ports or line |
| `QEMU exited` | reset or triple fault (`-no-reboot` turns a reset into an exit) | debug under the gdbstub |

## Gotchas

- Build and boot on the macOS host with `prebuilt/` only. Never use the
  `dev-common` container, never build the `ours-dev` image, never install into
  system directories. If a prebuilt tool is missing, say so and stop rather
  than substituting something close and calling it verified.
- `qemu-system-x86_64` is not on PATH (qemu_boot.py uses `prebuilt/qemu`),
  there is no `timeout` command, and the kernel never exits. Never start
  qemu_boot.py in the foreground; it hangs the session.
- The dbg image logs `I'm Thread 1/2` at debug level forever, thousands of
  lines a second. `tail` of a dbg log shows nothing else, so search for
  markers instead of tailing.
- Expected noise, not failures: GRUB's `WARNING: no console will be available
  to OS` and `error: no suitable video mode found.`; in dbg, `fault_probe`
  terminated with FatalPageFault and `Exception[divide error] from user space`
  (deliberate fault tests); the host linker's `ignoring -lto_library` warning.
- Objects orphaned by a rename stay in `out/` and can satisfy a link, hiding
  breakage. If a build looks healthier than it should, rebuild into a fresh
  `--out`; a full build takes about 30 seconds.
- Other sessions often share this worktree. Two sessions must not build or
  boot from the same out dir; use `--out out/<your-lane>`.
- If the shell's closing output changes, update `END_MARKER` in the script.
