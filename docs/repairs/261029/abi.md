# abi outstanding repairs

## 1. Which copy of `ours::to_string(Status)` a program keeps decides whether it costs 35 dynamic relocations

**Severity**
Low: a size cost, not a fault. In `init` it is 1,120 bytes of the file and 35 relocations the loader applies at start-up.

**Symptom**
Found while measuring `docs/changes/261029/loop.md` (2026-10-30). Between two builds of `init` that differ in no way that needs it, `.data.rel.ro` grew by 280 bytes and `.rela.dyn` by 840: a new object with no symbol, 35 pointers, each with an `R_X86_64_RELATIVE` relocation (the count of those in `init` went from 214 to 255, of which 6 are the new vtable slots of the same change). The object is the lookup table of `ours::to_string(Status)`: its baseline copy is a table of 35 32-bit offsets in `.rodata` (140 bytes, no relocation), and its copy in the new build is a table of 35 absolute addresses in `.data.rel.ro` (280 bytes, 35 relocations). `llvm-objdump -d -C` of the function shows `movslq (%rcx,%rax,4)` against `movq (%rcx,%rax,8)`. `echo_server` still has the first form.

**Cause**
`to_string` is an `inline` function in a public header (`system/public/ours/status.hpp:140`), a `switch` returning a string literal for each of 35 statuses. Every translation unit that calls it emits its own weak copy and the linker keeps the first it meets. In `init`, three objects carry a copy (`llvm-nm` on each): `ort/start.cpp.o`, `init/launcher_server.cpp.o` and `init/realm.cpp.o`. At the baseline `start.cpp.o` and `launcher_server.cpp.o` had compiled it to the table of offsets and `realm.cpp.o` to the table of addresses, and `init` kept `launcher_server.cpp.o`'s, which precedes the other two in its link line (`ninja -t query` on the program lists `launcher_server.cpp.o`, then `realm.cpp.o`, and `ort.start.cpp.o` much later). After the change that added calls to `ours::to_string` in `launcher_server.cpp`, that object's copy is the table of addresses, and `init` kept it. So the same source, with the same flags, compiles to either table depending on the translation unit, and which one a program gets depends on link order. Why the compiler chooses differently per unit I did not find; the lookup-table lowering and its conversion to offsets are done per module, and a module that uses some of the literals another way may not qualify.

**Change**
Not decided; the user's. The options: (a) define `to_string` out of line, in one library every program links, so that there is one copy compiled one way; the cost is a dependency of the public header's users on that library, and a call where the inline function was a table lookup; (b) write the table by hand as a `constexpr` array of offsets or of a fixed-size name, so that no pointer needs a relocation whichever unit compiles it; the cost is a second list of the statuses to keep in step with the enum (a `static_assert` on its length would catch the drift); (c) leave it, and accept 1,120 bytes of `init` and 840 of dynamic relocation work at its start.

**Reference**
None, this project's own design.

**Acceptance**
`llvm-readelf -r out/<dir>/user-x86/obj/system/service/init/init | grep -c R_X86_64_RELATIVE` counts 220 (214 at `a8489d18` and the 6 vtable slots of the change that found this), or the choice to leave it is written in the change record; and `llvm-nm -S -C` of `ours::to_string(ours::Status)` in `init` and in `echo_server` shows the same form.
