# lockup_detector

When the machine has stopped, every CPU's registers and backtrace, printed once by one CPU, then a panic. The design and its decisions are in `.agents/tasks/lockup-watchdog/DESIGN.md`.

What it does today (step 1 of that design's Staging): an NMI that nothing in the kernel sent asks for a dump. Such an NMI comes from outside the machine through the boot CPU's LINT1 (QEMU's monitor `nmi`, a front-panel button), or from `lockup nmi` in the kernel shell. The CPU that takes it becomes the reporter: it sends every other online CPU an NMI, each of them leaves its interrupted context in its own slot and halts inside the NMI, and the reporter prints one block per CPU in panic mode and panics. The tick-driven checker that finds a stuck CPU by itself is step 2.

## Layout

```text
include/lockup_detector/   public headers, reached as <lockup_detector/...>
src/report.cpp             the NMI dump: the claim, the slots, the report
```

## What the architecture supplies

- `ours::arch_mp_send_nmi()` (`ours/arch/mp.hpp`): an NMI to each CPU of a set, bounded per target.
- A call to `lockup_detector::on_nmi()` from the NMI vector, with the per-CPU data in place (x86: `faults.cpp`).
- `lockup_detector::arch_dump_registers()`: a CPU's register lines, in the layout a kernel fault's report uses.
- LINT1 routed to NMI on the boot CPU (x86: `local_apic.cpp`), so that an NMI from outside arrives.

## Reading a report

Addresses are offsets from the start of the kernel image, which is linked at 0: look them up in the image's unstripped ELF with `prebuilt/clang/llvm-23/bin/llvm-objdump -d` or `llvm-nm -n`. The dbg image's ELF is `kernel_x86.dbg.1/obj/kernel/products/bin/vmours` in the build directory, the rel image's `kernel_x86.dbg.0/obj/kernel/products/bin/vmours.with-tests` (the `debug` entries of `kernel.dev.dbg_1.debug.json` and `kernel.dev.debug.json`). A backtrace is a frame-pointer walk bounded to the running thread's kernel stack, so it loses a leaf function's caller and stops at a stack it cannot see.

## Building and testing

The library is `//kernel/lib/lockup_detector`, kernel-only; its headers alone are `//kernel/lib/lockup_detector:headers`. Nothing of it runs on the host yet: the checker of step 2 brings the first pure function, and its host test. The report is tested on the dbg image with `lockup nmi` (`//kernel/commands`).
