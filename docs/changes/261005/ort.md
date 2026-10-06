# ort changes

## Every program gets the runtime, and its namespace is bound before main (2026-10-05)

**Files**
- `system/user_executable.gni` — `//system/lib/ort` is a default dependency, beside libc
- the ten BUILD files that named `//system/lib/ort` themselves — no longer do (`examples/echo`, `examples/profile`, `examples/shell`, `kernel/lib/syscall/test/fault_probe`, `kernel/lib/userabi/userboot`, `system/driver/pc-ps2`, `system/lib/component/test/fixtures`, `system/lib/ort/test`, `system/service/init`, `system/service/init/test/routing_checks`)
- `system/lib/ort/src/start.cpp`, `system/lib/ort/BUILD.gn` — `ort_start()` calls `ort_extensions_init()` before main; ort defines it empty and weak
- `system/lib/fsio/src/fsio.cpp`, `system/lib/fsio/include/fsio/fsio.hpp`, `system/lib/fsio/README.md` — fsio defines `ort_extensions_init()` as `install()`, and `install()` binds only once and answers the first result every time
- `examples/shell/shell.cpp`, `examples/shell/BUILD.gn` — comments say who binds the namespace
- `.agents/skills/ours-kernel-design/references/boot.md` — the decision

**Symptom**
To write a user program, the author had to name `//system/lib/ort` in its BUILD file, although no program can run without it (it is `_start` and main's caller), and had to call `fsio::install()` at the top of main before any file could be opened.

**Cause**
ort was kept out of `user_executable()`'s defaults while programs moved onto it one at a time; the move finished and the default never followed. Binding the namespace was left to main because the runtime cannot call fsio: fsio is built on the runtime.

**Change**
ort joins libc as a default dependency, for the reason the template already gives libc: no program chooses to need it.

The runtime runs a hook between reading the processargs message and calling main, and fsio supplies it:

```cpp
// ort, start.cpp
extern "C" WEAK_LINK auto ort_extensions_init() -> void {}
...
ort_extensions_init();   // then main

// fsio, fsio.cpp
extern "C" auto ort_extensions_init() -> void {
    if (auto const status = install(); status != Status::Ok) { /* says so */ }
}
```

A weak definition, not a weak reference. The first version declared the hook weak and called it if non-null, as Fuchsia's libc does. That left every program without fsio (profile, userboot) with `ort_extensions_init` as an undefined weak dynamic symbol and a dynamic relocation for it. Fuchsia's dynamic linker sets such a symbol to 0; this repo's binder binds only to the vDSO's definitions and knows nothing of weak symbols. With an empty weak definition in ort and fsio's strong one, the symbol is defined in every program, the linker picks fsio's where fsio is linked, and nothing is left for the loader.

`install()` now binds once and records its answer, which every later call returns. The programs that check it (the shell, pc-ps2, routing_checks) keep their code, and learn how the runtime's install went.

**Reference**
Fuchsia: musl's `__libc_extensions_init`, declared weak in `zircon/third_party/ulib/musl/src/internal/libc.h`, called at startup, and defined by fdio (`sdk/lib/fdio/unistd.cc:586`), which binds the namespace from the processargs handles before main.

**Verification**
`llvm-nm`/`llvm-readelf` on the built programs: `ort_extensions_init` is defined in all of them, with no dynamic symbol or relocation; it is a lone `retq` in profile and calls `fsio::install()` in the shell. `boot_check.py --out out/sampler`: dbg PASS (34 passed, 2 skipped), rel PASS (30 passed, 10 skipped, 6/6 required); on dev, `init: routing_checks exited with 0`, including its check that the spawned child's namespace installed, and pc-ps2 ready.

**Deliberately not done**
Removing the programs' own `install()` calls: they are how a program that cannot run without its namespace learns it failed.

**How it was found**
The user asked whether a program should have to name ort itself, and how to open a file; they chose this option.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can ort not call `fsio::install()` directly?
- What did the weak reference leave in a program without fsio, and why would this repo's loader fail on it?
- Why does `install()` remember its first answer instead of binding again?
