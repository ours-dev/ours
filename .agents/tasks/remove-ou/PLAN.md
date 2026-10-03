# remove-ou: ring 3 calls the vDSO's ours_* directly, and ou goes

**Date**: 2026-10-03
**Status**: Accepted

## Request
The user, on 2026-10-03: `ours::syscall::*` is the kernel side and `ours_*` is already the user-level API, so `ou` is a layer with no reason to exist. Remove it: every program calls `ours_*` directly, and the few helpers in `ou` that are not forwarders (`debug_write`'s splitting, `debug_line`, `decimal`, `hex`) move to the runtime. The scope ends there: no change to the ABI, to `syscalls.gni`, or to what any program does.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Keep `ou` | keep it as is, unenforced (A); remove it, call `ours_*` directly (B); enforce it and generate a typed `ou` (C) | A at first, then B' (remove the forwarders, keep the helpers in the runtime) once the user pointed out `ours_*` is already the API | None | B, 2026-10-03 |
| Where the helpers go | the `runtime` source set; a target of their own beside it; a new library | a target of their own in `//system/lib/runtime` (`:debug`): the runtime source set carries `crt0.S`, and userboot and fault_probe, which use the helpers, have entry points of their own | None | The runtime, 2026-10-03 (the target follows from crt0) |
| `ou::Str` | move it with the helpers; replace it with `std::string_view` | `std::string_view`: `ou`'s own README names that as the step left, since every program now links the strlen and memcmp it needs | None | |

## Steps
1. Every caller outside the shell (runtime, launch, init, userboot, fault_probe) calls `ours_*` where it called an `ou` forwarder. Mechanical, by a script that knows each forwarder's argument order and defaults. Verified by `ours-verify`. One commit.
2. The same for the shell, the largest caller (about 520 sites). Verified by `ours-verify` and the shell's checks in both logs. One commit.
3. The helpers move to `//system/lib/runtime:debug` (`<runtime/debug.hpp>`, namespace `runtime`), on `std::string_view`; `ou::Str` and `ou::command_text` go with them; `//system/lib/ou` is deleted, and the documents and skills that describe it are corrected. Verified by `ours-verify`. One commit.

## Files
- `system/lib/runtime/src/start.cpp`, `include/runtime/startup.hpp`, `system/lib/launch/src/launch.cpp`, `system/service/init/init.cpp`, `kernel/lib/userabi/userboot/userboot.cpp`, `kernel/lib/syscall/test/fault_probe/*` — step 1.
- `examples/shell/shell.cpp` — step 2.
- `system/lib/runtime/BUILD.gn`, `include/runtime/debug.hpp`, `src/debug.cpp` — new, step 3.
- `system/lib/ou/` — deleted, step 3.
- The BUILD.gn files that depend on `//system/lib/ou`, `system/BUILD.gn`'s comment, `.agents/skills/ours-kernel-design/references/layering.md`, `.agents/tasks/system-abi-surface/DESIGN.md`, `docs/design/260923/userspace-interface.md` (an English note) — step 3.

## Test plan
- `ours-verify` after every step: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped).
- After step 2, every "shell: ... checks passed" line in both logs, as before (19 groups in dbg; rel skips the same 5).
- After step 3, no `ou::` and no `system/lib/ou` left in the tree outside the change records.
