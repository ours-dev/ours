# elfload outstanding repairs

## 1. elfload takes its ELF machine from the compiler's architecture, so it builds only for x86-64

**Severity**
Low: nothing is wrong at run time; it keeps the library from being tested on the host.

**Symptom**
`system/lib/elfload/src/program.cpp` stops with `#error "elfload has no ELF machine for this target"` when compiled for the host, which is arm64 here, so `elfload` has no host tests and nothing that depends on it can have any. `launch::Program::read()` had to avoid it for that reason.

**Cause**
`MACHINE` is chosen by `#if defined(__x86_64__)` (`program.cpp:17-22`): the architecture the library is compiled for, not the one the programs it checks are built for. They differ for every host tool and test.

**Change**
Take the machine from the target as a parameter of `check_program()` (a constant the build defines is ruled out: the architecture is detected by the compiler's macros alone, `ours-kernel-design` layering.md), and add host tests for `check_program()` and the `Binder` on ELF images built in the test, as `launch`'s are.

**Reference**
None, this project's own code; Zircon's `elfldltl` is a template over the ELF class and machine and has host tests.

**Acceptance**
`elfload` builds in the host toolchain, and a host test runs `check_program()` and `Binder::bind()`.
