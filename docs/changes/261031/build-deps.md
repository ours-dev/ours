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

## Title: The C library every ring builds lists //system/public, the owner of the two headers its one source file includes (2026-10-31)

**Files**
- `system/lib/libc/BUILD.gn` — `source_set("libc")` gains `//system/public` next to `:impl` (`src/string.cpp` includes `<ours/compiler.hpp>` and `<ours/types.hpp>`).
- `docs/changes/261031/build-deps.md` — this entry.

**Symptom**
Latent, cannot be triggered today. `gn check --check-system` reports `//system/lib/libc/src/string.cpp:22` and `:23`, `Include not allowed`, owner `//system/public:public`: x86 31 pairs before this change and 29 after, riscv 16 and 14. They are the last two of the five findings against the C library, the other three being `<ours/panic.hpp>` in the kernel's glue (previous entry).

**Cause**
The same as the previous entry (the include resolves through the toolchain's include roots, so the missing edge fails nothing), with one thing particular to this library: it is the one target that every ring builds from the same source. The generated files show five instances of `libc.ninja` per architecture, one in each of the two kernel variants, the phys toolchain, the user toolchain and the default one (which builds nothing unless asked), and the edge lands in all five. In the kernel and phys instances `//system/public` is a plain extra edge. In the user instance it is a new kind of edge: a user program reaches the public headers through a toolchain config (`//system:user.headers`) and so, as the comment in `system/public/BUILD.gn` says, "has no such edge", and `user_executable()` adds one for it. The C library is not an executable, so nothing added that edge for it: it was a user of the headers with no edge in the user toolchain as well.

What the edge brings is what the previous entry listed: `:public.config`'s directory, the generated one from `:vdso_decls`, `ustl`'s include directory where the instance lacked it, and an order-only edge after `:syscalls.gen`. `string.cpp` reads none of the generated files, so the only thing that can change is the order of the build, and that only for this one object.

**Change**
```gn
 if (!is_host_toolchain) {
   source_set("libc") {
     sources = [ "src/string.cpp" ]
-    deps = [ ":impl" ]
+
+    # <ours/compiler.hpp> and <ours/types.hpp>, two of the headers of
+    # //system/public.
+    deps = [
+      ":impl",
+      "//system/public",
+    ]
```

Rejected: giving `:impl` the edge as well. `src/string_impl.cpp` and `.hpp` include nothing of `//system/public`'s, and `:impl` is built in the host toolchain for the tests, where an edge to the generator of the system-call ABI would have no reason to be.

**Reference**
None, this project's own build graph.

**Verification**
- `gn check` plain: OK on both build directories. `--check-system` distinct pairs: x86 31 to 29 (58 lines), riscv 16 to 14 (28 lines); the two gone are `string.cpp:22` and `:23` on each, none new.
- Generated files against the snapshot taken after the previous commit: five `.ninja` files differ per architecture, the five `libc.ninja`. `compile_commands.json`: 752 commands on x86 and 630 on riscv before and after, none added or dropped, four changed on each: the `string.cpp` object in both kernel variants and the user toolchain gain `-Igen/system/public/public`, and the phys one `-Igen/system/public/public -I../../system/lib/ustl/include`.
- Byte for byte, built before and after on x86: `kernel.dev.obi` `8b3b314bc01688bb`, `kernel.dev.dbg_1.obi` `81aa5ccf6aa6e6a5` and `fake.obi` `c9f47b86e7163e12` (sha256 prefixes) are identical, and so are the four `libc.string.cpp.o` objects (`b493e91f2c4d962b` in both kernel variants, `cea33b72382e5624` phys, `08e388d377d53339` user). That includes the user toolchain's object, which the previous commit's section hashes did not cover. The dbg `vmours` sections are identical too.
- riscv probe: 121 failing compiles before and after, the same objects.
- x86 `boot_check.py`: dbg PASS shell finished at 5.7s · 44 passed, 4 skipped, 0 failed; rel PASS shell finished at 4.8s · 41 passed, 8 skipped, 0 failed · 6/6 required; host-tests PASS 153 passed, 0 failed; headers PASS 8 targets, 106 headers. riscv `boot_check.py --out out/bringup.riscv`: PASS handoff ok at 0.1s; headers PASS 8 targets, 81 headers.

**Deliberately not done**
- The edge from `:impl`, see Change.
- A split of `//system/public` into headers-only and generator parts, which would take the generator edge out of every includer of its headers: a layering change to the target every ring shares, and a decision for the user, not a step of a sweep.

**How it was found**
The same parse of `gn check --check-system` as the previous entry; the five instances come from `ls` of `libc.ninja` under each toolchain directory of the snapshot.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is the user toolchain's `libc` object the one the section hashes of the kernel could not vouch for, and what did vouch for it?
2. What did `user_executable()` add for a user program that `source_set("libc")` did not have, and why does the library now have it too?
3. Why is the edge on `:libc` and not on `:impl`?
