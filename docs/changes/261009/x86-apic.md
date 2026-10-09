# x86-apic changes

## Every CPU masks LINT0, the 8259's virtual wire, so QEMU no longer slows the boot CPU for other CPUs' UART writes (2026-10-09)

**Files**
- `kernel/arch/x86/lib/arch/include/arch/x86/apic/xapic.hpp` — `XApic::mask_lint0()`, beside `route_lint1_to_nmi()`: writes LINT0's LVT entry as ExtINT delivery under the mask bit (`0x10700`). The register itself, `XApicRegType::LvtLogicalInt0` at `0x350`, was already there.
- `kernel/arch/x86/apic/local_apic.cpp` — `init_apic_lint0()`, beside `init_apic_lint1()` and called just before it from `init_local_apic_percpu()`, so on every CPU: logs on the boot CPU what the firmware left in LINT0, then masks it.
- `scripts/kshell_run.py` — `--delay N`: N seconds of the image running between the boot's end marker and the first command, so that `lockup status` can be read after many checks.
- `docs/repairs/261008/kernel-lib-logz4.md` — item 1 removed (fixed here); what is left of it, the console lock's unfairness, is item 2, Low.
- `docs/repairs/261005/kernel-top.md` — deleted: its only item, CPU 0 running fifty to a hundred times slower on dbg, is fixed here.

**Symptom**
On the dbg image the boot CPU lost ticks and ran many times slower than the others for as long as the debug threads printed, every boot (`docs/repairs/261008/kernel-lib-logz4.md` item 1, `docs/repairs/261005/kernel-top.md` item 1, both removed by this change):

- cpu0 took 10% to 16% fewer ticks over a boot than cpu1 to cpu3, and as few as 72 to 76 of its 100 in its leanest second (56 with a second QEMU on the host); cpu1 to cpu3 never fewer than 99 (`docs/changes/261008/lockup-detector.md`).
- Its writes to the console waited up to 50 ms for the lock with interrupts off: 249 and 40 writes in two boots kept interrupts off for longer than a tick.
- User work placed on cpu0 ran 20 to 60 times slower: the same boot's user code took 286 and 713 of cpu0's ticks, against 11 to 15 without the debug threads.
- So the dbg boot's time depended on how much of the shell's work landed on cpu0: the shell finished anywhere from 4.8 s to 59.2 s (`docs/changes/261007/lockup-detector.md`), 29 to 45 s on 2026-10-05, against `boot_check.py`'s 60 s limit.

rel, under the same QEMU, lost nothing: it prints no debug lines, so no CPU wrote to the UART flat out.

**Cause**
The objects:

- The local APIC's LINT0: the first of its two external interrupt pins, where a PC wires the output of the legacy 8259 interrupt controller. In ExtINT delivery mode the pin is the "virtual wire": the CPU takes an interrupt from the 8259 as if it had no local APIC, fetching the vector from the 8259 itself. The firmware leaves the boot CPU's LINT0 at `0x700`, ExtINT and unmasked (the new log line reads it on every boot); an AP's comes out of INIT masked. Before this change the kernel never wrote it: the one piece of code that touches it, `XAPIC_CHIP` in `local_apic.cpp`, is `[[maybe_unused]]` and bound to no line.
- The 8259: masked at its own mask register by `Pic::disable()` at the start of `platform_init_apic()`, and otherwise unused; every ISA line is programmed in the IO-APIC instead.
- QEMU's TCG: each vCPU is a host thread running translated code; device emulation runs under QEMU's one global lock, and a vCPU kicked out of its translated code takes that lock before it goes on.
- The console, `SerialLogger::write()`: interrupts off, a test-and-set lock with no queue, then one poll of COM1 per byte.
- The tick: periodic, emulated on a one-shot device; a handler that runs more than a period late counts the missed periods in jiffies only, so the CPU's heartbeat, which `lockup status` reads, skips them.

The events, as found by four experiments of two dbg boots each on 2026-10-08 (`docs/changes/261008/kernel-lib-logz4.md`):

1. cpu2 and cpu3 run the debug threads `Sample1` and `Sample2` (`kernel/top/main.cpp`), each writing `I'm Thread N` in a loop with no sleep: tens of thousands of lines a second, a port read and a port write per byte.
2. Each write to the UART's transmit register makes QEMU's UART drive its interrupt line again, low, since the kernel enables no UART interrupt. QEMU passes the line to the 8259 whether it changed or not; the 8259 re-evaluates its output, masked or not, and QEMU hands that output to every local APIC that accepts 8259 interrupts. A local APIC accepts them while its LINT0 is unmasked: only cpu0's.
3. For an ExtINT LINT0, QEMU then re-evaluates cpu0's pending interrupts, and since this runs on cpu2's or cpu3's thread, not cpu0's, it kicks cpu0's vCPU out of its translated code to poll its APIC under the global lock. This is the path in QEMU's source as known (`hw/char/serial.c`, `hw/intc/i8259.c`, `hw/intc/apic.c`), not re-read for the 11.1.1 in `prebuilt/`; experiment 4 below is what establishes the effect.
4. cpu0 is kicked for every byte the other CPUs write, so it runs 20 to 60 times slower for as long as they print: user code, which takes no lock, slowed as much as the kernel.
5. In the console's test-and-set race the lock goes to whichever CPU's exchange lands first after a release. cpu2 and cpu3 retry at full speed; cpu0 retries rarely and almost never wins: it waited 5 ms on average where cpu1, the same third party to cpu2 and cpu3, waited 0.05 ms, and up to 50 ms, all with interrupts off.
6. Every wait over a period costs cpu0 ticks.

The experiments separated the layers. Without the debug threads (experiment 2), nothing was lost and the user work took 13 and 15 of cpu0's ticks. A temporary ticket lock (experiment 3) removed the long waits and the lost ticks but not the slowness: cpu0 still spent 338 ticks in user code, and the boot still took 17.5 s. A temporary mask of the boot CPU's LINT0 (experiment 4) removed all three: no lost ticks, waits of at most 0.57 ms, 11 user-mode ticks, the boot over in 4.4 and 4.5 s. So the slowness was QEMU's kick, through the one pin nothing used; the unfair lock only turned it into interrupts-off time.

The four experiments' tables, moved here from the repair item they were recorded in (`docs/repairs/261008/kernel-lib-logz4.md` item 1, removed by this change). The measurements: `scripts/kshell_run.py --out out/bringup.x86 "lockup status"` on dbg, one boot at a time, host load average 3.6 to 6.9 on 15 cores. "Ticks lost" is cpu1's heartbeat minus cpu0's, plus 25: cpu0 starts ticking about 0.25 s before the APs and leads by 25 whenever nothing is lost (experiments 2 and 4, and the second boot of 3). "User-mode ticks" counts cpu0's `Exception[apic timer] from user space` trace lines. "Boot ends" is the guest time of the shell's `--- end ---`; the status follows it at once, so it covers the boot's ticks.

Experiment 1, as built: the test-and-set lock, the debug threads, LINT0 as the firmware left it.

| Measure | Boot 1 | Boot 2 |
|---|---|---|
| Boot ends | 33.5 s | 11.8 s |
| Ticks taken, cpu0 and cpu1 | 2906 and 3111 | 859 and 892 |
| cpu0's ticks lost | about 230 | about 58 |
| cpu0's fewest ticks between two checks | 80 | 84 |
| cpu0's lines, and how many found the lock taken | 11,771 and 3,136 | 9,287 and 750 |
| cpu0's wait in all, and its longest | 16,095 ms and 49.570 ms | 3,683 ms and 21.443 ms |
| cpu0's writes off for over a tick, and its longest stretch | 249 and 49.643 ms | 40 and 21.479 ms |
| cpu1's waits, wait in all, and longest | 1,003, 50 ms and 0.437 ms | 3,024, 151 ms and 0.708 ms |
| Lines of cpu2 and of cpu3 | 484,467 and 484,344 | 139,349 and 139,466 |
| cpu0's user-mode ticks | 713 | 286 |

Experiment 2: the debug threads not started (a temporary early return in `labour_routine()`), the rest as built.

| Measure | Boot 1 | Boot 2 |
|---|---|---|
| Boot ends | 4.3 s | 4.0 s |
| Ticks taken, cpu0 and cpu1 | 182 and 157 | 178 and 153 |
| cpu0's ticks lost | 0 | 0 |
| cpu0's lines, and how many found the lock taken | 11,051 and 6 | 11,056 and 5 |
| cpu0's wait in all, and its longest | 0.309 ms and 0.125 ms | 0.255 ms and 0.090 ms |
| cpu0's writes off for over a tick, and its longest stretch | 0 and 0.229 ms | 0 and 0.285 ms |
| cpu1's waits, wait in all, and longest | 55, 1.4 ms and 0.277 ms | 56, 1.4 ms and 0.178 ms |
| cpu0's user-mode ticks | 13 | 15 |

Experiment 3: a temporary ticket lock in place of the test-and-set lock (a ticket from `fetch_add`, served in order; panic mode by a compare-exchange try), debug threads and LINT0 as built. From here the account also gives the ticks its long stretches cost.

| Measure | Boot 1 | Boot 2 |
|---|---|---|
| Boot ends | 17.5 s | 4.1 s |
| Ticks taken, cpu0 and cpu1 | 1508 and 1487 | 190 and 165 |
| cpu0's ticks lost | about 4 | 0 |
| cpu0's fewest ticks between two checks | 99 | no two checks yet |
| cpu0's lines, and how many found the lock taken | 11,444 and 2,737 | 17,737 and 6,557 |
| cpu0's wait in all, and its longest | 161 ms and 0.300 ms | 203 ms and 0.231 ms |
| cpu0's writes off for over a tick, longest stretch, ticks they cost | 0, 0.856 ms, 0 | 0, 0.424 ms, 0 |
| cpu1's waits, wait in all, and longest | 949, 49 ms and 0.321 ms | 2,954, 81 ms and 0.285 ms |
| Lines of cpu2 and of cpu3 | 234,806 and 234,811 | 487 and 38,467 |
| cpu0's user-mode ticks | 338 | 0: the shell's user work ran on cpu1 and cpu2 |

Experiment 4: LINT0 masked on the boot CPU (a temporary mask in `init_apic_lint1()`), the test-and-set lock and the debug threads as built.

| Measure | Boot 1 | Boot 2 |
|---|---|---|
| Boot ends | 4.4 s | 4.5 s |
| Ticks taken, cpu0 and cpu1 | 199 and 174 | 201 and 176 |
| cpu0's ticks lost | 0 | 0 |
| cpu0's lines, and how many found the lock taken | 11,049 and 2,528 | 11,037 and 2,495 |
| cpu0's wait in all, and its longest | 91 ms and 0.568 ms | 91 ms and 0.461 ms |
| cpu0's writes off for over a tick, longest stretch, ticks they cost | 0, 0.592 ms, 0 | 0, 0.485 ms, 0 |
| cpu1's waits, wait in all, and longest | 995, 40 ms and 0.297 ms | 1,008, 44 ms and 0.605 ms |
| Lines of cpu2 and of cpu3 | 36,464 and 36,622 | 36,278 and 36,306 |
| cpu0's user-mode ticks | 11 | 11 |

Why the wire could be cut: nothing the kernel uses comes through it.

- The PIT, the legacy tick, attaches to its IO-APIC pin: `arch_time_init()` (`kernel/arch/x86/kernel/time/time.cpp`) attaches and unmasks GSI `apic_io_isa_to_global(ISA_IRQ_PIT)`, GSI 2 on q35, and nothing falls back to the 8259 if that fails; it logs an error instead. The HPET's switch masks the same line, at the IO-APIC.
- Every ISA line is programmed in the IO-APIC with Fixed delivery to the boot CPU (`apic_configure_isa_irq()` in `platform_init_apic()`); nothing in `kernel/` or `system/lib/arch` uses ExtINT delivery (`ApicDeliveryMode::External`) except the new mask.
- The 8259 is masked at its own registers before the IO-APIC is set up, and never remapped or acknowledged: `Pic::remap()`, `issue()`, `mask()` and `unmask()` have no callers. An 8259 interrupt delivered through ExtINT would even arrive on the firmware's vector base, which this kernel does not handle.
- `XAPIC_CHIP`, the `irq::Chip` whose mask and unmask act on LINT0, is unused, so nothing unmasks the pin again.
- LINT1, the NMI pin, is a separate LVT entry and is untouched.

**Change**
The register already had its name, `XApicRegType::LvtLogicalInt0` (`0x350`); what was missing was a write. `XApic` gets one beside the LINT1 one, written as a whole word as `route_lint1_to_nmi()` is, rather than a read-modify-write of whatever the firmware left:

```cpp
// kernel/arch/x86/lib/arch/include/arch/x86/apic/xapic.hpp, after
auto mask_lint0() noexcept -> void {
    write_reg(XApicRegType::LvtLogicalInt0,
              (static_cast<u32>(ApicDeliveryMode::External) << DELIVERY_MODE_SHIFT) |
                  INTERRUPT_MASK);
}
```

and every CPU calls it while it sets up its local APIC:

```cpp
// kernel/arch/x86/apic/local_apic.cpp, before
    init_apic_pmi();
    init_apic_lint1((apic_base & arch::IA32_APIC_BASE_BSP) != 0);
}

// after
    init_apic_pmi();
    init_apic_lint0((apic_base & arch::IA32_APIC_BASE_BSP) != 0);
    init_apic_lint1((apic_base & arch::IA32_APIC_BASE_BSP) != 0);
}

static auto init_apic_lint0(bool const is_boot_cpu) -> void {
    if (is_boot_cpu) {
        log::info("local APIC: the firmware left LINT0 at {:#x}; masking it, as nothing uses the "
                  "8259's virtual wire",
                  XAPIC_STORAGE.read_reg(arch::XApicRegType::LvtLogicalInt0));
    }
    XAPIC_STORAGE.mask_lint0();
}
```

Every boot now logs, on both images:

```text
local APIC: the firmware left LINT0 at 0x700; masking it, as nothing uses the 8259's virtual wire
```

Choices:

- **Every CPU, not the boot CPU alone.** Only the boot CPU's was open, but a CPU that relied on what INIT or the firmware left would be the same assumption that left this one open; the write costs one MMIO store per CPU at boot.
- **ExtINT under the mask, not a zeroed word.** The mode bits say what the pin carries, so a register dump still reads as "the virtual wire, switched off"; the vector and trigger bits mean nothing in ExtINT mode and stay clear. A zeroed, masked word would also do; this is the value Linux writes.
- **In the local APIC's per-CPU setup, not after the IO-APIC's.** On the boot CPU it runs inside `platform_init_apic()`, after the 8259 is masked and with interrupts off until that function has programmed the IO-APIC, so nothing can arrive in between; an AP runs it long after. Masking later, once the timer is shown to work through the IO-APIC, would only matter if the kernel could fall back to the 8259 for its tick, and it cannot.
- **No x2APIC register.** The kernel reaches the local APIC only through the xAPIC MMIO window (`X2APIC_ENABLED` is a constant `false`), so the x2APIC MSR for LINT0 (`0x835`) has no user.
- **`kshell_run.py --delay`.** The item's acceptance reads `lockup status` after at least 15 s of the debug threads, and the script typed it one second after the end marker. The option adds its seconds to that one, reading and dropping the console meanwhile, so the silence watch still runs; nothing else changes.

**Reference**
- Linux: `setup_local_APIC()` (`arch/x86/kernel/apic/apic.c`) writes `APIC_DM_EXTINT | APIC_LVT_MASKED` to every CPU's LVT0 except the boot CPU's, which it leaves as ExtINT when the 8259 is in use (`pic_mode`, or no IO-APIC) or when the firmware left it unmasked; `check_timer()` (`arch/x86/kernel/apic/io_apic.c`) then writes `APIC_LVT_MASKED | APIC_DM_EXTINT` to the boot CPU's before testing the timer through the IO-APIC, and opens LVT0 again only when the timer does not work that way: first as a virtual wire with the timer's own vector (`APIC_DM_FIXED | cfg->vector`), then as ExtINT (`APIC_DM_EXTINT`). This change is that end state without the fallbacks, since this kernel has no 8259 timer path.
- Zircon: `apic_local_init()` (`zircon/kernel/arch/x86/src/lapic.rs:237`) sets the spurious vector, the error entry and the timer, and never writes LINT0 (`LAPIC_REG_LVT_LINT0`, `0x350`, is defined beside LINT1 and unused); the platform masks the 8259 with `pic_disable()` in `platform_init_apic()` (`zircon/kernel/platform/pc/interrupts.rs:111`), as `Pic::disable()` does here. That is the shape this kernel had, and the measurements show the 8259's mask alone does not stop QEMU's kick: the 8259 hands its output on whether it is masked or not, and only the local APIC's mask makes the boot CPU refuse it.

**Verification**
- `cpp-write-and-verify`: `scripts/format_cpp.py` clean on both C++ files; `clang-tidy -p out/bringup.x86 kernel/arch/x86/apic/local_apic.cpp` reports nothing in `local_apic.cpp` and nothing new in `xapic.hpp` (its one finding, `mmio_base_`'s case, predates this change).
- `boot_check.py`, twice: dbg PASS (shell finished at 5.0 s and 5.0 s, 35 passed, 3 skipped, 0 failed, 0 error lines), rel PASS (4.1 s and 3.8 s, 33 passed, 9 skipped, 0 failed, 6/6 required, `pc-ps2: key a` and `pc-ps2: key A`, `profile: ... 0 dropped`), host-tests PASS (143). Both logs carry the LINT0 line, `0x700` on both images.
- The acceptance of `docs/repairs/261008/kernel-lib-logz4.md` item 1: `scripts/kshell_run.py --out out/bringup.x86 --delay 15 "lockup status"` on dbg, twice, the status read at 20.4 s and 20.35 s of guest time, about 16.7 s after the debug threads' first line (3.65 s), host load average 4.2 to 4.5 on 15 cores:

| Measure | Boot 1 | Boot 2 |
|---|---|---|
| Boot ends (guest time of the end marker) | 4.4 s | 4.3 s |
| Ticks taken, cpu0 to cpu3 | 1707, 1682, 1681, 1680 | 1690, 1665, 1664, 1663 |
| cpu0's ticks lost (cpu1's heartbeat minus cpu0's, plus the 25 of its head start) | 0 | 0 |
| cpu0's fewest ticks between two checks, as cpu1, cpu2, cpu3 saw it | 100, 100, 100 | 100, 100, 99 |
| cpu0's lines, and how many found the lock taken | 11,045 and 2,514 | 11,034 and 2,520 |
| cpu0's wait in all, and its longest | 107.5 ms and 0.569 ms | 79.3 ms and 0.481 ms |
| cpu0's writes off for over a tick, longest stretch, ticks they cost | 0, 0.617 ms, 0 | 0, 0.555 ms, 0 |
| Lines of cpu2 and of cpu3 | 399,530 and 398,839 | 403,297 and 402,700 |
| Longest wait of any CPU | 2.815 ms (cpu2) | 2.907 ms (cpu2) |
| cpu0's user-mode ticks | 14 | 10 |

cpu0's heartbeat is cpu1's plus exactly its head start in both, so within 1% of the others' (0%); its fewest ticks between two checks is 99 to 100, against the 95 asked; its console line reads 0 writes over a tick. Every acceptance condition holds.

- Before and after, for cpu0 on dbg:

| Measure | Before (2026-10-08, experiment 1 and the threshold boots) | After (this change) |
|---|---|---|
| dbg shell finished | 4.8 s to 59.2 s; 33.5 s and 11.8 s guest in experiment 1 | 5.0 s, 5.0 s (`boot_check.py`); 4.4, 4.3, 4.3 s guest (`kshell_run.py`) |
| cpu0's ticks lost | about 230 and 58; 10% to 16% of a boot | 0 and 0 |
| cpu0's fewest ticks between two checks | 80 and 84; 72 to 76 in the threshold boots | 99 to 100 |
| cpu0's longest console wait | 49.6 ms and 21.4 ms | 0.57 ms and 0.48 ms |
| cpu0's writes off for over a tick | 249 and 40 | 0 and 0 |
| cpu0's user-mode ticks | 713 and 286 | 3, 17, 14, 10 |

"User-mode ticks" counts cpu0's `Exception[apic timer] from user space` trace lines, its ticks that landed in user code; it varies with how much of the shell's work the scheduler puts on cpu0, which is why 3 and 17 both mean "not slowed".
- The acceptance of `docs/repairs/261005/kernel-top.md` item 1: five dbg boots in a row (the two `boot_check.py` runs and the three `kshell_run.py` runs) finished the shell's checks at 5.0, 5.0, 4.4, 4.3 and 4.3 s, within a second of one another. Its other half, `profile: ... 0 dropped` on each, cannot be read on dbg today: the dbg image has no `bin/profile` in the shell's namespace, so the shell logs `shell: profile checks skipped (no namespace)`; rel printed `profile: ... 0 dropped` in both of its boots.
- The interrupt tests: `scripts/kshell_run.py --out out/bringup.x86 irq.test interruptline.test interrupt.test`: `irq.test: ok -- 72 checks`, `interruptline.test: ok -- 58 checks`, `interrupt.test: ok -- 32 checks`.
- 7 boots in all, one at a time: 4 for the two `boot_check.py` runs (dbg and rel side by side), 2 for `lockup status`, 1 for the interrupt tests.

**Deliberately not done**
- The console lock stays a test-and-set lock waited on with interrupts off. This change removes the one condition measured to starve a CPU on it, not the possibility; that is now `docs/repairs/261008/kernel-lib-logz4.md` item 2, Low, with the options that remain.
- The debug threads keep printing. Whether something should keep their role, the first sign of preemptive round-robin, is the user's call, and no longer a defect: with LINT0 masked they cost the other CPUs nothing measurable.
- `XAPIC_CHIP` is left as it is, unused: it may be scaffolding. Bound to a line one day, its `unmask` would open the virtual wire again; whoever binds it has to want that.
- `LvtLogicalInt0` and `LvtLogicalInt1` keep their names, though LINT means "local interrupt", not "logical": a rename of both, and of their users, is a change of its own.
- QEMU's internal path (events 2 and 3 of Cause) was not re-read in the 11.1.1 source nor traced; the measurements show the effect.

**How it was found**
The cause was found on 2026-10-08 (`docs/changes/261008/kernel-lib-logz4.md`, How it was found): the console counters put cpu0's lost ticks in its waits for the console lock; cpu0's user-mode ticks showed it slowed where it took no lock; the log's `the firmware left LINT1 at 0x400` and the absence of any write to LINT0 pointed at the boot CPU's virtual wire, which a temporary log line read as `0x700`; and a temporary mask (experiment 4) removed every symptom. This change is that mask, made permanent on every CPU, and measured over a long run.

**The user's prediction**
None: the user chose this option from the measured ones on 2026-10-09 ("那就屏蔽它") and wrote no prediction. Experiment 4 of 2026-10-08 stood in for one, and the long run matches it: no lost ticks, waits under a millisecond, the dbg boot over in about 4.5 s.

**Three questions you should be able to answer**
1. The 8259 was already masked at its own registers. Why did that not stop QEMU from kicking cpu0 on every UART write, and why does masking LINT0 in cpu0's local APIC stop it?
2. Why was it cpu0, and only cpu0, that slowed down, when cpu2 and cpu3 were the CPUs writing to the UART?
3. A ticket lock also removed the lost ticks. Why does masking LINT0 fix the dbg boot's time as well, while the ticket lock left it as spread as before?
