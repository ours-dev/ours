# scripts changes

## Title: scripts/kshell_run.py types kernel-shell commands into a booted image and judges each (2026-10-02)

**Files**
- `scripts/kshell_run.py` — new: boots one dev image, waits for the boot to end, types each command into COM1, prints what it printed, and exits 0 only if every one passed.
- `kernel/lib/kshell/README.md` — how to drive the shell with it, and why the prompt cannot end a command on dbg.
- `.agents/skills/ours-verify/SKILL.md` — a "Kernel tests" section: the `*.test` commands do not run at boot, and this is how to run them on both images.

**Symptom**
None; a tool. The kernel's `*.test` commands (`//kernel/commands`) are not part of the boot, so they could only be run by hand at QEMU's serial console. A throwaway script had done it during the CanMapSpecific work, but it could not send a second command to the rel image.

**Cause**
The objects:
- COM1, which QEMU connects to the script's pipes: what the kernel writes, and what is typed to it.
- The kernel shell, a thread that polls COM1 every 10 ms. It echoes what is typed, runs a line's command synchronously when the line ends, and then writes its prompt, `kshell> `, a byte at a time and outside the log's lock.
- The logger, which writes whole lines. On the dbg image other CPUs log `I'm Thread N` thousands of times a second, for good.
- A test command, which logs one line per check and ends with `<name>: ok -- N checks` or `<name>: FAILED -- ...`.

The events, for each command:
1. The boot ends with the user shell's `--- end ---`.
2. The command is typed. The shell echoes it and runs it.
3. The command logs its lines, a test its verdict last.
4. The shell writes its prompt.

Four traps, found in this order while writing it:
- The prompt ends no line, so a reader that takes the port a line at a time never sees it on a quiet image. The throwaway script only acted when a line arrived, so its next command never went out on rel. The port is read in raw chunks.
- Under the dbg image's chatter, a reader that drains the port until it is empty never returns, so no deadline is ever looked at. It ran for ten minutes and wrote 200 MB of log. Each pump takes a bounded burst.
- Keeping everything and searching it again on every read is quadratic in what the dbg image writes. Only one command's output is kept; the boot's is dropped, and only a short tail is searched again.
- On dbg the prompt's bytes land between other CPUs' log lines: the log showed `clpu2` and `c>pu2`, the `l` and `>` of `kshell> `. So the prompt is seldom whole there, and a test command is done at its verdict line instead, which the logger writes whole. Any other command still waits for the prompt and is best run on rel.

**Change**

```python
if test:
    done = re.compile(re.escape(name) + r": (?:ok|FAILED)\b[^\n]*\n")
else:
    done = re.compile(re.escape(PROMPT))
console.keep()
console.type(command)
ended = console.wait_for(done, time.monotonic() + args.command_timeout, through=test)
```

A command fails on a FAILED or error-level line, on no end within `--command-timeout`, or, for a `*.test` command, without its own `<name>: ok` line. The kernel shell says nothing about a name it does not know, so a misspelt test would otherwise pass by printing nothing. The boot, the environment it passes to `scripts/qemu_boot.py` and the log-level patterns are `boot_check.py`'s.

**Reference**
None, this project's own tool. The ours-verify skill's `boot_check.py` is its model.

**Verification**
- dbg, `vmaseal.test vmosnapshot.test`: both ok, exit 0.
- rel, `vmaseal.test vmosnapshot.test usercopy.test pmnode.test log`: all ok, exit 0, five commands in one boot.
- rel, `nosuch.test` with `--command-timeout 10`: the shell's prompt came back with no verdict; FAIL, exit 1.
- Not tested: a test that fails. No test fails today, and judging a FAILED line is the same pattern match as in `boot_check.py`.

**Deliberately not done**
- Building: it boots what is in `--out`, as `boot_check.py --no-build` does.
- A quick FAIL for a misspelt test: it waits out `--command-timeout`, since on dbg a returning prompt cannot be told from a scattered one.
- An interactive mode: QEMU's console already is one.

**How it was found**
None; the user asked for the throwaway script to become a tool in the repo.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can a line-by-line reader never see the kernel shell's prompt on a quiet image?
- Why does the prompt break up on the dbg image while a test's verdict line does not?
- Why does a test command have to print its own "ok" line to pass?
