# kernel-arch-seam changes

## Title: x86's headers stop declaring three functions nothing uses (2026-10-30)

**Files**
- `kernel/arch/x86/kernel/include/ours/arch/x86/time.hpp` — the declarations of `ours::time::hpet_init()` and `ours::time::pit_init()` are gone, and so is the `<ours/status.hpp>` include only they needed
- `kernel/arch/x86/kernel/include/ours/arch/x86/mp.hpp` — the declaration of `arch_mp_reschedule(CpuNum)` is gone
- `kernel/arch/x86/kernel/mp.cpp` — the definition of `arch_mp_reschedule(CpuNum)` is gone; its comment now heads `arch_mp_reschedule_mask()`, which does the same per CPU
- `kernel/arch/x86/kernel/include/ours/arch/idle_thread.hpp` — `ArchIdleThread::wake()`'s comment names `arch_mp_reschedule_mask()` as its caller
- `.agents/skills/ours-kernel-design/references/task.md` — the idle-wake rule names `arch_mp_reschedule_mask()`
- `docs/repairs/261029/kernel-arch-seam.md` — deleted: its only item is this one

**Symptom**
Nothing at run time. A reader of x86's headers found three functions that looked like part of the timer and IPI set-up but were not. `ours::time::hpet_init()` and `ours::time::pit_init()` were declared and defined nowhere. `arch_mp_reschedule(CpuNum)` was defined and called from nowhere, and its `arch_` prefix is the generic seam's (`ours/arch_mp.hpp`), so a riscv porter reading x86's header could take it for part of the seam and write a riscv body nothing calls.

**Cause**
All three are leftovers. The two `ours::time` declarations have existed since the oldest commit that has the file (`1387a1a3`, 2026-04-16, then `kernel/arch/x86/main/include/ours/arch/time.hpp`) and no version of the tree ever defined them: the HPET is set up by `ours::hpet_init(PhysAddr)` (`ours/arch/hpet.hpp`, `time/hpet.cpp`), a different function in another namespace that the platform's init hook `platform_hpet_init()` (`kernel/platform/pc/hpet.cpp`) calls with the address it reads from ACPI's HPET table, and the PIT by `setup_pit()` (`time/time.cpp`). C++ overloads by namespace and parameters, so a declaration with the same name and no definition links fine as long as nobody calls it, and the compiler never complains. `arch_mp_reschedule(CpuNum)` lost its last caller in `85a8fb69` (2026-09-10), when `IpiService::send_resched(CpuNum)` began delegating to the mask form, which runs the same monitor-word check per CPU. The arch-seam move (`1cc87400`) carried all three over unchanged instead of deleting them, because `ours-kernel-design` asks before an uncalled function is removed: it may be scaffolding for planned work. The user decided on 2026-10-30 that none of them is.

**Change**
The three are deleted, with the comments that named `arch_mp_reschedule()`. Ruled out: renaming `arch_mp_reschedule()` to `x86_mp_reschedule()` and keeping it, which keeps a function nothing calls; keeping the two `ours::time` declarations for a planned split of the timer set-up, which no plan contains.

**Reference**
None, this project's own cleanup. Zircon's generic seam has only the mask form, `arch_mp_reschedule(cpu_mask_t)` (`zircon/kernel/include/arch/mp.h`), with no single-CPU one.

**Verification**
- `git grep -n "hpet_init()\|pit_init()\|arch_mp_reschedule("` over `kernel/` finds only comments naming the real `hpet_init(PhysAddr)`.
- `llvm-nm -C` of the x86 dbg vmours has no `arch_mp_reschedule(` symbol.
- x86 `ours-verify`: build ok; dbg PASS shell finished at 5.8s · 44 passed, 4 skipped, 0 failed · 0 error line(s) · reached: end; rel PASS shell finished at 4.6s · 41 passed, 8 skipped, 0 failed · 6/6 required · 0 error line(s) · reached: end; host-tests PASS 152 passed, 0 failed.
- riscv check: PASS handoff ok at 0.1s · reached: handoff ok · initrd fake.obi.
- `format_cpp.py --check` clean on the four C++ files; `clang-tidy` reports nothing in `mp.cpp`.

**Deliberately not done**
None.

**How it was found**
The review of riscv-kernel step 1a, which saw the arch-seam move carry the three over, filed them as `docs/repairs/261029/kernel-arch-seam.md` item 1 for the user.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why does a declaration with no definition link without an error, and when would it stop linking?
- Why were `ours::time::hpet_init()` and `ours::hpet_init(PhysAddr)` never in conflict?
- What does the mask form of the reschedule do for each CPU in the mask before it sends any IPI?
