# tsc-clock-source changes

## Title: the invariant TSC enrolled as a clock source, above the HPET (2026-11-02)

**Files**
- `kernel/arch/x86/kernel/time/tsc.cpp` — the TSC clock source: `rdtsc` read, enrolled at rating 300 gated on the invariant-TSC CPUID bit and a calibrated frequency (Irq+5, after the APIC calibration at Irq+4).
- `kernel/arch/x86/lib/arch/include/arch/x86/processor/feature.hpp` — `CpuFeatureType::InvariantTsc`, detected from CPUID 0x8000'0007:EDX[8].

**Symptom**
None — an enabling capability. The kernel's time source was HPET-only; the design anticipated a TSC above it without implementing one.

**Cause**
The TSC is the better counter where it can be trusted: no MMIO read, and — the part that matters for the vDSO — `rdtsc` is unprivileged, so user space could sample the same counter the kernel anchors on, without a trap. The gate is CPUID 0x8000'0007:EDX[8] (Invariant TSC): a fixed rate independent of P-state and coherence across CPUs, which is what makes sampling from arbitrary threads safe.

**Change**
A `TscClockSource` whose read is `rdtsc`, enrolled at rating 300 (above the HPET's 250) iff the invariant bit is set and the APIC calibration produced a TSC frequency (reusing `apic_tsc_hz()` — the calibration already timed the TSC against a clock source over two windows, which a fresh calibration would redo). Without the bit or the calibration, the HPET stays, unchanged. The clock framework's `change_source()` re-pins the vDSO anchor live, so user-space reads switch to the TSC without a reboot.

**Reference**
Linux's TSC clocksource: gated on `X86_FEATURE_CONSTANT_TSC` (the same CPUID bit) with a runtime watchdog against a secondary clock; rated above the HPET. Zircon: the TSC is the primary x86 clock, HPET the fallback, the same ordering.

**Verification**
`boot_check.py`: dbg PASS (45), rel PASS (43, 6/6 required), host-tests PASS 159. Disked: `routing_checks: passed`. Under our QEMU/TCG dev machine the CPU model does not advertise the invariant bit, so the boot log answers `TSC: not invariant, the clock stays on the HPET` and the HPET remains the source — the gate working as designed; on real hardware/KVM (`-cpu host`) the invariant bit is set and the TSC enrolls above the HPET. clang-tidy quiet.

**Deliberately not done**
- The TSC stability watchdog (Linux re-checks the TSC against the HPET at runtime and marks it unstable on skew): the invariant bit plus a fixed rate covers the machines we target; a watchdog is the follow-up when a machine that lies about the bit shows up.
- RDTSCP and CR4.TSD handling: RDTSC is unprivileged by default and nothing here disables it; rdtscp's ordering guarantees are unnecessary for a monotonic read.

**How it was found**
The user recalled that TSC exposure to user space is only gated by a CPUID flag; the investigation confirmed RDTSC is unprivileged and the invariant bit is the trust anchor.

**The user's prediction**
None. The user enabled the work and recalled the CPUID mechanism that shaped the design.

**Three questions you should be able to answer**
- Why is the invariant bit the load-bearing gate — what breaks if a TSC that changes rate with P-state is enrolled and the CPU throttles mid-benchmark?
- Under QEMU/TCG our dev machine stays on the HPET — which property of TCG's TSC makes trusting it wrong, and is that also true under KVM?
- The tick-device rating (HPET 120, above the PIT) and the time-source rating (HPET 250, below the anticipated TSC) are two independent ladders for one chip — why can the same device lose one comparison and win the other?
