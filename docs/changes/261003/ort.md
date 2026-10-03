# ort changes

## Title: The runtime library is ort, the ours runtime (2026-10-03)

**Files**
- `system/lib/runtime/` → `system/lib/ort/`: the directory, `include/runtime/` → `include/ort/`, `test/runtime_smoke.cpp` → `test/ort_smoke.cpp`.
- Inside it: namespace `runtime` → `ort`, header guards `RUNTIME_*` → `ORT_*`, the C symbols `runtime_start` and `runtime_call_main` (`crt0.S`, `start.cpp`) → `ort_start` and `ort_call_main`, the source set and the smoke test renamed to match, the `.clang-format` group for its own headers, the README, and the log prefix of a failed start, `runtime:` → `ort:`.
- Its users: `system/service/init/`, `examples/shell/`, `kernel/lib/userabi/userboot/`, `kernel/lib/syscall/test/fault_probe/` (includes, `ort::` calls, dependencies), `system/lib/launch/BUILD.gn`, `system/public/ours/process_args.hpp`, `kernel/lib/userabi/src/userboot.cpp` (comments), and the root `BUILD.gn` (the tests).
- `.agents/skills/ours-kernel-design/references/layering.md` — `ort::debug_line`.

**Symptom**
The library every ring-3 program runs on was called `runtime`, as a directory, a header prefix (`<runtime/...>`) and a C++ namespace. As a namespace, `runtime` is a word any other library could want too, and it says nothing about whose runtime it is.

**Cause**
The objects:
- The library: `_start` (`crt0.S`), the processargs decoder, the startup-handle table, the call into `main`, and since the removal of `ou` the print helpers.
- Its names: the GN label, the include prefix, the namespace and the C symbols `crt0.S` and `start.cpp` share.

The events:
1. It was named for what it is, when it was the only runtime and there was nothing to tell it apart from.
2. The user asked for a short project-specific name, as Fuchsia's runtime pieces carry `zxr` (the Zircon runtime): `ort`, the ours runtime.

**Change**
A rename and nothing else: every name above, by fixed rules, and the files moved with `git mv` so their history follows. The prose "the runtime", which names what the library is, stays.

```cpp
// before
#include <runtime/debug.hpp>
static_cast<void>(runtime::debug_line("init: spawned shell"));
// after
#include <ort/debug.hpp>
static_cast<void>(ort::debug_line("init: spawned shell"));
```

**Reference**
Fuchsia names its runtime pieces after the kernel (`zxr_thread`, `zxr_mutex`, the Zircon runtime). The vDSO, by contrast, is `libzircon.so` with `zx_` symbols, and keeps its names here too (`libours.so`, `ours_*`).

**Verification**
- `ninja -k 0` on `out/vmo-snapshot`: no error. The smoke program, every ring-3 program and the host tests rebuilt from `system/lib/ort`.
- The host processargs test, built from the new path: 16 tests passed.
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped); the shell's groups as before (19 passed in dbg; 13 passed and 5 skipped in rel).

**Deliberately not done**
- The change records, repairs and design documents written before this keep the name `runtime`: they describe the tree as it was.
- The user also asked for the vDSO's declarations to be in the `ours` namespace. That has a linkage question to settle first, so it is its own change.

**How it was found**
The user's naming convention, 2026-10-03: `ort` for the runtime.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why did the C symbols in `crt0.S` and `start.cpp` have to change together?
- What would `git mv` keep that deleting and re-adding the files would lose?
- Why is `runtime` a poorer C++ namespace than `ort`, though it describes the library better?
