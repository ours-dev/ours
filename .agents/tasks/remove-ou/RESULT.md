# remove-ou: ring 3 calls the vDSO's ours_* directly, and ou is gone

**Date**: 2026-10-03

## Delivered
- Step 1, 771991de: the runtime, the launcher, init, userboot and fault_probe call `ours_*` at the 41 places they called an `ou` forwarder.
- Step 2, ba1a6583: the shell does the same at 427 places.
- Step 3, this commit: the print helpers live in `//system/lib/runtime` (`<runtime/debug.hpp>`) on `std::string_view`; `ou::Str` and `ou::command_text` are gone; `//system/lib/ou` is deleted; the code comments, the layering skill, the system-abi-surface design and two other task documents say so.

## Verification
- `ninja -k 0` on `out/vmo-snapshot` after each step: no error.
- `boot_check.py --out out/vmo-snapshot` after each step: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped).
- The shell's own check groups after steps 2 and 3: 19 passed in dbg; 13 passed and 5 skipped in rel, as before the change.
- init is built but not started by either image (userboot starts the shell), so its rewritten calls are checked by the compiler only. Not run.

## Deviations from the design
- The helpers went into the runtime source set itself, not a target of their own: the reason for a separate target (userboot without the runtime) turned out to be false.

## Found along the way
- The rewriting script took a C++ digit separator (`0xffff'0000`) for a character literal, and silently skipped 68 calls in the shell. They still compiled while `ou` existed and surfaced only when its headers went; fixed and rechecked in every file.
- `//system/lib/runtime/BUILD.gn` still said userboot defines its own `_start`; it has run on the runtime for a while. Corrected.

## Not done
- The status codes and the other open questions of `.agents/tasks/system-abi-surface/DESIGN.md` are untouched.
