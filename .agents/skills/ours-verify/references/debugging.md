# Debugging a failed boot

The two scripts that do this are documented in their own docstrings; read
those first, they stay current with the code:

- `scripts/qemu_debug.py` boots an image halted under QEMU's gdbstub and
  publishes the matching ELF at `out/.debug/kernel.elf`. It prints the exact
  lldb command line once the stub is listening.
- `scripts/lldb_ours.py` adds `cl` for CPU_LOCAL variables. A plain `p` on a
  CPU_LOCAL symbol prints the linker template, not any CPU's value.

## Driving it from an agent session

- Start `scripts/qemu_debug.py` in the background: it waits for a debugger
  and never returns on its own.
- Use the system `/usr/bin/lldb --batch` with the command line qemu_debug.py
  prints, plus `-o 'breakpoint set -n vpanic' -o 'continue' -o 'bt all'`.
  Always import `scripts/lldb_ours.py` first.
- Boot under the gdbstub is far slower than the ~3 seconds a normal boot
  takes. Allow well over a minute before giving up, then kill both lldb and
  QEMU.

## Without a debugger

To place a bare instruction address from a crash dump, look it up in the
symbolled ELF, `out/<dir>/kernel_x86.dbg.1/obj/kernel/products/bin/vmours`,
with `prebuilt/clang/llvm-23/bin/llvm-nm -C -n` or `llvm-objdump -d`. The
kernel runs at slide `0xffffff8840000000`.
