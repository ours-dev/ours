# kernel-arch-seam outstanding repairs

## 1. x86's headers declare three functions nothing uses: ours::time::hpet_init(), ours::time::pit_init() and arch_mp_reschedule()

**Severity**
Low, a question for the user: nothing is wrong at run time, but each declaration misleads a reader, and one of them, with the generic `arch_` prefix in an x86-private header, misleads a porter.

**Symptom**
- `kernel/arch/x86/kernel/include/ours/arch/x86/time.hpp` declares `ours::time::hpet_init() -> Status` and `ours::time::pit_init() -> Status`. Neither is defined anywhere (`git grep`). The function that exists is `ours::hpet_init(PhysAddr)`, in another namespace (`ours/arch/hpet.hpp`, `time/hpet.cpp`), and the PIT is set up by `setup_pit()` in `time/time.cpp`.
- `kernel/arch/x86/kernel/include/ours/arch/x86/mp.hpp` declares `arch_mp_reschedule(CpuNum)`, and `mp.cpp` defines it, but nothing calls it. Its one caller, `IpiService::send_resched(CpuNum)`, was made to delegate to the mask form in `85a8fb69` (2026-09-10), which calls `arch_mp_reschedule_mask()`.

**Cause**
Leftovers from earlier refactors that the arch-seam move (`1cc87400`) carried over as they were, rather than delete without asking: `ours-kernel-design` says an uncalled function may be scaffolding for planned work and is asked about, not removed as cleanup. `arch_mp_reschedule()` stayed in x86's header because no generic code calls it, but its `arch_` prefix is the one the generic seam (`ours/arch_mp.hpp`) uses, so a riscv porter reading x86's header may take it for part of the seam and write a riscv body nothing calls.

**Change**
For the user to choose, per function:
- `ours::time::hpet_init()` and `ours::time::pit_init()`: delete the two declarations (recommended: no definition has existed since at least `1387a1a3`, so nothing can call them), or keep them for a planned split of the timer set-up.
- `arch_mp_reschedule()`: delete it with its declaration (recommended, since `IpiService` reaches every reschedule through the mask form); or keep it as x86's own and rename it `x86_mp_reschedule()`, so its prefix says it is not part of the seam; or move it back into the generic seam if a single-CPU reschedule is wanted on every architecture.

**Reference**
None, this project's own cleanup. Zircon's generic seam has `arch_mp_reschedule(cpu_mask_t)` only (`zircon/kernel/include/arch/mp.h`), with no single-CPU form.

**Acceptance**
`git grep -n "hpet_init()\|pit_init()\|arch_mp_reschedule("` over `kernel/` finds no declaration without a definition and no definition without a caller; x86 `ours-verify` passes.
