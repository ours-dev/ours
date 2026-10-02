# libkshell

A minimal serial-interactive shell over the gktl command registry. The
registry (`GKTL_COMMAND`) declares commands in a linker section; this is the
missing front end: a kernel thread polls COM1, assembles lines (with echo
and backspace) and dispatches them through `gktl::run_command`.

## Shape

- One thread (`kshell`), spawned at `gktl::InitLevel::Platform`, sleeping
  between polls so it does not pollute profiles.
- No line editing beyond backspace, 16-argument cap, 128-byte lines —
  bring-up scope by design.

Drive it interactively over QEMU's serial console, or from scripts:
`prof.start tick 4` / `prof.stop` / `prof.dump` (see libprof).

`scripts/kshell_run.py` does the typing for you: it boots an image, waits for
the boot to end, types each command and reports what it printed, for example
`python3 scripts/kshell_run.py --out out/mine --image rel vmosnapshot.test`.
A `*.test` command is done at its `<name>: ok` or `FAILED` line, any other at
the next prompt. The prompt is written a byte at a time, outside the log's
lock, so on the dbg image other CPUs' log lines break it up: there only a
test's verdict line is a dependable end.

## Turning it on and off

The shell runs every registered command, in ring 0, for whoever can write to
the serial line, so it is switched at two levels:

- **Boot:** the thread starts only when the image's command line says
  `kernel.shell=true` (or bare `kernel.shell`). It is off by default, as in
  Zircon; the bring-up images ask for it in their `kernel_cmdline`
  (`//kernel/products/image/dev`, `dev.dbg_1`). Off, the log says
  `kshell: not started (kernel.shell is off)`.
- **Build:** the GN argument `kernel_shell_enabled = false`
  (`//kernel/kernel_params.gni`) leaves this library and `//kernel/commands`
  out of the link, whatever the command line says.

      tools/set.py bringup.x86 --args kernel_shell_enabled=false
