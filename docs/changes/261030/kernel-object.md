# kernel-object changes

## The MmIo resource kind is real, and even its root refuses RAM and the kernel's own devices (2026-10-30)

**Files**
- `kernel/object/resource_dispatcher.cpp` — `create()` accepts `MmIo` like `Irq` and `IoPort`; `space_of(MmIo)` is `1 << mm::get_phys_addr_bits()`; `validate_ranged_resource()` answers AccessDenied when `platform::touches_kernel_physical()` says the request touches RAM or a device the kernel drives.
- `kernel/object/include/ours/object/resource_dispatcher.hpp` — the comments that said MmIo was NotSupported.
- `kernel/platform/include/ours/platform/mmio.hpp`, `kernel/platform/mmio.cpp` — new: `reserve_kernel_mmio()`, `kernel_mmio()`, `kernel_ram()`, `touches_kernel_physical()`, and the init hook `snapshot_ram()` that copies the memory manager's map of usable RAM into static storage.
- `kernel/platform/pc/interrupt.cpp`, `kernel/platform/pc/hpet.cpp` — record the local APIC, every IO-APIC and the HPET as devices the kernel drives.
- `kernel/platform/BUILD.gn`, `kernel/kernel_executable.gni` — `mmio.cpp` is the one source every platform shares; it had no home in the link, so the kernel executable now depends on `//kernel/platform`.
- `kernel/commands/resource_test.cpp` — the MmIo cases (60 checks in all).
- `.agents/tasks/driver-capabilities/DESIGN.md`, `PLAN.md` — the RAM source and the two failed attempts.

**Symptom**
Latent, cannot be triggered today: `ResourceDispatcher::create(MmIo)` answered NotSupported, so no resource of that kind existed and no syscall could take one. The change exists because the userspace virtio-blk driver needs to map a device's registers, and the first thing that must be true before any syscall may map a physical address is that the resource gating it can never name RAM.

**Cause**
Why the gate sits on the root and not on a narrower resource: the root MmIo resource covers the whole physical address space (`1 << phys_addr_bits`), and the root is what `ResourceServer` hands to a component, as a duplicate. Memory and devices share that one address space, so the root covers the kernel's own pages and every other process's too. A program that could map a physical page of RAM could read or rewrite anything. I/O ports already work this way: the root IoPort resource covers every port, and `kernel_ioports()` is a deny list checked at every use. MmIo needs the same, with two lists instead of one.

The first version of the RAM half was wrong in a way that passed its test, which is the reason this record is long. It asked `mm::EarlyMem::visit_usable()` at validation time. That map is the memory manager's own bookkeeping from boot, and it lives in `INIT_DATA`: `EarlyMem::BOOTMEM`, `KERNEL_BOOTMEM`, `USABLE_STORAGE` and `RESERVATION_STORAGE` are all in `.init.data`. `labour_routine()` (`kernel/top/main.cpp`) calls `EarlyMem::reclaim_init_area()` and only then `userboot_init()`, and `reclaim_init_area()` hands the init sections' frames to the page allocator. After that the frames are ordinary free memory. Nothing is unmapped or poisoned, so reading the old address still works, and returns whatever the new owner wrote there.

The dangerous part is which way it fails. If the frame holding `KERNEL_BOOTMEM` is reused as a zero-filled page, `usable_count_` reads 0. The visit loop then has nothing to visit, finishes, and returns `Status::Ok`. "I looked at every RAM run and none overlaps" and "I looked at nothing" are the same answer, so `touches_ram()` returns false and `validate_ranged_resource()` grants RAM. A gate that fails open on corrupt input is worse than no gate, because the code reads as if it protected something. `resource.test` passed only because it ran seconds after boot, before any of those frames had been reused. Three independent reviewers found this from three different angles, and a fourth agent, asked to refute it, could not.

The second attempt was `boot::handoff().mem.ranges`, which is not in the init sections. It faulted: `page fault in kernel mode on user address 0x1febc008`, with `rsi=0x1febc000`, a physical address near the top of the 512 MiB of RAM. The spans inside `phys::Handoff` hold physical addresses that only phys's early identity mapping can read; `Handoff`'s own comment says the kernel "reads once during early init and either copies into its own permanent state or discards" it. The word "copies" was the instruction.

**Change**
The RAM half now keeps a copy. An init hook (`GKTL_INIT_HOOK(KernelRamSnapshot, snapshot_ram, gktl::InitLevel::Vmm)`) walks the usable map once, while it is still valid and long before the reclaim, and stores each run, grown outward to whole pages, in an ordinary static array:

```cpp
// before: the gate read the init-section map on every request
auto touches_ram(u64 const base, u64 const size) -> bool {
    auto found = false;
    auto const status = mm::EarlyMem::visit_usable(base, base + size, bootmem::ANY_NODE,
        [&](bootmem::MemoryRegion const &) { found = true; return false; });
    return found || status != Status::Ok;
}

// after: it reads its own copy, and refuses everything until the copy exists
auto touches_ram(u64 const base, u64 const size) -> bool {
    if (!KERNEL_RAM_KNOWN) {
        return true;
    }
    for (auto const &range: kernel_ram()) {
        if (overlaps(base, size, range.base, range.size)) {
            return true;
        }
    }
    return false;
}
```

The copy has 128 slots, the capacity of the map it copies, so it cannot run out. If the copy fails for any reason, `KERNEL_RAM_KNOWN` stays false and every address counts as RAM. The device table (`KERNEL_MMIO`, 32 slots) behaves the same way: past the end it records `KERNEL_MMIO_FULL` and refuses everything, where the first version panicked the boot on the ninth device (the IO-APIC count comes from firmware). The rule for both is that an error must be a refused request.

Rounding the RAM runs outward matters because the usable list is page-trimmed: a run that ends mid-page would leave the RAM part of that page "not RAM" to the gate, and a mapping covers whole pages.

The test cannot reproduce the zeroed-frame failure, so it pins the property that prevents it: both tables must lie outside `[__init_start, __init_end)`. It also holds the root's answers against a byte-by-byte model at every edge of every run, and asks the page allocator for a frame, which is RAM by definition and does not come through the copy.

**Reference**
Zircon `sys_vmo_create_physical()` (`kernel/lib/syscalls/driver.cc:107`) validates with `validate_resource_mmio()` and relies on RAM having been subtracted from the MMIO resource allocator at boot (`x86_resource_init_hook()` and `ResourceDispatcher::InitializeAllocator(ZX_RSRC_KIND_MMIO, ...)`, `kernel/platform/pc/memory.cc`). Ours keeps the deny-list-at-use shape that `IoPort` already has, so the root stays a plain range; the user confirmed that on 2026-10-30. Linux `devmem_is_allowed()` (`arch/x86/mm/init.c`) is the same idea for `/dev/mem`: RAM is refused outright. I recalled that one and did not open it for this change.

**Verification**
- `ninja` build of `out/bringup.x86`: ok.
- `boot_check.py`: dbg PASS (44 passed, 4 skipped, 0 failed, 0 error lines), rel PASS (41 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines), host-tests PASS (151 passed, 0 failed).
- `kshell_run.py --out out/bringup.x86 resource.test`: `ok -- 60 checks`, with 8 runs of RAM and 3 devices: `[0xfee00000, 0xfee01000)` (local APIC), `[0xfec00000, 0xfec01000)` (IO-APIC), `[0xfed00000, 0xfed01000)` (HPET), which are the q35 addresses.
- clang-tidy on `mmio.cpp`, `resource_dispatcher.cpp`, `resource_test.cpp`, `pc/hpet.cpp`, `pc/interrupt.cpp`: no diagnostic on a line this change wrote. One older one remains, `resource_dispatcher.hpp:98`, the destructor without `override`. clang-format: clean.
- Not measured: the arm and riscv kernels were not built, so the shared `//kernel/platform` source set is unchecked there. The zeroed-frame false grant was reasoned from the code and the repo's own change records, not reproduced.

**Deliberately not done**
- No syscall takes an MmIo resource yet; `vmo_create_physical` is the next step, and with it the question of ACPI tables, which sit in memory the firmware describes: if the allocator's map counts them as RAM they are refused to a future userspace ACPI component.
- No narrowing syscall: the root still hands out the whole range minus the two lists.
- No cache attribute for the mapping, and no per-range lock: both tables are written during boot and only read afterwards.
- The `Crashlog` range is not given a check of its own; a reviewer asked and a second one showed it is always inside the usable map.

**How it was found**
The first `resource.test` run on the handoff version of the RAM check ended in `page fault in kernel mode on user address 0x1febc008 outside user_copy`, then a lockup report. `rsi=0x1febc000` and `r12=0x1febc008` were not user addresses at all: 0x1febc000 sits just under the top of 512 MiB, so it was a physical address used as a pointer. The Handoff's own comment named the cause. The init-section bug was not found by running anything. After the test passed, an adversarial review ran: three reviewers with different lenses (bypass, lifetime and concurrency, wiring and conventions) read the diff cold, then one skeptic per finding tried to refute it. The lifetime finding was reported by all three and not refuted.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does reading a map from an init section after the reclaim fail open rather than crash, and what makes "visited nothing" look the same as "no overlap"?
2. The root MmIo resource covers all physical addresses, so what keeps it from granting RAM, and why is the deny list checked at every use instead of when a resource is created?
3. Why did the first `resource.test` pass with the broken design, and what property does the test pin now that would have caught it?
