# build-deps changes

## Title: The heap, the shell and the kernel's libc glue list the owner of each header gn check reads in their sources, the first of the include-what-you-use sweep (2026-10-31)

**Files**
- `kernel/lib/heap/BUILD.gn` — `//kernel/lib/heap:heap` gains `//system/lib/logz4:headers` (`vmalloc.cpp` includes `<logz4/log.hpp>`); a comment says why `kmalloc.cpp`'s `<ours/platform/init.hpp>` has no edge.
- `kernel/lib/kshell/BUILD.gn` — `//kernel/lib/kshell:kshell` gains `//system/lib/logz4:headers` (`shell.cpp` includes `<logz4/domain.hpp>` and `<logz4/log.hpp>`).
- `kernel/lib/libc/BUILD.gn` — `:libc` and `:format-glue` gain `//system/public` (`<ours/panic.hpp>`, in three files).
- `docs/repairs/261031/build-deps.md` — new, item 1: the include of `kmalloc.cpp` that is the wrong thing to fix with an edge.
- `docs/changes/261031/build-deps.md` — this file.

**Symptom**
Latent, cannot be triggered today: every one of these targets compiles and links. The symptom is in `gn check --check-system`, which is the only reader of a header's owner. At the start of the sweep (HEAD `c5ba24d8`) it reports, as distinct (file, line, include) pairs, 37 on `out/bringup.x86` and 22 on `out/bringup.riscv`; GN prints each finding once for each of the two kernel variants (`kernel_x86.dbg.0` and `.dbg.1`), so the 77 and 47 lines that the same runs print are about twice that. This entry removes six pairs on each architecture: `vmalloc.cpp:6` (heap), `shell.cpp:15` and `:16` (kshell), `cpp_dependencies.cpp:1` and `libcxx.cpp:1` (`:libc`), `libcxx_format_glue.cpp:14` (`:format-glue`). All six read `Include not allowed`: the header's owner is in no dependency of the target at all.

**Cause**
GN holds an `#include <...>` to the dependency graph in two steps. First it finds which target lists the header in `public` or `sources` (the owner; a header nobody lists is ignored, which is why `docs/repairs/261030/build-headers.md` exists). Then it requires the includer to depend on the owner, directly or through a chain of `public_deps`. Neither step has anything to do with whether the compile works: the kernel toolchains put every include root on the command line already (the "prefix configs"), so `<logz4/log.hpp>` resolves for a target with no edge to logz4, and an edge to a `:headers` target adds no code to link. That is how all of these were written without anyone noticing: the file builds, and the missing edge only means the dependency of the target is declared nowhere and held by nothing.

An edge costs little when the owner is a pure header target, as `//system/lib/logz4:headers` is, and costs nothing visible in the compile commands when the includer already reaches what the owner publishes: logz4's own public deps are `ustl:headers` and `//system/public`, and the heap and the shell both depend on those already. It is not free when the includer reaches nothing of the kind, as `//kernel/lib/libc:libc` does not, and the owner is `//system/public`, whose header `<ours/panic.hpp>` they include. `:public` is a `source_set` that carries two things besides its headers: the `:public.config` include directory, and `:vdso_decls`, a group whose public dependency is the generator of the system-call ABI (`:syscalls.gen`, run once in the default toolchain). A dependent therefore gets an order-only edge after the generator and the generated include directory, whether or not it reads either. For the four kernel objects of `//kernel/lib/libc:libc` that is one more `-I` and the edge; for `libcxx_format_glue.cpp`, compiled in the phys toolchain, it is also `ustl`'s include directory, which `:public` publishes. None of it changes what the compiler produces for these files (see Verification), but it is a real change to the build graph of a phys-stage target, and the reason this entry names it instead of calling the edge free.

`kmalloc.cpp:8` (`<ours/platform/init.hpp>`) is the seventh finding in these three libraries and is deliberately not here: the file needs `INIT_CODE`, which `<ours/init.hpp>` defines, so the edge the check asks for would document a dependency of the heap on the platform seam for a macro. The include is what is wrong; it is item 1 of `docs/repairs/261031/build-deps.md`.

**Change**
Three `BUILD.gn` files, each list kept in the order it was written, new entries last:

```gn
# kernel/lib/heap/BUILD.gn, source_set("heap")
-  deps = [ "//kernel/mm:headers" ]  # the real thing, for the implementation
+  deps = [
+    "//kernel/mm:headers",  # the real thing, for the implementation
+
+    # vmalloc.cpp logs through <logz4/log.hpp>.
+    "//system/lib/logz4:headers",
+  ]

# kernel/lib/kshell/BUILD.gn, source_set("kshell")
-  deps = [ "//kernel/phys:handoff" ]
+  deps = [
+    "//kernel/phys:handoff",
+    "//system/lib/logz4:headers",
+  ]

# kernel/lib/libc/BUILD.gn, source_set("libc") and source_set("format-glue")
-  deps = [ "//system/lib/libc" ]
+  deps = [
+    "//system/lib/libc",
+    "//system/public",
+  ]
```

Why the sweep goes by directory family and not by header: the findings of one target share one cause and one review (the includer reads one `BUILD.gn`), and a revert of a family is one `git revert`. This is the first of five families (kernel/lib, `//system/lib/libc`, kernel/kernel, kernel/object, x86's kernel target and driver).

Rejected:
- An edge from `//kernel/lib/heap:heap` to `//kernel/platform:headers`: see Cause, and repair item 1.
- `public_deps` instead of `deps` for the new edges: it would hand logz4 or the public headers to every dependent of the heap and the shell, which hides which includer needs them; the includes are in `.cpp` files, so `deps` is the right strength.
- Splitting `//system/public` so that `ours/panic.hpp` has an owner with no generator edge: a layering change to a target every ring uses, not part of a sweep of edges.

**Reference**
None, this project's own build graph. GN's rule for what an include may reach (direct dependency, or a chain of `public_deps`) is `gn help check`; the repo's statement of the convention is the last bullet of `.agents/skills/ours-kernel-design/references/layering.md`.

**Verification**
- `gn check` plain: `Header dependency check OK` on `out/bringup.x86` and on `out/bringup.riscv`.
- `gn check --check-system --error-limit=1000000`, parsed into distinct (file, line, include) pairs: x86 77 lines and 37 pairs before, 66 and 31 after; riscv 47 and 22 before, 36 and 16 after. Six pairs gone on each, none new, none changed (the sets were compared, not the counts).
- Generated files, diffed against a snapshot of the build directory at HEAD: nine `.ninja` files differ per architecture. Four are one `linkdeps` line that gains `phony/system/lib/logz4/headers.linkdeps` (`heap.ninja` and `kshell.ninja`, each in both kernel variants). Two are `object.ninja`, whose `linkdeps` line already had that token and now lists it earlier (the order of a phony's inputs, nothing added or dropped). Three carry compile commands: `libc.ninja` in both variants (`-Igen/system/public/public` appended to the two objects' commands, and `|| phony/system/public/syscalls.gen` on them) and phys `format-glue.ninja` (`-Igen/system/public/public -I../../system/lib/ustl/include` appended, the same order-only edge). `compile_commands.json`, compared as a set because its order is not stable: 752 commands on x86 and 630 on riscv, none added or dropped, five changed on each exactly as listed (four kernel `libc` objects, one phys glue object).
- x86 dbg `vmours` (`kernel_x86.dbg.1`), built after the change, against the one built at HEAD: sha256 prefixes of `.text` `6250b0ceb2af19c1`, `.rodata` `249063baa8a2be5c` and `.data` `e88a26c4c147e9ca`, identical; `vmours.with-tests` (`kernel_x86.dbg.0`) `.text` `c305fc60badab6fb`, `.rodata` `e53cad978d70c732`, `.data` `d9dbba464e2d51d1`, identical. The compile commands that changed gained only include directories, and the output did not move.
- riscv probe, `ninja -k 0` on `kernel_riscv.dbg.1/obj/kernel/products/bin/vmours`: 121 failing compiles at HEAD and 121 after, the same objects (the sorted lists are equal).
- x86 `boot_check.py`: build ok; dbg PASS shell finished at 5.7s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.8s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 153 passed, 0 failed; headers PASS 8 targets, 8 include roots, 106 headers under //kernel/arch/ and //kernel/lib/arch:, every one listed.
- riscv `boot_check.py --out out/bringup.riscv`: riscv PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi; headers PASS 8 targets, 8 include roots, 81 headers under //kernel/arch/ and //kernel/lib/arch:, every one listed; exit code 0.
- `format_gn.py --check` on the three files: already formatted.

**Deliberately not done**
- `kmalloc.cpp`'s include (repair item 1), because it is a source change and the right fix is not an edge.
- The includes of headers that no target lists (everything under `kernel/include` and `//kernel/mm`'s 46 headers): the check cannot read them whatever the targets list, and `docs/repairs/261030/build-headers.md` is where they are counted.
- Edges for includes the check already accepts because some `public_deps` chain reaches the owner. They satisfy GN and are not part of this sweep's findings; see the last item of `docs/repairs/261031/build-deps.md` once it exists.

**How it was found**
`gn check out/bringup.x86 --check-system --error-limit=1000000 > before.txt` (the default limit stops at ten errors), then a Python parser over the text: split at the `___________________` separators, read `ERROR at //file:line:col: <kind>`, the include text, the includer under `It is not in any dependency of`, and the owners under `The include file is in the target(s)`. Grouping the 37 pairs by (includer, owner) gives eleven targets and six owners, which is what made the sweep a matter of a few `BUILD.gn` files and not of 37 edits.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. The compile of `vmalloc.cpp` succeeded before this change with no edge to logz4. What made the include resolve, and what then was the edge for?
2. Why do the new edges change the compile commands of `//kernel/lib/libc` and not those of the heap or the shell, although logz4's headers publish `//system/public` as well?
3. Why is the one remaining finding in the heap, `kmalloc.cpp:8`, left without an edge, and what would adding it have said about the heap's layer?
