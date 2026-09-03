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
