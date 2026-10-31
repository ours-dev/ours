# shell outstanding repairs

## 1. The user shell's list of kernel commands ends at 2048 bytes: 20 of 41 commands are shown, the `commands` built-in does not say so, and which 20 follows link order

**Severity**
Medium: the boot listing says it was cut, and the shell's own comment accepts a truncated list, but half of the registry is missing from every dbg and rel boot and from the `commands` built-in, which prints nothing about the cut. Which 20 are shown is decided by where the linker put each object, and nothing fails or warns when a new command pushes another one off the end.

**Symptom**
The user shell reads the registry through `ours::command_list` in two places, both into the same `Registry`: the `--- commands ---` listing it prints at the end of every boot (`system/app/shell/shell.cpp:715-725`), and the `commands` built-in typed at its prompt (`shell.cpp:521-529`). The boot listing ends with `pagesource.test  pagesource.test: page faults that wait for a p(truncated: the registry outgrew this program's buffer)` and then `--- end ---`. The built-in prints the same text and no tail, because it reports every status except `NoSpace`. Measured on the build of `3eb20421` (x86 `out/bringup.x86`, the dbg and rel boot logs of `boot_check.py`; the built-in was not run, it reads the same buffer by the same call):
- The registry text is 4002 bytes: 41 `GKTL_COMMAND` records summed from the sources by a throwaway script, each as `command_list` writes it (`<cmd>`, then two spaces and the help string if there is one, then a newline). The shell's buffer holds 2047 bytes and a NUL.
- 20 commands are shown, the last with its help cut: `interruptline.test`, `ioport.test`, `irq.test`, `timershare.test`, `dump-frame`, `largepage.bench`, `preemptdeadline.bench`, `cov`, `cpu`, `crash`, `dpc.test`, `hotplug.test`, `idle.stats`, `idle.test`, `interrupt.test`, `largepage.test`, `lockup`, `log`, `mutex.stress`, `pagesource.test`. Both images show the same 20.
- 21 are not shown: `timer.test`, `pager.test`, `pmzone.test`, `vmaundo.test`, `tickfallback.test`, `tickreplace.test`, `port.test`, `threadjoin.test`, `samplerclaim.test`, `resource.test`, `sleep.test`, `vmawx.test`, `vmaphys.test`, `pmnode.test`, `preempt.test`, `usercopy.test`, `vmosnapshot.test`, `waitqueue.test`, `vmaseal.test`, `semaphore.test`, `tickbcast.test`.

Moving four tests to `//kernel/arch/x86/commands` (`3135d44a`) changed which are shown, because the registry is in link order. Reconstructed from `//kernel/commands`' sources list before the move (the same method reproduces today's 20 names exactly, which is its check, but it is not a measurement of the old build): `timershare.test` was not shown and is now, `pager.test`, the last and half-cut entry, is not shown now, and `pagesource.test` was whole and is cut.

**Cause**
Three things, and the second is why the first cannot be fixed by looking at the list:
- `Registry::m_text` is a fixed `char[2048]` (`shell.cpp:100`). Its comment says it is sized rather than grown because the image has no allocator, and that the part that fits is still shown. The registry text is now about twice the buffer.
- `ours::syscall::command_list()` (`kernel/lib/syscall/src/command.cpp:85`, the walk over the records at line 98) writes the records from the start of `.gktl.commands` and has no offset argument. A caller that gets `NoSpace` cannot ask for the rest, so what is lost is whatever the linker placed last.
- The order is link order, so it changes when an object moves between targets, as it did on 2026-10-31, and when a command is added anywhere. Nothing checks which commands are shown or that the list ended: no script under `scripts/` or `.agents/skills/ours-verify/` reads the "(truncated" tail.

**Change**
Options, for the user to choose between; the second changes the user ABI, so it is not mine to pick:
- Enlarge the buffer to cover the registry with room: 8 KiB is about twice today's text and sits in the shell's `.bss`. One line, closes the problem for today, and moves the cliff to about 80 commands.
- Add an offset to `command_list(buffer, length, offset)`, and have the shell ask until a call returns less than it was given. No limit and no size to tune. It changes an entry of `system/public/ours/syscalls.gni` (the vDSO stub, the declaration and the kernel shim are generated from it) and needs the registry to be the same between calls, which it is: it is fixed at link time.
- Sort the text by name in the kernel, so that what is cut is at least predictable. It does not show one command more, so it is not the fix, but a sorted list reads better under either option above.

Whichever is chosen, make the cut visible to the check, and to the built-in: `boot_check.py` fails a boot whose shell list prints "(truncated", and the `commands` built-in prints the same tail the boot listing does.

**Reference**
Zircon's `help` command (`zircon/kernel/lib/console/mod.rs:420`, `cmd_help`) prints from inside the kernel, and sorts alphabetically when it may allocate, so it has no buffer to outgrow and its order does not follow the link. Here the list crosses a syscall into a user program, which is this project's own design, so the buffer is the shell's.

**Acceptance**
The dbg and rel boot logs contain the whole list: 41 lines, ending with the last registered command, and no "(truncated". The `commands` built-in prints the same. `boot_check.py` fails when the tail is printed. x86 `ours-verify` and the riscv check pass.
