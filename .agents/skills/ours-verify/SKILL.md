---
name: ours-verify
description: Build the ours kernel and boot both dev images in QEMU to prove a change works, and debug a boot that fails (breakpoints and backtraces through the lldb MCP server). Use after any change to kernel, phys, userboot, init, shell or build files, before every commit, whenever the user says verify, build and boot, "验证一下", "跑一下", "启动看看", or asks whether something still boots, and when a boot panics, asserts or hangs ("panic 了", "卡住了", "看下调用栈"). A clean compile is not verification in this repo.
---

# Verify an ours change

Done means it builds, the host tests pass, **both** images boot, and the
shell finishes its self-checks. "It compiles" is not done, and neither is
booting one image.

## Run it

From the repo root:

```bash
python3 .agents/skills/ours-verify/scripts/boot_check.py
```

It builds `out/bringup.x86` with `prebuilt/ninja`, boots `kernel.dev.dbg_1.obi`
and `kernel.dev.obi` side by side, stops QEMU once each verdict is in (about
25 seconds end to end), and prints one line per image. Exit code 0 means both
passed. Paste those lines to the user as the evidence.

While the images boot it also runs the host tests and prints a `host-tests`
line: every executable `//:default` builds in the host toolchain with
`testonly = true`, except `//benches`, asked of GN on each run (`gn ls`,
`gn desc //:default deps --all`, `gn outputs`), so a new test runs as soon
as it is in `//:default`. Each runs from the build directory and passes when
it exits 0; one failing fails the check, with its last lines printed. All of
their output is in `host-tests.log` beside the boot logs.

On rel it also types keys: once the PS/2 keyboard driver logs `pc-ps2: ready`,
it sends `sendkey a` and `sendkey shift-a` to a QEMU monitor on a socket of
its own, not the one behind Ctrl-A c, whose echo would land inside the serial
log's lines, and requires `pc-ps2: key a` and `pc-ps2: key A`. That is the
one check of a device's interrupt reaching a user-space driver. What the
monitor answered is in `rel.monitor.log`, beside `rel.log`.

A boot that falls silent reports itself. Every image has such a monitor
(`dbg.monitor.log` too), and once the kernel has printed its first line, 15
seconds without a line before the end marker sends it `nmi`. The NMI reaches
cpu0 whatever its interrupt flag, the lockup detector prints every CPU's
state and panics, and the image's line says `console silent for 15s before
the end, so QEMU was sent nmi: lockup report, ...`, with the report printed
under it and kept in the log (see "Reading a lockup report" below). A boot
already quiet when its timeout comes is let run until it is NMI'd.

Flags: `--out out/<dir>` (configured automatically if new), `--no-build`,
`--only dbg|rel`, `--timeout N`, `--no-tests` (skip the host tests, for a
loop of boots only; never for the check before a commit). `SMP=1`, `NUMA=1`
and the other `scripts/qemu_boot.py` variables pass through.

`NUMA=1` is what builds the four-node machine, and it decides the topology on
its own: it is read from the environment by `scripts/qemu_boot.py` and nothing
derives it from the out directory or from the build's own `mm_numa`. The two
are needed together and act at different times -- `mm_numa = true` is a GN
argument that sets `MAX_NODES` and compiles the SRAT parse, and changes no
byte of QEMU's command line; `NUMA=1` makes the firmware present the tables
that parse reads. Either one alone gives a single node, and so does forgetting
it on the second of two commands: it has to be on `kshell_run.py` too, which
passes the environment through rather than inferring it.

```bash
python3 tools/set.py bringup.x86 --out out/numa.x86 --args 'mm_numa=true'
NUMA=1 python3 .agents/skills/ours-verify/scripts/boot_check.py \
  --out out/numa.x86
NUMA=1 python3 scripts/kshell_run.py --out out/numa.x86 pmnode.test
```

A four-node boot is the only way to exercise the fallback order, a node's hard
affinity, `Gaf::ThisNode` and the per-node watermarks at all: with `MAX_NODES`
at 1 every one of those passes by having nothing to distinguish. It is not
part of the standard check, so run it by hand after a change to any of them.

`NUMA=<n>` for `n >= 2` builds `n` generated nodes instead, one CPU each while
the CPUs last and just under 1 GiB split between them. That machine is for the
limits rather than the ordering: `NUMA=33` is the smallest one whose firmware
names a proximity domain past `MAX_NODES`, so it is the only way to run the
truncation paths in `kernel/arch/x86/mm/srat.cpp`, and it declares no
distances, so its firmware carries an SRAT and no SLIT -- the shape most real
machines have, and one `NUMA=1` cannot produce. Neither machine covers the
other, so a change to the SRAT or SLIT parse wants both.

For C++ edits, `cpp-write-and-verify` (clang-format and clang-tidy) comes
first; this skill checks behaviour, that one checks the source.

## Compiler cache

Every worktree's compiles share one ccache, so a source another worktree has
compiled at the same commit is copied out, not compiled again: a full build of
`out/bringup.x86` takes about 6 seconds from a warm cache against about 40
cold. `tools/set.py` turns it on, writing
`compiler_launcher = "//prebuilt/ccache/ccache"` into `args.gn`, whenever
`prebuilt/ccache/ccache` exists, and `scripts/new_worktree.py` and an
unconfigured `--out` both go through `tools/set.py`. A build directory
configured before 2026-10-07 has no such line: re-run `tools/set.py` on it.

- Fetch the binary, once per machine: `python3 scripts/fetch_ccache.py`.
- Hits and size, from the checkout root:
  `CCACHE_CONFIGPATH=build/ccache.conf prebuilt/ccache/ccache -s`. Without
  the variable ccache reads its default cache, not this one.
- Build without it: `tools/set.py <product>.<arch> --args
  'compiler_launcher=""'`.
- Settings (cache directory, size limit, sloppiness): `build/ccache.conf`.

To rule the cache out of a failure, build the same commit into a fresh
`--out` configured with `compiler_launcher=""`: if that passes, suspect the
cache.

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

It sends the same `nmi` after 15 seconds of silence (`--silence N`, 0 for
never): on dbg until the run ends, since its debug threads never stop
logging, and on rel only until the boot's end marker, since its shell waits
quietly. `crash`, `lockup nmi`, `lockup spin <cpu>` and `lockup spin all`
end the machine, so each can only be the last command; the script judges the
report they lead to instead of a prompt. `lockup spin all` is reported only
through the script's `nmi`, which is the test of that path.

## Reading a failure

| Summary says | Meaning | Next step |
|---|---|---|
| `error-level line` | panic or failed ASSERT | read the printed context; for a backtrace, break on `vpanic` with the `lldb` MCP tool: [references/debugging.md](references/debugging.md) |
| dbg FAIL, rel PASS | an assertion caught it; rel ran past it with assertions compiled out | fix the assertion's cause, not the assertion |
| both FAIL at the same point | nothing is masked by an assertion | debug the shared stage shown in `reached:` |
| `no "--- end ---" within Ns` | hang or livelock | `reached:` shows the last stage; rerun with `SMP=1` to test for an SMP race |
| `console silent for Ns before the end, so QEMU was sent nmi` | every CPU stopped, most likely each with interrupts off | read the report printed under it: [Reading a lockup report](#reading-a-lockup-report) |
| `lockup report, lockup: cpuN took no tick for 10 s` | cpuN stopped taking its tick, and another CPU found it | the same; cpuN's block is printed last |
| `no "<line>" within Ns` | the shell finished, but a line the image requires (`REQUIRED` in `boot_check.py`) never came: on rel, `routing_checks: passed`, from the root's test component, or a `pc-ps2:` line, from the keyboard driver | read the `init:` and `pc-ps2:` lines in `rel.log`: a component that did not start, or a use that routes nowhere or cannot be served, is logged there, and so is a driver that could not take its ports or line; for a missing `pc-ps2: key` line, `rel.monitor.log` shows whether QEMU took the keys |
| `host-tests FAIL` | a host test exited non-zero, or GN could not list them | the failing test's last lines are printed; `host-tests.log` has all of its output. Run the binary from the build directory to reproduce: `cd out/<dir> && ./host/obj/<path>` |
| `QEMU exited` | reset or triple fault (`-no-reboot` turns a reset into an exit) | debug under the gdbstub |

## Reading a lockup report

The lockup detector (`//kernel/lib/lockup_detector`) prints one report per
boot, then panics. A header says why, then one block per CPU: its thread, pc,
whether interrupts were on, the spin locks it held and the ticks it had
taken, five register lines, and a backtrace. From `lockup spin all` on dbg:

```text
lockup: dump asked for by an NMI on cpu0; each CPU's state follows
lockup: cpu0 (reporting) thread lockup-spin at +0x86132, interrupts off, 0 spin locks held, 183 ticks taken
  rip=0xffffff8840086132 (+0x86132)  cs=0x10  rsp=0xffffff88040a8d50
  (rax to r15, four lines)
lockup: cpu0 backtrace +0x86132 +0x2753c
lockup: cpu1 thread lockup-spin at +0x86132, interrupts off, 0 spin locks held, 158 ticks taken
(cpu1 to cpu3 alike)
OURS KERNEL PANIC
lockup: dump asked for by an NMI on cpu0
```

- The header is `dump asked for by an NMI on cpu<N>` (the scripts' `nmi`,
  `lockup nmi`), or `cpu<N> took no tick for <T> s, found by cpu<M>` with a
  second line counting, per CPU, the checks in a row it went without a tick:
  the larger, the earlier it went quiet. The stuck CPU's block comes last.
- In a hang, a CPU spinning on or holding a spin lock has `interrupts off`,
  and `spin locks held` says whether it holds any. `ticks taken` far behind
  the others (about 100 a second) says it stopped earlier. A CPU caught in
  ordinary work can have interrupts off too, for a moment.
- `cpu<N> did not answer its NMI` or `was sent no NMI` replaces a block the
  report could not get.

Addresses are offsets from the start of the kernel image, which is linked at
0, so the offset is the address in the unstripped ELF: for dbg
`out/<dir>/kernel_x86.dbg.1/obj/kernel/products/bin/vmours`, for rel
`out/<dir>/kernel_x86.dbg.0/obj/kernel/products/bin/vmours.with-tests`. A
one-byte disassembly names the function around an offset (there is no
`llvm-symbolizer` in `prebuilt/`):

```bash
prebuilt/clang/llvm-23/bin/llvm-objdump -d -C --no-show-raw-insn \
  --start-address=0x86132 --stop-address=0x86133 \
  out/bringup.x86/kernel_x86.dbg.1/obj/kernel/products/bin/vmours
```

```text
0000000000086100 <(anonymous namespace)::spin_routine(void*)>:
   86132:       testb   %r14b, %r14b
```

So `+0x86132` is `spin_routine+0x32`. A backtrace entry after the first is
a return address: the call is the instruction before it, so widen the range
backwards to see it. `llvm-nm -n -C` on the same ELF lists every symbol in
address order. The walk follows frame pointers within the running thread's
kernel stack, so it misses a leaf function's caller and stops at a stack it
cannot see.

To hunt an intermittent hang, loop `kshell_run.py` over the commands the hang
was seen in, capped, stopping at the first run that fails: a run that hangs
is NMI'd and leaves its report in the log. Each run writes over the last
one's log, which is why the loop stops at the first failure.

## Gotchas

- Build and boot on the macOS host with `prebuilt/` only. Never use the
  `dev-common` container, never build the `ours-dev` image, never install into
  system directories. If a prebuilt tool is missing, say so and stop rather
  than substituting something close and calling it verified.
- `qemu-system-x86_64` is not on PATH (qemu_boot.py uses `prebuilt/qemu`),
  there is no `timeout` command, and the kernel never exits. Never start
  qemu_boot.py in the foreground; it hangs the session.
- **The boot logs are not text files, and plain `grep` lies about them.** They
  are raw serial capture and carry a few NUL bytes, so BSD grep calls them
  binary and then reports **no matches at all** -- no output, exit status 1,
  not even a count from `-c`. `grep -c CPU dbg.log` prints nothing on a log
  holding thousands of `CPU` lines, which looks exactly like a log that does
  not hold any. Use `grep -a` (or read the file in Python with
  `errors="replace"`) for every search of a boot log. On 2026-10-29 this cost
  three wrong conclusions in a row and a repair item filed for a bug that did
  not exist: the four-node NUMA boot was working, and the log said so, and
  the grep said the log was silent. If a search of a 600 KB log finds
  nothing, check that it finds *something* before believing it.
- The dbg image logs at debug level, heavily while it boots, so search a dbg
  log for markers instead of tailing it.
- Expected noise, not failures: GRUB's `WARNING: no console will be available
  to OS` and `error: no suitable video mode found.`; in dbg, `fault_probe`
  terminated with FatalPageFault and `Exception[divide error] from user space`
  (deliberate fault tests); the host linker's `ignoring -lto_library` warning.
- A machine with RAM above 4 GiB does not boot. On q35 that is anything over
  `MEM=2816`, where QEMU starts placing RAM above the 4 GiB PCI hole:
  `MEM=3072` gets through phys and fails an `ASSERT` preparing the AP
  trampoline (`docs/repairs/261029/x86-kernel.md`). `MEM=2560` and below boot.
- Objects orphaned by a rename stay in `out/` and can satisfy a link, hiding
  breakage. If a build looks healthier than it should, rebuild into a fresh
  `--out`; a full build takes about 30 seconds.
- Other sessions often share this worktree. Two sessions must not build or
  boot from the same out dir; use `--out out/<your-lane>`.
- If the shell's closing output changes, update `END_MARKER` in the script.
