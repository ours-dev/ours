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

## Title: The kernel's sync, task, sched and time source sets list //kernel/lib/arch:headers and //system/lib/logz4:headers, the owners of the headers their sources include (2026-10-31)

**Files**
- `kernel/kernel/sync/BUILD.gn` — `//kernel/lib/arch:headers` (`event.cpp` and `semaphore.cpp` include `<arch/auto_interrupt_disabler.hpp>`, `mutex.cpp` and `spin_lock.cpp` `<arch/spin_hint.hpp>`).
- `kernel/kernel/task/BUILD.gn` — `//system/lib/logz4:headers` (`<logz4/log.hpp>` in `cpu_hotplug.cpp`, `idle_thread.cpp`, `thread.cpp`).
- `kernel/kernel/task/sched/BUILD.gn` — `//kernel/lib/arch:headers` (`scheduler.cpp` includes `<arch/auto_interrupt_disabler.hpp>`). The target lists `fair.cpp` when the Normal discipline is fair; no `fair.*` file is touched and none was read for this.
- `kernel/kernel/time/BUILD.gn` — `//kernel/lib/arch:headers` (`timer.cpp`, `timer_queue.cpp`) and `//system/lib/logz4:headers` (`clock_event_device.cpp`, `clock_source.cpp`, `tick_broadcast.cpp`, `tick_device.cpp`, `timer_queue.cpp`).
- `docs/changes/261031/build-deps.md` — this entry.

**Symptom**
Latent, cannot be triggered today. `gn check --check-system` reported 17 pairs on x86 and 8 on riscv against these four targets: x86 sync 4, sched 1, time 9 (4 arch, 5 logz4), task 3; riscv task 3 and time 5, all of them logz4. All `Include not allowed`. After this change: x86 29 to 12 pairs (24 lines), riscv 14 to 6 (12 lines), none new.

**Cause**
The same as the first entry of this file for logz4. The arch half is a different shape worth reading: `<arch/auto_interrupt_disabler.hpp>` and `<arch/spin_hint.hpp>` belong to `//kernel/lib/arch:headers`, the generic half of the `<arch/...>` headers, and the architecture's own half sits under `kernel/arch/<cpu>` (`<arch/x86/spin_hint.hpp>`, `<arch/riscv/spin_hint.hpp>`). A kernel source set that includes the generic one is a user of the architecture layer and should say so.

x86 reports these pairs and riscv does not, with the same sources and the same include lines, and the reason is not that riscv's includes are right: riscv's are held by an accident of its graph. `gn path` from `//kernel/mm:headers` to `//kernel/lib/arch:headers` finds, in the riscv build directory, `//kernel/mm:headers --[public]--> //kernel/phys:handoff --[public]--> //kernel/arch/riscv/phys:headers --[public]--> //kernel/arch/riscv:headers --[public]--> //kernel/lib/arch:headers`; in the x86 directory it finds no path. These targets depend on `//kernel/mm:headers`, so on riscv they reach the owner through four `public_deps` hops that nobody wrote for them, and GN is satisfied. That is the stricter reading of the convention showing: a target can pass `gn check` for a header it has no edge for, until an unrelated target drops a `public_deps` entry. Both architectures now have the direct edge, which is what the header lists in the file say.

**Change**
One line plus a comment in each list, appended to the end:

```gn
# kernel/kernel/sync/BUILD.gn, and sched/BUILD.gn with the one file named
    "//system/lib/lockdep",
+
+   # <arch/auto_interrupt_disabler.hpp> (event.cpp, semaphore.cpp) and
+   # <arch/spin_hint.hpp> (mutex.cpp, spin_lock.cpp), the generic half of the
+   # architecture's interface.
+   "//kernel/lib/arch:headers",

# kernel/kernel/task/BUILD.gn
+   # <logz4/log.hpp> (cpu_hotplug.cpp, idle_thread.cpp, thread.cpp).
+   "//system/lib/logz4:headers",

# kernel/kernel/time/BUILD.gn
+   "//kernel/lib/arch:headers",
+   "//system/lib/logz4:headers",
```

Rejected: putting `//kernel/lib/arch:headers` in `public_deps` of these source sets, which would pass it on to every dependent of `//kernel/kernel/sync` and the others; the includes are in `.cpp` files, so `deps` is the right strength.

**Reference**
None, this project's own build graph.

**Verification**
- `gn check` plain: OK on both build directories. `--check-system` distinct pairs, sets compared: x86 29 to 12, riscv 14 to 6; 17 and 8 gone, none new.
- Generated files against the snapshot after the previous commit: eight `.ninja` files differ per architecture (`sync`, `task`, `sched` and `time` in both kernel variants), each only in one `linkdeps` line that gains `phony/kernel/lib/arch/headers.linkdeps` or `phony/system/lib/logz4/headers.linkdeps` (in `task.ninja` the arch token, already there through another dependency, also changes place within the line). `compile_commands.json` is unchanged as a set: 752 commands on x86 and 630 on riscv, none differ.
- x86 `kernel.dev.obi` `8b3b314bc01688bb`, `kernel.dev.dbg_1.obi` `81aa5ccf6aa6e6a5` and `fake.obi` `c9f47b86e7163e12` (sha256 prefixes) byte-identical before and after, and the dbg `vmours` sections too (`.text` `6250b0ceb2af19c1`, `.rodata` `249063baa8a2be5c`, `.data` `e88a26c4c147e9ca`). riscv `fake.obi` `5636a21138f1b7a1` and `bootshim.bin` `f6be1306ccb3df88` byte-identical.
- riscv probe: 121 failing compiles before and after, the same objects.
- x86 `boot_check.py`: dbg PASS shell finished at 5.7s · 44 passed, 4 skipped, 0 failed; rel PASS shell finished at 4.7s · 41 passed, 8 skipped, 0 failed · 6/6 required; host-tests PASS 153 passed, 0 failed; headers PASS 8 targets, 106 headers. riscv `boot_check.py --out out/bringup.riscv`: PASS handoff ok at 0.1s; headers PASS 8 targets, 81 headers.

**Deliberately not done**
- Any edit of `fair.*`, which belongs to the user. `sched/BUILD.gn` is a dependency list and not part of the discipline.
- Edges for the other headers these sources include that the check already accepts through some chain (`<ours/...>` headers of the kernel's own include root are unowned, and the mm ones are unlisted, so nothing can be said about them).

**How it was found**
The parse of `gn check --check-system` as in the first entry; the riscv/x86 difference from `gn path out/bringup.riscv "//kernel/mm:headers(//build/switch/set/dbg.1:kernel_riscv.dbg.1)" "//kernel/lib/arch:headers(...)"` and the same on x86.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why did riscv's `gn check` stay silent about `spin_hint.hpp` in `//kernel/kernel/sync` while x86's did not, with the same source and the same include?
2. Which of the four hops of that riscv chain could someone remove for an unrelated reason, and what would then happen to the check?
3. Why is `deps` and not `public_deps` the right strength for these edges?

## Title: //kernel/object:object lists //system/lib/logz4:headers directly, because the chain through the sampler is private and GN does not count it (2026-10-31)

**Files**
- `kernel/object/BUILD.gn` — `source_set("object")` gains `//system/lib/logz4:headers` at the end of its `deps` (`handle.cpp`, `pager_proxy.cpp`, `port_dispatcher.cpp`, `process_dispatcher.cpp` and `thread_dispatcher.cpp` include `<logz4/log.hpp>`), with a comment that says why the sampler's dependency on it is not enough.
- `docs/changes/261031/build-deps.md` — this entry.

**Symptom**
Latent, cannot be triggered today. `gn check --check-system` reports five pairs on each architecture against this target (x86 12 pairs before this change and 7 after, riscv 6 and 1), and these are the only five of the sweep that read `Can't include this header from here.` and not `Include not allowed.`:

```text
The target:
  //kernel/object:object
is including a file from the target:
  //system/lib/logz4:headers

It's usually best to depend directly on the destination target.
In some cases, the destination target is considered a subcomponent
of an intermediate target. In this case, the intermediate target
should depend publicly on the destination to forward the ability
to include headers.

Dependency chain (there may also be others):
  //kernel/object:object -->
  //kernel/lib/sampler:sampler --[private]-->
  //system/lib/logz4:headers
```

**Cause**
This is the other half of the rule the earlier entries only touched. GN lets a target include a header of any target it depends on directly, and of any target reached from those through `public_deps` hops. A `deps` hop (private) ends the reach: what `//kernel/lib/sampler` links against is its own business, and whoever depends on the sampler does not thereby depend on logz4. The two messages say which of the two situations it is: `Include not allowed` means no chain exists, and `Can't include this header from here` means a chain exists and has a private hop in it, which is what happened here. `//kernel/object:object` has a path to logz4's headers through the sampler, and the path is no good for the include.

There were two ways to make it good. The message offers one: make the sampler forward the header (`public_deps`), which would hand logz4's headers to everyone who depends on the sampler on the grounds that this one target needs them. The other is the edge here. `object` includes the header in five of its own `.cpp` files and does not include anything of the sampler's through it, so the dependency is `object`'s own and belongs in `object`'s list. This also keeps the include from depending on the sampler's choice of strength: had the sampler's edge been moved from private to public some day for another reason, the check would have passed with no edge in `object`, and the day it was moved back the check would have gone red in a target nobody touched.

**Change**
```gn
     deps = [
       # (the comment on the first entry is unchanged)
       "//kernel/arch/$ours_cpu/kernel:headers",
       "//kernel/lib/sampler",
       "//kernel/platform:headers",
+
+      # <logz4/log.hpp> (handle.cpp, pager_proxy.cpp, port_dispatcher.cpp,
+      # process_dispatcher.cpp, thread_dispatcher.cpp). //kernel/lib/sampler
+      # depends on it, but privately, so that path does not count: GN reads an
+      # include through a chain of public dependencies only.
+      "//system/lib/logz4:headers",
     ]
```

Rejected: the sampler's `deps` entry on `//system/lib/logz4:headers` moved to `public_deps`, as above.

**Reference**
None, this project's own build graph. The rule is in `gn help check`, on which dependencies an include may be read through.

**Verification**
- `gn check` plain: OK on both build directories. `--check-system` distinct pairs, sets compared: x86 12 to 7 (14 lines), riscv 6 to 1 (2 lines); five gone on each, none new. The one pair left on riscv is `kmalloc.cpp:8`, repair item 1.
- Generated files against the snapshot after the previous commit: two `.ninja` files differ per architecture, `object.ninja` in both kernel variants, in one `linkdeps` line. `compile_commands.json` unchanged as a set (752 commands on x86, 630 on riscv).
- x86 `kernel.dev.obi` `8b3b314bc01688bb`, `kernel.dev.dbg_1.obi` `81aa5ccf6aa6e6a5` and `fake.obi` `c9f47b86e7163e12` byte-identical before and after, the dbg `vmours` sections too (`.text` `6250b0ceb2af19c1`); riscv `fake.obi` `5636a21138f1b7a1` and `bootshim.bin` `f6be1306ccb3df88` byte-identical.
- riscv probe: 121 failing compiles before and after, the same objects.
- x86 `boot_check.py`: dbg PASS shell finished at 5.7s · 44 passed, 4 skipped, 0 failed; rel PASS shell finished at 4.8s · 41 passed, 8 skipped, 0 failed · 6/6 required; host-tests PASS 153 passed, 0 failed; headers PASS 8 targets, 106 headers. riscv `boot_check.py --out out/bringup.riscv`: PASS handoff ok at 0.1s; headers PASS 8 targets, 81 headers.

**Deliberately not done**
- Moving the sampler's edge to `public_deps`, see Change.
- Auditing the sampler's own includes of logz4; its edge is private on purpose and its own sources are held to it by the same check.

**How it was found**
The parse of `gn check --check-system` as in the first entry; the two kinds of message are told apart by the first line of each block, and the chain of the `Can't include` ones is printed in the block itself.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. What is the difference between `Include not allowed` and `Can't include this header from here`, and which one means a path to the header exists?
2. Why is it better for `object` to depend on logz4's headers itself than for the sampler to pass them on?
3. How could the check have passed for `object` with no edge of its own, and what would have made it stop?

## Title: x86's kernel target lists //kernel/platform:headers and the i8253 driver lists //kernel/arch/x86:headers, the last two families of the include-what-you-use sweep (2026-10-31)

**Files**
- `kernel/arch/x86/kernel/BUILD.gn` — `source_set("kernel")` gains `//kernel/platform:headers` at the end of its `deps` (`bootstrap.cpp` and `faults.cpp` include `<ours/platform/init.hpp>`, `faults.cpp` `<ours/platform/interrupt.hpp>`, `time/hpet.cpp` and `time/time.cpp` `<ours/platform/isa.hpp>`).
- `kernel/driver/BUILD.gn` — `source_set("driver")` gains `//kernel/arch/x86:headers` (`clock/i8253.cpp` includes `<arch/x86/io.hpp>`), with a comment that the one target that links the driver is x86's kernel target.
- `docs/changes/261031/build-deps.md` — this entry.

**Symptom**
Latent, cannot be triggered today. `gn check out/bringup.x86 --check-system` reported six pairs against these two targets: five `Can't include this header from here` for x86's kernel target (the chain it printed was `//kernel/arch/x86/kernel:kernel --> //kernel/arch/x86/commands:commands --[private]--> //kernel/platform:headers`) and one `Include not allowed` for `//kernel/driver:driver`. After this change x86 has 7 pairs less 6, one left (`kmalloc.cpp:8`, repair item 1), and riscv, whose graph holds neither target, is unchanged at 1.

Over the whole sweep (this file, five entries): x86 37 pairs at HEAD `c5ba24d8` and 1 now, riscv 22 and 1; 36 and 21 gone, none new.

**Cause**
For x86's kernel target: it includes the platform seam in four files and had no edge for it. The only way GN found to the seam was through `//kernel/arch/x86/commands`, which is a private edge and so not one an include may use. That edge exists for the console tests, not for these files, so removing the shell (`kernel_shell_enabled = false` leaves the commands out of the link) takes away the path that happened to exist. Measured, on a throwaway shell-less configuration with the edge taken out again: the same five pairs read `Include not allowed`, no chain of any kind. The include is x86's own: its code calls the board through `ours/platform/`, the seam `kernel/platform` publishes for a generic caller to reach a board without naming it.

For the driver: `clock/i8253.cpp` reads and writes the PIT's ports through `<arch/x86/io.hpp>`. It is generic in name only: nothing but x86's kernel target links `//kernel/driver` (`git grep` of the build files finds `kernel/arch/x86/kernel/BUILD.gn` and nothing else), so the architecture-private include is only ever compiled for x86 and an edge to `//kernel/arch/x86:headers` names x86 in a directory without the architecture in its path where no other architecture builds it. The edge is not what makes the file x86-only; the link is. That is the same rule as for `kernel/arch/x86/commands` (`layering.md`: a feature one architecture has is kept out of the others by what GN links), and the comment in the list says so, so that a second architecture that links `//kernel/driver` finds a reason and not a mystery.

**Change**
```gn
# kernel/arch/x86/kernel/BUILD.gn, source_set("kernel")
     "//kernel/lib/mtl:headers",
     "//kernel/mm:headers",
+
+    # <ours/platform/init.hpp> (bootstrap.cpp, faults.cpp),
+    # <ours/platform/interrupt.hpp> (faults.cpp) and <ours/platform/isa.hpp>
+    # (time/hpet.cpp, time/time.cpp): the seam x86's kernel code calls the
+    # board through. Before this edge only private ones reached it, such as
+    # the commands below, and GN does not read an include through those.
+    "//kernel/platform:headers",
   ]

# kernel/driver/BUILD.gn, source_set("driver")
   deps = [
+    # <arch/x86/io.hpp> (clock/i8253.cpp): the i8253 is a PC device, and the
+    # one target that links this one is x86's kernel target, so no other
+    # architecture is ever asked for x86's port I/O.
+    "//kernel/arch/x86:headers",
     "//kernel/lib/gktl:headers",
```

Rejected:
- `//kernel/arch/$ours_cpu:headers` for the driver: it would resolve to `//kernel/arch/riscv:headers` on riscv, which has no `arch/x86/io.hpp`, and say that the file is architecture-neutral when it is not.
- Moving `clock/i8253.cpp` under `kernel/arch/x86` or `kernel/platform/pc`: whether a PC device's driver belongs in `kernel/driver` is a placement question (`layering.md`, "Architecture, platform and drivers"), not one a dependency sweep answers; it is not done here.

**Reference**
None, this project's own build graph.

**Verification**
- `gn check` plain: OK on both build directories, and on a throwaway shell-less configuration (`tools/set.py bringup.x86 --args kernel_shell_enabled=false`, configured and checked only, not built, and removed): plain OK, `--check-system` 1 pair, `kmalloc.cpp:8`. `--check-system` distinct pairs, sets compared: x86 7 to 1 (2 lines), riscv 1 to 1 (2 lines); six gone on x86, none new.
- Generated files against the snapshot after the previous commit: six `.ninja` files differ on x86 and none on riscv. `kernel.ninja` in both kernel variants and the two `vmours` link-step files: one `linkdeps` token (`phony/kernel/platform/headers.linkdeps`) changes place in a line; `driver.ninja` in both variants: its `linkdeps` line gains `phony/kernel/arch/x86/headers.linkdeps` and `phony/kernel/lib/arch/headers.linkdeps`, the latter through the x86 headers' own public deps. `compile_commands.json` unchanged as a set (752 commands on x86, 630 on riscv).
- x86 `kernel.dev.obi` `8b3b314bc01688bb`, `kernel.dev.dbg_1.obi` `81aa5ccf6aa6e6a5` and `fake.obi` `c9f47b86e7163e12` byte-identical before and after, and the dbg `vmours` sections too (`.text` `6250b0ceb2af19c1`, `.rodata` `249063baa8a2be5c`, `.data` `e88a26c4c147e9ca`; `vmours.with-tests` `.text` `c305fc60badab6fb`). riscv `fake.obi` `5636a21138f1b7a1` and `bootshim.bin` `f6be1306ccb3df88` byte-identical.
- riscv probe: 121 failing compiles before and after, the same objects.
- x86 `boot_check.py`: dbg PASS shell finished at 6.0s · 44 passed, 4 skipped, 0 failed; rel PASS shell finished at 4.9s · 41 passed, 8 skipped, 0 failed · 6/6 required; host-tests PASS 153 passed, 0 failed; headers PASS 8 targets, 106 headers. riscv `boot_check.py --out out/bringup.riscv`: PASS handoff ok at 0.1s; headers PASS 8 targets, 81 headers.

**Deliberately not done**
- The placement of the i8253 driver, see Rejected.
- Anything for the one pair left on both architectures, `kmalloc.cpp:8`: the include is wrong, repair item 1.
- Edges where GN is already satisfied through a public chain. Entry three found one (riscv's `<arch/...>` reached through four hops); how many such includes remain is a measurement the next entry's repair item records, not a part of this commit.

**How it was found**
The parse of `gn check --check-system` as in the first entry; the one-line `git grep -n "kernel/driver" -- '*.gn' '*.gni'` showed who links the driver.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why did x86's kernel target get `Can't include this header from here` for the platform seam in the default build, and `Include not allowed` in a shell-less one, with no edge of its own?
2. Why does the driver's edge name x86 although `kernel/driver` is a generic directory, and what is it that keeps the file out of riscv's build?
3. Why is `//kernel/arch/$ours_cpu:headers` the wrong edge for `i8253.cpp`, when it is the right one for the memory tests?
