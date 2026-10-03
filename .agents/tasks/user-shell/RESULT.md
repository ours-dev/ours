# user-shell: interactive user-mode serial shell

**Date**: 2026-10-03

## Delivered
- Added the opt-in, System-Debug-resource-gated `debug_read` syscall and PC COM1 input implementation.
- Passed the Debug resource through userboot and init; init gives the shell duplicated launcher handles.
- Replaced the shell's test-only exit with an interactive line editor, built-ins, and foreground BootFS program launching. Existing syscall checks and `--- end ---` remain.
- Made the user shell the sole COM1 input reader in the dev image.
- No commit was made: `examples/shell/shell.cpp` and `kernel/lib/userabi/src/userboot.cpp` contain existing user changes, and a path-level commit would include those unrelated hunks.

## Verification
- GN generation and `gn check target/default` passed.
- Changed C++ lines and GN files passed repository formatting checks; changed translation units were compiled with clang-tidy. The existing `H` test variable reports a style warning only.
- `ours-verify` passed: debug image 19 passed, 0 skipped, 0 failed; release image 18 passed, 0 skipped, 0 failed.
- Live release QEMU serial test passed `help`, `ls` (five BootFS entries), `echo hello world`, and launching the packaged shell as a foreground child; `exit` returned and closed the parent shell.
- Task and change documents passed `scripts/lint_docs.py`.

## Deviations from the design
None.

## Found along the way
- The pre-existing BootFS program scratch buffer was 64 KiB, smaller than the packaged shell. Replaced it with one persistent read-only BootFS mapping; child launch was verified live.
- An initial patch displaced the BootFS handle's existing `KernelHandle` expression. It was restored before verification, and the user's W^X loader change remains intact.

## Not done
General stdin/VFS terminal support, IRQ-driven UART receive, history, quoting, pipes, job control, and kernel-shell command automation remain out of scope.
