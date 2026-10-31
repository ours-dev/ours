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

## 2. About 40 percent of the kernel targets' project includes name a header no target lists, so the include-what-you-use convention cannot be applied to them

**Severity**
Low: nothing fails; the convention, and `gn check`, have nothing to hold these includes to.

**Symptom**
Latent, cannot be triggered today. Measured on the `kernel_x86.dbg.1` variant of `out/bringup.x86` (97 targets) with a throwaway script (method under item 3), 912 of the 2298 include lines that resolve to a file inside the tree name a header that no target lists in `public` or `sources`: `ours/mm/*` 352, `ours/task/*` 131, `ours/time/*` 128, `ours/lockdep.hpp` 61, `ours/cpu.hpp` 48, `ours/sync/*` 30, `ours/irq/*` 28, then `ours/ipi_service.hpp`, `ours/boot/*`, `ours/init.hpp` and a few more. On `kernel_riscv.dbg.1` it is 735 lines.

**Cause**
Two roots hold the headers. `//kernel/mm:headers` lists none of the 46 under `kernel/mm/include` (`docs/repairs/261030/build-headers.md`, item 1). The rest are under `kernel/include`, which is the include path of the kernel products and not a `cpp_interface` root, so there is no target a list could be written on (the same file, item 6). GN ignores an include whose file no target lists, so the sweep that fixed 36 of the 37 pairs of `gn check --check-system` on x86 (21 of 22 on riscv) could not see any of these, and the sweep's own count (x86 37 pairs to 1) says nothing about them.

**Change**
This depends on the two items of `build-headers.md` and is not a step of its own: list the mm headers, give `kernel/include` an owner (a design question: one target for the whole directory, or one per subsystem directory, `ours/task`, `ours/time`, `ours/sync`), and only then does the edge sweep of item 3 reach these includes. Do not add edges to `//kernel/mm:headers` or to an owner of `kernel/include` before they list their headers: the check would still read nothing.

**Reference**
None, this project's own graph.

**Acceptance**
The count above, taken again, shows the `unowned` class at 0 for the headers under `kernel/mm/include` and `kernel/include`, and `gn check --check-system` still reports 0 pairs (or only item 1's).

## 3. 91 includes on x86 and 51 on riscv are held by a chain of public dependencies and not by an edge of the includer, which the convention asks for

**Severity**
Low: nothing fails, `gn check` is satisfied; the declaration is missing, and it holds only while an unrelated target keeps a `public_deps` entry.

**Symptom**
Latent, cannot be triggered today. On `kernel_x86.dbg.1`, of the 1386 include lines of headers some target lists, 814 name an owner that is a direct dependency of the includer, 167 name an owner whose own library the includer depends on (`//kernel/commands` depends on `//kernel/lib/gktl`, which forwards `//kernel/lib/gktl:headers`, the owner of `gktl/command.hpp`), and 405 are held by a longer chain of public dependencies only. The 405 lines are 91 distinct (includer, owner) pairs in 37 includers; by owner: `//system/public:public` 25, `//system/lib/ustl:headers` 23, `//kernel/lib/arch:headers` 15, `//kernel/lib/heap:headers` 9, `//kernel/arch/x86:headers` 6, `//system/lib/logz4:headers`, `//kernel/lib/ktl:headers` and `//kernel/lib/gktl:headers` 3 each, and one each of `//kernel/platform:headers` (`kmalloc.cpp`, item 1), `//kernel/phys:handoff`, `//kernel/lib/user_copy:headers` and `//kernel/arch/x86/phys:headers`. On `kernel_riscv.dbg.1`: 51 pairs in 24 includers. Examples: `//kernel/mm:mm` includes 31 `ustl/` headers and 12 `gktl/` ones and lists neither; `//kernel/object:object` includes 18 `heap/` headers; `//kernel/commands:commands` 18 `arch/` ones; `//kernel/kernel/task:task` 8 `arch/spin_hint.hpp`-style ones.

**Cause**
`gn check` accepts an include through any chain of public dependencies, and the findings of the sweep were the includes that had no such chain, or only a private one. This item is the rest of the convention's wording ("every target that owns a header its sources include directly"): the same fault as the sweep's, but with no finding to show it. It is how riscv's `<arch/spin_hint.hpp>` in `//kernel/kernel/sync` stayed unreported for as long as `//kernel/mm:headers` reached `//kernel/lib/arch:headers` through four hops (`docs/changes/261031/build-deps.md`, the third entry), and how x86's had been reported all along.

How the numbers were taken: `gn desc out/bringup.x86 '//*(//build/switch/set/dbg.1:kernel_x86.dbg.1)' --format=json`; for each target, every `#include` of every file in its `sources` and `public` resolved against the target's own `include_dirs` to a file, the file to the targets that list it, and the owner classified against the includer's `deps`. The JSON does not say which dependencies are public, so "a chain of public dependencies" above is "not direct and not one of the pairs `gn check` reports", which is exact only because `gn check` reports one pair (item 1). The "owner's own library" class is a judgement: `3eb20421` treated `//kernel/lib/gktl` as owning `gktl/command.hpp` for the x86 commands target, and so does this count.

**Change**
This is a scope decision for the user, because most of the edges are to two roots that nearly every kernel target already reaches: `//system/public:public` and `//system/lib/ustl:headers` are 48 of the 91. Options: (a) add every one, in the same families and the same per-commit checks as the sweep (three or four commits, none changing a compile command where the includer already reaches the owner, as the heap and shell edges of the first entry did not); (b) add all but those two roots, 43 edges, and leave the two as the one allowed implicit dependency; (c) stop here, with `gn check` as the only held reading. Recommended: (a), together with a repeatable check, so that the convention is held for new includes and not only for the ones present when it was written: the throwaway script above made into `scripts/` Python (`gn desc` JSON in, the pairs out), run by `ours-verify` beside the header-list check. Not (c): without a check the convention is a sentence. Do not do this item before item 2 for the 912 unowned lines: they are the larger gap.

**Reference**
None, this project's own graph. The convention is the last bullet of `.agents/skills/ours-kernel-design/references/layering.md`.

**Acceptance**
The same count, taken again on both build directories, shows the `public` class at 0 (or at the exempt roots, if (b) is chosen), `gn check` plain and `--check-system` still pass, the compile commands and the `.text` of the x86 dbg `vmours` are unchanged, and x86 `ours-verify` and the riscv check pass.
