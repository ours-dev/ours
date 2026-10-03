# user-shell: implementation plan

**Date**: 2026-10-03
**Status**: Accepted

## Request
Replace the syscall-demo behavior in `examples/shell/shell.cpp` with a better interactive shell. Scope is the debug-only serial bridge and a usable BootFS command shell, not a general I/O subsystem.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Input API | `debug_read`; general terminal service | `debug_read` with Debug resource and opt-in boot option | Not stated; user said "do it" after the proposed boundary | 2026-10-03 |
| Serial ownership | Kernel shell; user shell; shared pollers | User shell owns COM1 in dev images | Not stated; input theft was explained before approval | 2026-10-03 |

## Steps
1. Add `debug_read`, the System Debug resource slot, platform read boundary, and capability handoff. Verify the generated ABI and build before adding the shell loop.
2. Add bounded line editing, built-ins, and BootFS program launch. Map the BootFS once read-only because this tree has no unmap syscall; keep startup checks and the boot marker. Enable the userspace reader and disable the kernel COM1 shell in the dev image.
3. Run source formatting/lint, exercise built-ins and child launch over QEMU serial, and run `ours-verify` for both dev images. Record results and review the diff; do not commit mixed user changes in `examples/shell/shell.cpp` or `kernel/lib/userabi/src/userboot.cpp`.

## Files
- `.agents/tasks/user-shell/DESIGN.md` — accepted design.
- `system/public/ours/syscalls.gni`, `system/public/ours/syscall.hpp`, `system/public/ours/syscall/resource.hpp` — debug-read ABI and resource slot.
- `kernel/lib/syscall/src/debug.cpp`, `kernel/platform/**`, `kernel/top/boot_options.cpp`, `kernel/include/ours/boot/options.hpp` — gated platform input path.
- `kernel/lib/userabi/src/userboot.cpp`, `system/lib/ort` userboot startup, `system/service/init/init.cpp` — capability handoff.
- `examples/shell/shell.cpp`, `examples/shell/BUILD.gn`, `kernel/products/image/dev/BUILD.gn` — interactive shell and image wiring.
- `docs/changes/261003/user-shell.md` — change record.

## Test plan
- Generate and compile the vDSO syscall ABI for the x86 dev image.
- Run `python3 scripts/format_cpp.py <path>` and `python3 scripts/format_cpp.py --check <path>` on changed C++ files; run prebuilt clang-tidy against the build compile database.
- Feed shell input through QEMU serial and verify built-ins, line editing, BootFS program launch, and clean exit.
- Run `python3 .agents/skills/ours-verify/scripts/boot_check.py` and require both images to reach `--- end ---`.
