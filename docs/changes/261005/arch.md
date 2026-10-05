# arch changes

## Machine instructions in generic code go through arch and runtime helpers (2026-10-05)

**Files**
- `kernel/lib/arch/include/arch/boot_marker.hpp`, `kernel/arch/x86/lib/arch/include/arch/x86/boot_marker.hpp` — new: `arch::boot_marker()`, one byte straight to the debug serial port
- `kernel/top/main.cpp`, `kernel/lib/ccrt/src/runtime.cpp` — their own copies of a raw COM1 `out` give way to `arch::boot_marker()`
- `kernel/lib/ccrt/BUILD.gn` — depends on the arch headers, named outright for the phys toolchain
- `kernel/platform/pc/debug_console.cpp` — `pause` becomes `arch::spin_hint()`
- `system/lib/ort/include/ort/spin_hint.hpp`, `system/lib/ort/BUILD.gn` — new: `ort::spin_hint()`, user space's one spin hint
- `kernel/lib/userabi/userboot/userboot.cpp`, `system/service/init/init.cpp` — `pause` becomes `ort::spin_hint()`
- `kernel/lib/arch/BUILD.gn`, `kernel/arch/x86/BUILD.gn` — list the new headers, and `arch/x86/stack_root.hpp`, which the previous change left out
- `docs/repairs/261005/init.md` — new: init's halt spins for ever

**Symptom**
The user noticed C++ code writing x86 instructions where they did not belong. Two copies of a raw COM1 write sat in architecture-neutral code (the kernel's startup and the C++ runtime), hard-coding x86's port 0x3F8 and duplicating each other. Three spin loops wrote `pause` inline, one in kernel platform code and two in user programs.

**Cause**
The arch layer exists so that generic code says what it wants and each architecture says how. Generic code that writes the instruction itself can only build for one architecture and repeats knowledge that should live in one place. The wrappers were already there (`arch::outb()`, `arch::spin_hint()`); these call sites predate them or did not use them.

Not every inline `asm` is misplaced. Kept as they are:

- Code whose purpose is a particular instruction: the fault probes (`kernel/lib/syscall/test/fault_probe`, `examples/shell`) execute `ud2`, `hlt` and port I/O from ring 3 to check the faults, and the fake kernel is a self-contained test image.
- `asm` that is not an instruction: `asm("name")` symbol labels (`LINK_NAME`, hsien's section symbols), and the empty `asm("")` compiler barrier in `cpu_local.hpp`.
- `kernel/platform/pc/power.cpp`'s triple fault, platform code for the PC, which is x86 by definition.

**Change**
The marker writes become one arch function, defined per architecture:

```cpp
// Before, in kernel/top/main.cpp and again in kernel/lib/ccrt/src/runtime.cpp
asm volatile("out %0, %1" : : "a"(c), "d"(u16{0x3F8}));
// After
arch::boot_marker(c);
```

The spin loops call the wrappers. In user space there was none, so `ort::spin_hint()` is new, written per architecture as the kernel's is (PAUSE, YIELD, the riscv pause hint). It keeps the `asm volatile`, and with it the property both user loops depend on: C++ lets a compiler assume a loop with no side effects ends, and clang deletes `for (;;) {}` at -O2, so a loop meant to spin for ever needs a side effect the optimiser must keep.

The x86 phys handoff keeps its own marker write: it is arch code already, and the phys environment's dependencies are its own.

**Reference**
None for the cleanup itself. Splitting generic interfaces from per-architecture headers is the existing `arch/<name>.hpp` and `arch/<arch>/<name>.hpp` pattern of `kernel/lib/arch`.

**Verification**
Build, and `boot_check.py --out out/sampler`: dbg PASS (34 passed, 2 skipped), rel PASS (30 passed, 10 skipped, 3/3 required). The phys image's initializer markers (`\x00\x01\x02\x03` before the boot shim's first line, from ccrt) and the kernel's markers come out byte for byte as before. No inline instruction is left in the five files.

**Deliberately not done**
init's halt still spins, as before, and keeps a CPU busy for ever; blocking instead changes behaviour and is a separate decision (`docs/repairs/261005/init.md`). userboot's spin is deliberate: it keeps ring 3 taking timer interrupts as evidence the proof-of-life image resumed user mode.

**How it was found**
The user asked whether some inline assembly was misplaced; a search of every inline `asm` outside the arch directories sorted the cases above.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Which inline `asm` belongs in generic code, and which does not?
- Why do the user programs' spin loops need a side effect at all, and what happened without one?
- Why did ccrt need the arch headers named in its build file, when kernel code gets them by default?
