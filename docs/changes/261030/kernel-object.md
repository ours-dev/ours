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
Zircon `RootResourceFilter` (`kernel/lib/root_resource_filter/src/mod.rs`): a deny list of RAM and of peripherals the kernel owns, checked even for the root resource. `sys_vmo_create_physical()` (`kernel/lib/syscalls/driver.cc:107`) validates with `validate_resource_mmio()`, which reaches `root_resource_filter_can_access_region()` from `validate_ranged_resource_dispatcher()` (`kernel/object/resource.rs:78`), rounding an MMIO request outward to whole pages. Its RAM comes from the physboot handoff (`root_resource_filter_ffi.cc`, `cpp_root_resource_filter_populate_finalized_ranges()`); devices are added with `root_resource_filter_add_deny_region()` (`lapic.rs:226`, `ioapic.rs:263`, `kernel/platform/debug.cc:258`). Ours is the same shape: `reserve_kernel_mmio()`, `touches_kernel_physical()`, and a copy of the RAM map where Zircon reads a handoff that stays readable. I first described Zircon as keeping RAM out through a region allocator and this as a departure, which was wrong; the allocator (`ResourceDispatcher::create`) tracks overlap and the exclusive flag, which ours does not do yet. Linux `devmem_is_allowed()` (`arch/x86/mm/init.c`) is the same idea for `/dev/mem`: RAM is refused outright. I recalled that one and did not open it for this change.

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

## vmo_create_physical gives a program a window onto a device, and a mapping of one is always populated (2026-10-30)

**Files**
- `system/public/ours/syscalls.gni` — the call `vmo_create_physical(resource, paddr, size, out)`, appended last: a call's number is its place in the table. The text of `vma_map` says a physical VMO's mapping is always populated.
- `kernel/lib/syscall/src/vmo.cpp` — the handler: the resource first, then the object, then the handle, as `vmo_create` orders them.
- `kernel/object/vmo_dispatcher.cpp`, `include/ours/object/vmo_dispatcher.hpp` — `create_physical()` now checks its arguments (whole pages, neither 0, no wrap), and `is_physical()` is new; a comment that called the function latent is corrected.
- `kernel/object/vma_dispatcher.cpp` — `map()` adds `Populate` for a physical VMO whatever the options say.
- `system/service/init/test/routing_checks/routing_checks.cpp` — the end-to-end check: RAM and the local APIC refused, the argument errors, then the host bridge's configuration space mapped and its vendor id read.
- `docs/repairs/261030/kernel-object.md`, `.agents/tasks/driver-capabilities/DESIGN.md`, `PLAN.md` — what this step leaves, and the text corrected after its review.

**Symptom**
None; this is step 3 of `.agents/tasks/driver-capabilities/PLAN.md`. Before it nothing in user space could name a physical address: `VmoDispatcher::create_physical()` and `VmMapping::map_physical()` existed, and the first had no caller.

**Cause**
The objects:
- an MmIo resource: the capability, from step 2, that the call checks;
- the physical VMO: `mm::VmObjectPhysical`, a base and a size and no pages of its own, so nothing to free, to commit or to copy;
- the mapping: `VmMapping::map_physical()` fills the page tables from the window.

The events of `vmo_create_physical(root, 0xE0000000, 4096, &window)` and the read that follows:
1. The handler asks `validate_ranged_resource(root, MmIo, paddr, size)`: the root covers the address, and `touches_kernel_physical()` finds no RAM and no device of the kernel's in it.
2. `create_physical()` refuses what is not a window (not whole pages, a size or a base of 0, a wrap) and makes the VMO. The handle goes through the quota, its value is copied out, and only then does it enter the table.
3. `vma_map(vma, PermRead, 0, window, 0, 4096, &address)` reaches `VmaDispatcher::map()`, which adds `Populate` because the VMO is physical.
4. `map_with_vmo()` runs `VmMapping::map()`, and `map_physical()` writes the page-table entries for the window.
5. The program loads a `u16` from `address`; the load goes to the physical address, not to a page the kernel owns.

Why the populate is forced, and why that is not a convenience: `VmMapping::page_fault()` serves a missing page only for a paged VMO and answers NotSupported for any other, because a physical window has no moment at which a page could arrive. A mapping made without `MapRange` would be valid and empty, and the first access would be a fatal page fault. Making the program remember the option for one kind of VMO would be a trap, so the object that knows its kind decides. The mutation below shows the fault.

**Change**
The handler:

```cpp
auto vmo_create_physical(ours::Uandle const resource, PhysAddr const paddr, usize const size,
                         user_copy::UserOutPtr<ours::Uandle> const out) -> Status {
    if (auto const status = object::validate_ranged_resource(resource, RsrcKind::MmIo, paddr, size);
        status != Status::Ok) {
        return status;
    }
    // ... create_physical(), Handle::make(), copy out, then table.add()
}
```

and the rule in `map()`:

```cpp
if (vmo.is_physical()) {
    populate |= mm::VmMapOption::Populate;
}
```

The resource is asked before the arguments, as `vmo_replace_as_executable` asks its resource first: BadHandle, BadType and AccessDenied come ahead of every argument error. A size or base that wraps is still caught first, by the resource, which answers OutOfRange before it ever adds them.

Rejected: leaving `MapRange` to the caller, for the reason above; and making the cache attribute part of this step, which the user had deferred on the strength of a premise that turned out to be wrong (repair item 2).

**Reference**
Zircon `sys_vmo_create_physical()` (`kernel/lib/syscalls/driver.cc:107`) and `VmObjectPhysical::Create()` (`kernel/vm/vm_object_physical.cc:140`), which checks whole pages, a size of 0 and a wrap, and sets the VMO uncached. Ours does not set the cache policy yet, and refuses a base of 0, which Zircon allows.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped, 0 failed, 0 error lines), rel PASS (41 passed, 8 skipped, 0 failed, 6/6 required, 0 error lines), host-tests PASS (151 passed, 0 failed). rel's log has `routing_checks: passed`.
- The first read gave `0x0`, not Intel's `0x8086` and not the `0xFFFF` of an absent device. The image boots under edk2, which puts q35's configuration window at `0xE0000000`; the `0xB0000000` I had written is SeaBIOS's. With the right base the load returns `0x8086`.
- A mutation, undone afterwards: with `Populate` not forced, routing_checks was terminated with `type=FatalPageFault vector=page fault error_code=0x4 ... cr2=0x226000` on its first read of the window.
- clang-tidy on `vmo.cpp`, `vmo_dispatcher.cpp`, `vma_dispatcher.cpp` and `routing_checks.cpp`: no diagnostics. clang-format: clean.
- A review by three independent readers found no false grant. It found the size of the window uncapped (repair item 1), the mistaken premise about the cache attribute (item 2), a comment that named `VmMapping::fault` and not `VmMapping::page_fault()`, and a documentation gap, all corrected here or recorded.
- Not measured: real hardware; the arm and riscv kernels were not built.

**Deliberately not done**
- No uncached mapping: repair item 2, put to the user.
- No size cap or 2 MiB leaves: repair item 1.
- The ECAM base is a constant of the test, because nothing here parses the ACPI tables.
- `vmo_read` and `vmo_write` stay NotSupported on a physical VMO, and a slice or snapshot of one too.

**How it was found**
The `0x0`: the test printed the value it read, so the failure said what the window held and not only that it differed. 0 is neither Intel's id nor the all-ones of an absent function, so the window was mapped and live but not over the host bridge; the launcher in `scripts/qemu_boot.py` loads edk2 from `pflash` on q35, and edk2's configuration window is at `0xE0000000`. The mutation was made to check a claim in a comment, not a bug.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why does `VmaDispatcher::map()` force `Populate` for a physical VMO, and what exactly happens to a program that does not get it?
2. The resource is checked before the arguments. What does that order mean for a base that wraps the address space?
3. Why did the first read return 0 and not 0xFFFF, and what does that tell you about the window?

## A user mapping of a physical VMO is uncached, and the test reads the page-table entry (2026-10-30)

**Files**
- `kernel/object/vma_dispatcher.cpp` — `map()` adds `MmuFlags::Discache` for a physical VMO, beside the `Populate` it already forces.
- `kernel/commands/vma_physical_test.cpp`, `kernel/commands/BUILD.gn` — the console command `physvmo.test`.
- `system/public/ours/syscalls.gni` — the text of `vmo_create_physical` says the mapping is uncached.
- `docs/repairs/261030/kernel-object.md`, `.agents/tasks/driver-capabilities/DESIGN.md`, `PLAN.md` — the repair item leaves the list; the design says what was decided.

**Symptom**
Latent, cannot be triggered on QEMU, where device registers are emulated and no cache sits in front of them. On a real machine a CPU may serve a read of a status register from its cache and write a register late or merge two writes, so a driver polling through a physical mapping could read a value that is no longer true.

**Cause**
The previous entry shipped the call with write-back mappings, because the design said the page tables had no cache attribute, and so the cost looked large. That premise was wrong: `MmuFlags::Discache` exists, `arch_mmu_flags.hpp` turns it into the PCD bit, and the kernel's own mappings of the local APIC and IO-APIC pass it. The review of that step found it, and also that the reference makes a physical VMO uncached by default.

Where the rule must live is the part worth learning. The first thought was mm: `VmArea::create_mapping()` knows the VMO, so a physical object could always get `Discache`. A look at the callers of `map_phys()` showed why not: `descriptor.cpp` and `idt.cpp` map frames of ordinary RAM as physical objects (the IDT and the GDT). Mapping a RAM frame uncached would make every interrupt read its descriptor from memory with no cache, and, worse, it would leave the same frame cached under the kernel's own address and uncached under this one. x86 does not define what happens then; the manual calls it an attribute conflict. So the rule is not "physical object means device". It is "a VMO a program can hold means device", which is true only because `validate_ranged_resource()` refuses RAM: the program's physical VMOs are the ones the gate has proved are not RAM. The rule sits in `VmaDispatcher::map()`, in the layer where that proof is the context.

**Change**

```cpp
if (vmo.is_physical()) {
    populate |= mm::VmMapOption::Populate;
    mmuf |= mm::MmuFlags::Discache;
}
```

`physvmo.test` builds an address space, wraps its root region in a `VmaDispatcher` and maps two pages of a physical VMO over a page above all RAM, without `MapRange`. It reads each page-table entry back with `arch_aspace().query()` and requires the frame, readable, writable, user and Discache, and that a paged VMO mapped beside it carries no Discache.

Rejected: Discache in `VmArea::create_mapping()` for every physical object, for the aliasing reason above; and a cache policy stored on `VmObjectPhysical` and set by `create_physical()`, which is the reference's shape and the better one once a second policy (write-combining, for a framebuffer) exists, but is more than one flag today.

**Reference**
Zircon `VmObjectPhysical::Create()` (`kernel/vm/vm_object_physical.cc:140-160`): "Physical VMOs should default to uncached access", `SetMappingCachePolicy(ARCH_MMU_FLAG_UNCACHED)`; `sys_vmo_set_cache_policy()` (`kernel/lib/syscalls/vmo.rs:241`) changes it before the VMO is mapped.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped, 0 failed, 0 error lines), rel PASS (41 passed, 8 skipped, 0 failed, 6/6 required), host-tests PASS (151 passed). rel's routing_checks still reads the host bridge's vendor id through the uncached window.
- `kshell_run.py physvmo.test vmaphys.test resource.test`: `physvmo.test: ok -- 6 checks`, `vmaphys.test: ok -- 8 checks`, `resource.test: ok -- 60 checks`.
- A mutation, undone afterwards: with the `Discache` line commented out, `physvmo.test` failed on exactly the uncached check and nothing else.
- Not measured: real hardware, and what an uncached access does to a device.

**Deliberately not done**
Write-combining, and a cache policy a program can choose.

**How it was found**
By the review of the previous step, which read the page-table flags and found `Discache` where the design said there was none.

**The user's prediction**
None.

**Three questions you should be able to answer**
1. Why is "physical object" the wrong condition for uncached in mm, and what is the right one in the dispatcher?
2. What does x86 do with a frame mapped cached under one address and uncached under another?
3. How does `physvmo.test` tell a cached mapping from an uncached one, given that a program cannot see page-table entries?
