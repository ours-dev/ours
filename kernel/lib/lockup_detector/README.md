# lockup_detector

When a CPU has stopped taking its tick, or the machine has stopped, every CPU's registers and backtrace, printed once by one CPU, then a panic. The design and its decisions are in `.agents/tasks/lockup-watchdog/DESIGN.md`.

What it does today (steps 1 to 4 of that design's Staging):

- **The checker finds a stuck CPU by itself.** Every tick on every CPU moves that CPU's heartbeat, and once a second of its own ticks every CPU checks every other online CPU's. A CPU whose heartbeat has not moved for `kernel.lockup.threshold` checks in a row (10 by default, 0 turns the checks off) has interrupts off, and the CPU that finds it reports. The checks start at InitLevel::Platform, once every CPU is up, and stand down while a panic is under way or once a report is claimed.
- **An NMI that nothing in the kernel sent asks for a dump.** Such an NMI comes from outside the machine through the boot CPU's LINT1 (QEMU's monitor `nmi`, a front-panel button), or from `lockup nmi` in the kernel shell. It is how a machine whose every CPU has interrupts off, and so has no tick left to check with, is reached. The boot scripts (`boot_check.py` of the ours-verify skill, `scripts/kshell_run.py`) send the monitor's `nmi` themselves once the console has been silent for 15 s, and keep the report in the log.

Either way the CPU that claims the report sends every other online CPU an NMI, each of them leaves its interrupted context in its own slot and halts inside the NMI, and the reporter prints one block per CPU in panic mode and panics. A stuck CPU's block comes last, so that the crash record's tail keeps it.

`lockup status` shows how close every CPU has come to being found stuck. Each CPU's notes also keep the most checks in a row a target went without a tick and the fewest ticks it took between two checks; the command prints the threshold, each CPU's heartbeat and its notes on the others, and the closest any CPU came. A CPU's notes are copied on that CPU, in an IPI, since only its own tick may touch them. Six boots of both images, alone and with a second QEMU on the host, never saw a CPU go even one check without a tick, so the threshold stays at 10 (`docs/changes/261008/lockup-detector.md`).

## Layout

```text
include/lockup_detector/   public headers, reached as <lockup_detector/...>
  lockup_detector.hpp      on_tick(), on_nmi(), log_status(), and the arch hook arch_dump_registers()
  heartbeat_note.hpp       the note a CPU keeps of another CPU's heartbeat: the pure check
src/checker.cpp            the heartbeat, the once-a-second check, the threshold, the arming hook, `lockup status`
src/checker.hpp            what the report asks of the checker: this CPU's heartbeat; the library's own
src/report.cpp             the claim, the slots, the NMIs, the report
src/report.hpp             what the checker asks of the report; the library's own
test/                      host tests of the check
```

## What the rest of the kernel supplies

- The tick calls `lockup_detector::on_tick()` first, on every tick (`ours::time::TickSched::on_tick()`).
- `kernel.lockup.threshold=<seconds>` is parsed with the other boot options (`ours::boot::BootOptions::lockup_threshold()`).
- `ours::arch_mp_send_nmi()` (`ours/arch/mp.hpp`): an NMI to each CPU of a set, bounded per target.
- A call to `lockup_detector::on_nmi()` from the NMI vector, with the per-CPU data in place (x86: `faults.cpp`).
- `lockup_detector::arch_dump_registers()`: a CPU's register lines, in the layout a kernel fault's report uses.
- LINT1 routed to NMI on the boot CPU (x86: `local_apic.cpp`), so that an NMI from outside arrives.

## Reading a report

The first lines say what the report is about: `lockup: cpu<N> took no tick for <T> s, found by cpu<M>`, followed by how many checks in a row each CPU went without a tick as cpu<M> counted them (the larger the count, the earlier that CPU went quiet), or `lockup: dump asked for by an NMI on cpu<N>`. Then one block per CPU: thread, pc, whether interrupts were on, the spin locks it held, the ticks it had taken (its heartbeat), its registers and its backtrace. The ticks are counted from each CPU's own first tick, so they compare only roughly between CPUs, but a CPU that went quiet early is behind the others by about a hundred a second: in a dump asked for from outside, where no CPU still ticks and no checker has counts, they are what says in what order the CPUs stopped.

Addresses are offsets from the start of the kernel image, which is linked at 0: look them up in the image's unstripped ELF with `prebuilt/clang/llvm-23/bin/llvm-objdump -d` or `llvm-nm -n`. The dbg image's ELF is `kernel_x86.dbg.1/obj/kernel/products/bin/vmours` in the build directory, the rel image's `kernel_x86.dbg.0/obj/kernel/products/bin/vmours.with-tests` (the `debug` entries of `kernel.dev.dbg_1.debug.json` and `kernel.dev.debug.json`). A backtrace is a frame-pointer walk bounded to the running thread's kernel stack, so it loses a leaf function's caller and stops at a stack it cannot see.

## Building and testing

The library is `//kernel/lib/lockup_detector`, kernel-only but for `:heartbeat_note`, the pure check, which also builds on the host; `//kernel/lib/lockup_detector:tests` runs its tests there and is part of `//:default`. On the dbg image, `lockup spin <cpu>` (`//kernel/commands`) turns interrupts off on one CPU until the checker reports it, `lockup spin <cpu> <ms>` for a while only, `lockup spin all` on every CPU at once, which only the scripts' `nmi` reports, `lockup nmi` asks for a dump, and `lockup status` shows the checker's notes. `scripts/kshell_run.py` runs them and checks the report each leads to.
