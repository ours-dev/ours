# build-deps outstanding repairs

## 1. `kmalloc.cpp` includes `<ours/platform/init.hpp>` for a macro that `<ours/init.hpp>` defines

**Severity**
Low: nothing fails at build or run time; `gn check --check-system` reports the include on both architectures and the target has no edge that would silence it.

**Symptom**
Latent, cannot be triggered today. `gn check out/bringup.x86 --check-system` and the same on `out/bringup.riscv` report `//kernel/lib/heap/src/kmalloc.cpp:8` (`#include <ours/platform/init.hpp>`): `Include not allowed`, includer `//kernel/lib/heap:heap`, owner `//kernel/platform:headers`. It is the one pair of the include-what-you-use sweep that was left without an edge (x86 1 of its 37 pairs at the start, riscv 1 of 22).

**Cause**
The file uses nothing the platform seam declares. Its only use of the header is `INIT_CODE` on `init_kmalloc()` (line 99), and `INIT_CODE` is defined in `<ours/init.hpp>`, which `ours/platform/init.hpp` happens to include first. So the include names a header the file does not need and gets the one it does through it. An edge from `//kernel/lib/heap:heap` to `//kernel/platform:headers` would make the check pass and would be wrong in a second way: it would make the heap depend on the platform seam for a macro. `<ours/init.hpp>` lives under `kernel/include`, which no target owns (`docs/repairs/261030/build-headers.md`, item 6), so once the include is the right one the check says nothing about it, as for every other header of that directory.

**Change**
Change the include in `kernel/lib/heap/src/kmalloc.cpp` to `<ours/init.hpp>`, keep the target's deps as they are (the comment in `kernel/lib/heap/BUILD.gn` already says why there is no edge), and build both architectures: the file may rely on something else `ours/platform/init.hpp` brings in (`gktl/init_hook.hpp`, `ustl/traits/underlying.hpp`), which a compile shows at once. Ruled out: the edge to `//kernel/platform:headers`, for the reason above; leaving it, which keeps a finding in the check that nobody can tell from a real one.

**Reference**
None, this project's own graph.

**Acceptance**
`gn check --check-system` no longer reports `kmalloc.cpp` on either build directory, the heap's object file is byte-identical (`kernel_x86.dbg.1/obj/kernel/lib/heap/src/heap.kmalloc.cpp.o` before and after), and x86 `ours-verify` and the riscv check pass.
