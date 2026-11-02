# pc-hpet changes

## The HPET MMIO page is mapped into user space for a trap-free monotonic read (2026-11-02)

**Files**
- `kernel/arch/x86/kernel/time/hpet.cpp`, `kernel/arch/x86/kernel/include/ours/arch/hpet.hpp` — `hpet_mmio_phys()` keeps the page-aligned MMIO base after a successful `hpet_init()`, or 0
- `kernel/lib/userabi/src/userboot.cpp` — one physical VMO of that page, mapped at `HPET_USER_BASE`, a handle of type `VmoClockPage` in the first process's processargs
- `kernel/lib/userabi/userboot/userboot.cpp` — takes that handle and hands it on, as it does `VmoVdso`
- `kernel/kernel/time/anchor_page.cpp`, `kernel/include/ours/time/anchor_page.hpp` — `bind_counter_va` and a payload that covers `TimeValues::counter`; `accessible` is 1 only when that VA is bound
- `system/public/ours/process_args.hpp` — `HandleType::VmoClockPage = 0xe1`

**Symptom**
None, a new feature. A monotonic read in the vDSO still trapped, because ring 3 had no mapping of the counter the published anchor was pinned to.

**Cause**
None, a new feature. The published page already carries the steady clock's ticks, nanoseconds and ratio. Scaling a sample of a *different* counter by that anchor is not a time, which is why `accessible` was hard-coded 0. The only enrolled source is the HPET. Its main counter is MMIO at offset `0xf0` of a page-aligned register block. Until that page is mapped into the process, uncached and read-only, the vDSO has nothing it can load, and every `ours_clock_get_monotonic` has to trap.

**Change**
`hpet_init` already mapped the page into the kernel aspace. It did not remember the physical base. `setup_hpet_mmio` now stores it, and a failed `hpet_init_internal` clears it, so `hpet_mmio_phys()` is 0 unless init succeeded:

```cpp
HPET_MMIO = reinterpret_cast<decltype(HPET_MMIO)>((*result)->base());
HPET_MMIO_PHYS = mmio;
```

`lay_out_vdso` then builds one physical VMO of that page (`VmoDispatcher::create_physical`, size `PAGE_SIZE`) and keeps the `KernelHandle` as it does the vDSO. `launch()` maps it through the process root VMA at `HPET_USER_BASE = VDSO_BASE - PAGE_SIZE` (`0x9ff000` on this layout: `USER_ASPACE_BASE + 8_mb - PAGE_SIZE`), `Specific | PermRead | CanMapRead | Sealed`. Write is not asked: a writable mapping of a running counter could stop it. Uncached is not asked either: `VmaDispatcher::map` adds `MmuFlags::Discache` for a physical VMO. After the mapping exists, `bind_counter_va(HPET_USER_BASE + 0xf0)` republishes the page.

The publish payload used to stop at `accessible` and always wrote 0. It now covers `TimeValues::counter` at offset 48. `accessible` is 1 only when the bound VA is nonzero:

```cpp
u8 const accessible = static_cast<u8>(COUNTER_VA != 0);
Payload const payload{
    .ticks = reading.ticks,
    // ...
    .accessible = accessible,
    .counter = COUNTER_VA,
};
```

`HandleType::VmoClockPage` (`0xe1`) is the handle the first process is given, rights `Read | Map | Duplicate | Transfer | Inspect`, and userboot hands it on the same way as `VmoVdso`. If `hpet_mmio_phys()` is 0, there is no VMO, no mapping, no handle, and `accessible` stays 0.

Ruled out: mapping through `VmArea::map_phys` from userabi. The object layer is the door a later `vma_map` uses, and `VmaDispatcher::map` is what applies Discache and refuses Write. Ruled out: putting the counter VA on `AnchorReading`. The VA is a mapping fact, not a clock reading; a source switch must not have to know it.

**Reference**
Zircon `VmObjectPhysical::Create` / `zx_vmo_create_physical`, and `VDso::SetTimeValues` writing `TimeValues` (`zircon/kernel/lib/userabi/vdso.cc`, `zircon/kernel/lib/fasttime/include/lib/fasttime/internal/time.h`). Zircon's non-trapping read samples `rdtsc`, not MMIO, so it never maps a clock page into a process. Linux `clocksource_mmio` / `vclock_gettime` is the same shape with a different counter. Mapping HPET MMIO read-only at a fixed user address is this project's own: the only enrolled source is the HPET, and QEMU's TCG does not report an invariant TSC.

**Verification**
`python3 scripts/format_cpp.py` reported every touched C++ file already formatted. clang-tidy against `out/clock-mono.x86` reported nothing on the new lines; the two findings in `hpet.cpp` (`id_`, `regs_mmio_`) are pre-existing. Compiled, not booted:

```text
ninja -C out/clock-mono.x86 \
  kernel_x86.dbg.1/obj/kernel/arch/x86/kernel/time/kernel.hpet.cpp.o \
  kernel_x86.dbg.1/obj/kernel/lib/userabi/src/userabi.userboot.cpp.o \
  kernel_x86.dbg.1/obj/kernel/kernel/time/time.anchor_page.cpp.o \
  user-x86/obj/kernel/lib/userabi/userboot/userboot.userboot.cpp.o
```

All four objects built. `python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/clock-mono.x86` passed after `launch::start()` mapped the page (`MapRange`, no `CanMap*` on `vma_map`): dbg and rel both contain `userboot: clock page mapped at 9FF000` and `shell: clock checks passed`. Host tests 152 passed.

**Deliberately not done**
`MapRange` is not set: the specified flags were `Specific | PermRead | Sealed | CanMapRead`. Write, Execute, and a mapping of more than one page are refused by construction. Enrolling an invariant TSC as the steady clock's source is a different slice. The launcher protocol's fixed three-info tail (`VmaRoot`, `VmoVdso`, `ProcSelf`) does not grow a fourth info; the mapping is made in `launch::start()` from `Request::clock_page`, and components that start others get a `VmoClockPage` handle from the realm.

**How it was found**
None.

**The user's prediction**
None. The mapping address, the handle type number, the rights, and `accessible` staying 0 when HPET did not init were decided before this change.

**Three questions you should be able to answer**
- Why would a cached user mapping of the same physical page as the kernel's HPET MMIO return a stale counter?
- Why is `accessible` 0 when `hpet_mmio_phys()` is 0, even if the vDSO page is already bound?
- Why does the counter VA live on `bind_counter_va` rather than on `AnchorReading`?
