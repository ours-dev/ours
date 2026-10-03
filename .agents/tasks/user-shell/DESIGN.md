# user-shell: interactive user-mode serial shell

**Date**: 2026-10-03
**Status**: Accepted

## Problem
`examples/shell/shell.cpp` is a ring-3 syscall smoke-test program, not a command shell. Userspace can print through `debug_write` but has no input path. The dev image currently starts the kernel shell on COM1, so two independent readers cannot safely own serial input.

## Goals and non-goals
- Replace the user-visible behavior with an interactive shell that supports bounded line editing, `help`, `echo`, `commands`, `clear`, `exit`, and launching programs from BootFS.
- Keep existing syscall smoke checks and the `--- end ---` boot-verification marker.
- Add only debug serial input for this bring-up image. This is not general stdin, a terminal object, VFS integration, pipes, redirection, or job control.
- Make the user shell the sole COM1 input reader in the dev images; retain the existing kernel smoke checks.

## Design
Add `ours::debug_read` to the generated vDSO ABI. It takes a System Debug resource, a bounded output buffer, and an actual-byte count. The call is disabled unless `kernel.debug_read` is enabled and validates the resource with `object::validate_ranged_resource`. The PC platform implements the blocking first-byte/read-available behavior, matching Zircon `sys_debug_read()` and `platform_dgetc()`; unsupported platform implementations answer `NotSupported`.

The kernel mints the System Debug resource for the first user process. Userboot hands it to init, and init transfers it with duplicated Job, BootFS, VMEX, and vDSO handles to the shell. The shell maps the BootFS once, read-only, for its lifetime and passes file spans to the existing `launch` library. This avoids a fixed executable-size buffer and does not need an unmap syscall. The dev image enables `kernel.debug_read` and disables `kernel.shell`, making the user process the only COM1 reader.

The existing syscall checks remain a distinct startup self-check phase before the prompt, so the boot verifier still observes `--- end ---` and the established checks still run. The shell uses a bounded line buffer, handles CR/LF and backspace, supports `help`, `ls`, `echo`, `commands`, `clear`, and `exit`, and launches foreground BootFS programs. Unknown or overlong input is reported without overrunning storage.

## Alternatives considered
- A general stdin/terminal/VFS stack: rejected for this bring-up slice; no terminal or file-descriptor object exists, and the shell only needs the debug serial input path today.
- A fixed-size executable scratch buffer: rejected after the packaged shell itself exceeded the 64 KiB limit. A single read-only BootFS mapping supports arbitrary packaged file sizes without requiring an unmap operation after each command.
- Let both the kernel shell and user shell poll COM1: rejected because either reader can consume a byte intended for the other.
- Read input through `debug_write` or the init channel: rejected because the first is output-only and the second is only a startup/liveness channel, not an interactive console.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| First I/O boundary | Debug syscall; general terminal service | Debug syscall, gated by a System Debug resource and boot option | Not stated; user approved the proposed debug-only bridge | 2026-10-03 |
| COM1 input owner | Kernel shell; user shell; shared polling | User shell in dev images; kernel shell disabled there | Not stated; assistant surfaced keystroke theft as the race | 2026-10-03 |

## Testing and verification
- Build the generated syscall ABI and the x86 dev image.
- Run clang-format and clang-tidy on every changed C++ file.
- Exercise shell commands through QEMU serial input, including editing, command dispatch, program launch, and exit.
- Run `ours-verify` to build and boot both dev images and confirm the smoke checks reach `--- end ---`.

## Open questions
- None
