# Debugging a failed boot

Use the `lldb` MCP server (`.mcp.json`, served by `scripts/lldb_mcp.py`): its
`command` tool runs one lldb command in a live debugger and returns the output.
The debugger already has `scripts/lldb_ours.py` loaded, so `cl`, `cpus`,
`nodes` and `kthread` work from the first command. `scripts/qemu_debug.py` and
`scripts/lldb_ours.py` document themselves; read their docstrings for detail.

## Attach and stop where you want

1. Start `scripts/qemu_debug.py` in the background. It boots the image halted
   before its first instruction, links the matching ELF to
   `out/.debug/kernel.elf`, and prints `gdbstub ready on localhost:<port>` when
   it can be attached. It never exits on its own.
   - A private build: `OUT=$PWD/out/<dir>`. `OUT` must be absolute; a relative
     one leaves a broken `out/.debug/kernel.elf` link and every breakpoint
     silently stays unresolved.
   - Another session may be debugging too: pick your own `PORT`.
2. Through the `command` tool, one command per call:
   `target create out/.debug/kernel.elf`, then
   `target modules load --file kernel.elf --slide 0xffffff8840000000`, then
   `gdb-remote localhost:<port>`. The session is now stopped at the reset
   vector (`0xfff0`).
3. Set breakpoints now (`breakpoint set -n start_kernel`, `-n vpanic`). QEMU
   implements them itself, so they hold even though the kernel is not loaded
   yet.

## Run, wait, inspect

- `continue` returns at once ("Process 1 resuming"). Poll `process status`
  every second or two until it says `stopped` and shows the stop reason.
- `process interrupt` stops a running kernel, landing on whichever vCPU it
  catches; `thread list` shows all of them.
- Inspect only while stopped: `bt`, `register read`, `memory read`, `cl <var>`.
  While running these fail with "requires a process which is currently
  stopped".
- The server handles one request at a time, so a command that blocks holds up
  every call after it.
- Finish with `process kill`, then stop `qemu_debug.py`. The link it wrote
  belongs to whoever booted last; leave it.

## Without the MCP server

`/usr/bin/lldb --batch` with the attach line `qemu_debug.py` prints, plus
`-o 'breakpoint set -n vpanic' -o 'continue' -o 'bt all'`. Boot under the
gdbstub is far slower than the ~3 seconds of a normal boot; allow over a minute.

To place a bare instruction address from a crash dump without a debugger, look
it up in `out/<dir>/kernel_x86.dbg.1/obj/kernel/products/bin/vmours` with
`prebuilt/clang/llvm-23/bin/llvm-nm -C -n` or `llvm-objdump -d`; the kernel runs
at slide `0xffffff8840000000`.
