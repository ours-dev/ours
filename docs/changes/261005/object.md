# object changes

## Title: IRQ and I/O port resources, handed to the root, and the I/O ports no resource grants (2026-10-05)

**Files**
- `kernel/object/include/ours/object/resource_dispatcher.hpp`, `kernel/object/resource_dispatcher.cpp` — `create()` mints Irq and IoPort resources, a kind's root (base 0, size 0) or a range inside its space, and answers InvArgs for a number that names no kind; `is_root()`; `get_info()`; `validate_ranged_resource()` gains a form that takes the dispatcher, treats a root as its kind's whole space, and refuses a request that touches one of the kernel's own ports with AccessDenied; a resource's base and size are a `u64`, not a `PhysAddr`
- `kernel/platform/include/ours/platform/ioport.hpp` (new) — `platform::IoPortRange` and `platform::kernel_ioports()`, the ports the kernel drives itself
- `kernel/platform/pc/ioport.cpp` (new) — the PC's list: the PICs, the PIT, the i8042, COM1 and the reset control port, checked sorted and disjoint at compile time
- `kernel/platform/arm/ioport.cpp`, `kernel/platform/riscv/ioport.cpp` (new) — an empty list, since neither CPU has an I/O port space
- `kernel/platform/BUILD.gn`, `kernel/platform/pc/BUILD.gn`, `kernel/platform/arm/BUILD.gn`, `kernel/platform/riscv/BUILD.gn`, `kernel/object/BUILD.gn` — the new header and sources; `//kernel/object` depends on the platform's headers
- `system/public/ours/object_info.hpp` — `InfoTopic::Resource = 18` and its record, `InfoResource`, pinned at 56 bytes
- `system/public/ours/syscall/resource.hpp` — includes `<ours/types.hpp>`, which it used without including; the comment on `RsrcKind` says a kind is now exchanged, through the info record
- `system/public/ours/syscalls.gni` — `object_get_info` documents the Resource topic
- `kernel/lib/syscall/src/object.cpp` — `object_get_info()` answers the Resource topic, for a handle with Inspect
- `kernel/lib/userabi/src/resource.hpp`, `kernel/lib/userabi/src/resource.cpp` — `make_root_resource_handle()`, sharing one body with `make_system_resource_handle()`
- `kernel/lib/userabi/src/userboot.cpp` — `write_bootstrap()` mints the IRQ and I/O port roots after SAMPLING and sends them as `HandleType::IrqResource` and `IoportResource`; the arrays grow from 8 to 10
- `kernel/lib/userabi/userboot/userboot.cpp` — takes both, says so when either is missing, and hands them to the next program; `start_next()` takes the handles as one `HandedHandles` instead of six arguments, and its arrays grow from 7 to 9
- `system/service/init/init.cpp` — the root takes both and logs `init: holding the IRQ and I/O port resources`
- `examples/shell/shell.cpp`, `examples/shell/shell_tests.hpp`, `examples/shell/shell_tests.cpp` — the shell takes both; new `check_resources()`: on dev.dbg_1 the two roots and the Resource topic, on dev that neither root reached the shell
- `kernel/commands/resource_test.cpp` (new), `kernel/commands/BUILD.gn` — `resource.test`
- `.agents/skills/ours-verify/scripts/boot_check.py` — dev requires the root's line
- `.agents/tasks/microkernel-interrupts/PLAN.md` — step 9's two departures recorded, and step 12 told to take the i8042's ports off the table

**Symptom**
None, a planned change: step 9 of `.agents/tasks/microkernel-interrupts/PLAN.md`. Before it no Irq or IoPort resource could exist: `ResourceDispatcher::create()` answered NotSupported for every kind but System, the ABI named `HandleType::IrqResource` and `IoportResource` and nothing sent them, and the first process was handed VMEX, DEBUG and SAMPLING only. Step 10's `ioports_request` and step 11's physical `interrupt_create` each need a resource to check, and step 13's root needs one to hand out.

**Cause**
The objects:
- A resource (`ResourceDispatcher`) is a capability over a run of numbers of one kind: `kind`, `base` and `size`, fixed at creation. Interrupt lines are one number space (a u32, the GSI after step 7), I/O ports another (a u16), System slots a third.
- A kind's root is the resource with base 0 and size 0. It stands for the kind's whole space, not for an empty range.
- `validate_ranged_resource()` is the one question every system call that takes a resource asks: does this resource grant `[base, base + size)` of this kind? Today `debug_read` and `vmo_replace_as_executable` ask it, for the DEBUG and VMEX slots.
- `platform::kernel_ioports()` is the list of I/O ports the kernel drives itself.
- `write_bootstrap()` builds the first process's startup handles; user-space userboot hands them on; the root (init) on dev, or the shell on dev.dbg_1, holds them.

The events, as step 10's `ioports_request` will run them for a driver holding the I/O port root:
1. The driver asks for COM1, `ioports_request(root, 0x3F8, 8)`.
2. The kernel asks `validate_ranged_resource(root, IoPort, 0x3F8, 8)`. The kind matches; the root covers 0 to 0xFFFF, so the range is inside; the request touches 0x3F8-0x3FF on the kernel's list, so the answer is AccessDenied, and nothing is opened.
3. Asked for the RTC instead, `0x70, 2`, the same checks pass, and step 10 clears two bits in the process's I/O bitmap. From then on every `in` and `out` the driver makes on 0x70 and 0x71 runs on the CPU directly, without a trap.

Event 3 is why the list exists, and why it has to be checked in event 2. An I/O port has no point of use the kernel sees: once its bit is clear, the CPU executes the instruction, and the kernel never hears of it. So the only moment the kernel can keep a port is when it is asked to open it, and the only thing it can check then is the resource. A resource that covers a kernel port must therefore not grant it, and that holds for the root above all, since the root covers every port there is.

Why each port is on the list, as the comment beside each says: the PIT is one of the kernel's clock-event devices, and reprogrammed while it drives the timer line it changes or stops the tick; the PICs are masked by `platform_init_apic()` so that the IO-APIC is the one way interrupts come in, and a program could unmask them; COM1 is the console the log writes and the kernel shell and `debug_read` read, and a second user would take its input or change its line settings; 0xCF9 resets the machine; and the i8042, while `keyboard.cpp` sets the controller up and reads every scancode from its interrupt, would lose scancodes to a second reader.

Why interrupt lines get no such list (D8 (a)): a line has a point of use the kernel does see, `irq::attach`. A line the kernel's own observer is on refuses every other attach, since no kernel observer shares its line, and the PIT's is Permanent besides. So the irq layer already keeps the kernel's lines, at the moment a driver would take one, and a second list would only say the same thing twice. The root grants every line, the kernel's included (`resource.test` checks line 0), and the attach refuses the kernel's (`irq.test`, step 6, checks a second attach to the PIT's line).

Why a root has to be special-cased: the range check is written for a resource that covers `[rbase, rbase + rsize)`. Fed a root as it is, base 0 and size 0, it would read an empty range and grant nothing at all. `is_root()` names the case, and the check then measures against the kind's whole space, so a request past it (a port above 0xFFFF, a line above 0xFFFFFFFF) is still OutOfRange. The comparisons stay the overflow-free form the System check already used: base first, then the rest measured from the resource's own base.

Why the list is checked on every IoPort resource, not only the root: a non-root IoPort resource can only be minted by the kernel today, since there is no `resource_create`. Checking at use means even a range minted over COM1 grants nothing of it (`resource.test` mints one to see), so no future mint path can hand out a kernel port by mistake. The cost is one walk over seven entries per check.

Why the list lives in the platform: which ports the kernel drives is a fact of the PC, kept beside the code that drives them (`debug_console.cpp`, `pic.cpp`, `power.cpp`, `keyboard.cpp`, the PIT's driver). Writing PC port numbers into `kernel/object`, which is the same on every architecture, would put that fact in the wrong layer. The accepted design (`docs/design/260923/userspace-interface.md`, section 2.6) already had the platform supply the kernel's own ranges. arm and riscv answer an empty list, so the seam has an implementation on every platform the build names, as `debug_console_read()` has.

Why an info topic: the step's check runs in the shell, which can only see a resource through a system call. `object_get_info` with the Resource topic answers kind, flags, base, size and name, and so lets the shell check that what it was handed under `IrqResource` is the Irq root and not, say, the IoPort root. It asks for Inspect, as the Process topic does.

How the two roots travel: `write_bootstrap()` mints them for the first process only, as it mints the System slots, since the first process is where the system's authority starts. Userboot has no use for them and hands them on, keeping its promise that the next program starts with what the kernel gave it. On dev the next is the root, which keeps them to hand to drivers (step 13 routes duplicates); on dev.dbg_1 it is the shell, which checks them.

Two corrections found on the way:
- `create()` answered NotSupported for any kind but System, so a number that names no kind (3 and 4 are kept out of use) got the same answer as MmIo, a kind that exists and is not supported yet. It now answers InvArgs for such a number.
- The earlier attempt at this step let the shell skip its check whenever both handles were missing. That was too wide: on dev.dbg_1, where the kernel mints both on every boot, a missing handle is a failure, and a skip would have hidden it. The check now skips nowhere: where the root started the shell it checks that neither root came with it, as the next paragraph says.

Why the dev shell checks that it holds neither root, found at review: the rule this step sets up is that the root keeps both roots, and a component reaches them only through a route (step 13), never as a startup handle. The first version skipped on dev, printing that the root keeps them, which stated the rule without checking it: had the root's ELF runner forwarded its own startup handles, the shell would have taken the IRQ root and printed the same line. The shell already checks the same rule for VMEX on dev (`check_launcher_service()`, design I6), so the dev half of `check_resources()` now does as well: either handle being there fails the check.

Why a resource's base and size are a `u64`, found at review: `ResourceDispatcher` kept them as `PhysAddr`, the type of a physical address, and this step's new callers passed interrupt lines and ports through it: `create(&resource, rights, RsrcKind::IoPort, PhysAddr(0x70), PhysAddr(2), ...)` reads as physical address 0x70. Only an MmIo range is made of addresses; the other kinds count lines, ports and slots. `PhysAddr` is an alias of `usize`, so the compiled code is the same; what changes is that the type now says what the number is, as `validate_ranged_resource()` and `InfoResource`, which already took a `u64`, did.

Two departures from the plan, recorded there too:
- The plan left the i8042's 0x60 and 0x64 open for step 12, on the premise that the kernel writes 0x64 only to reboot. At this step `keyboard.cpp` still sets the controller up and reads every scancode from its interrupt, so both ports are the kernel's and are on the list; step 12 takes them off when it deletes that driver.
- The plan put the list beside `resource_dispatcher.cpp`; it is the platform's, for the reason above.

**Change**
`create()`, before and after (abridged):

```cpp
// before
if (kind != Kind::System) {
    return Status::NotSupported;
}
if (size != 1 || base >= RSRC_SYSTEM_COUNT) {
    return Status::InvArgs;
}

// after
switch (kind) {
    case Kind::System:
        if (size != 1 || base >= RSRC_SYSTEM_COUNT) { return Status::InvArgs; }
        break;
    case Kind::Irq:
    case Kind::IoPort: {
        auto const space = space_of(kind);               // 1 << 32 lines, 1 << 16 ports
        auto const root = base == 0 && size == 0;
        if (!root && (size == 0 || size > space || base > space - size)) { return Status::InvArgs; }
        break;
    }
    case Kind::MmIo:
        return Status::NotSupported;
    default:
        return Status::InvArgs;                          // a number that names no kind
}
```

The check, before and after (abridged):

```cpp
// before: the handle form only
if (resource->kind() != kind) { return Status::BadType; }
auto const rbase = u64(resource->base());
auto const rsize = u64(resource->size());
if (base < rbase || size > rsize || base - rbase > rsize - size) { return Status::OutOfRange; }
return Status::Ok;

// after: on the dispatcher; the handle form resolves the handle and calls this
if (resource.kind() != kind) { return Status::BadType; }
auto rbase = u64(resource.base());
auto rsize = u64(resource.size());
if (resource.is_root()) { rbase = 0; rsize = space_of(kind); }
if (base < rbase || size > rsize || base - rbase > rsize - size) { return Status::OutOfRange; }
if (kind == RsrcKind::IoPort && touches_kernel_ioports(base, size)) { return Status::AccessDenied; }
return Status::Ok;
```

The PC's list, with the reason for each entry in the code:

```cpp
constexpr auto KERNEL_IOPORTS = std::array{
    IoPortRange{.base = 0x20, .count = 2},    // master PIC
    IoPortRange{.base = 0x40, .count = 4},    // PIT
    IoPortRange{.base = 0x60, .count = 1},    // i8042 data, until the keyboard leaves the kernel
    IoPortRange{.base = 0x64, .count = 1},    // i8042 command and status, likewise
    IoPortRange{.base = 0xA0, .count = 2},    // slave PIC
    IoPortRange{.base = 0x3F8, .count = 8},   // COM1
    IoPortRange{.base = 0xCF9, .count = 1},   // reset control
};
static_assert(sorted_and_disjoint());
```

`write_bootstrap()` after SAMPLING, each a boot failure if it cannot be made, as VMEX is:

```cpp
handles[count] = make_root_resource_handle(RsrcKind::Irq, "irq");
if (!handles[count]) { return Status::NoResource; }
infos[count++] = handle_info(HandleType::IrqResource, 0);
handles[count] = make_root_resource_handle(RsrcKind::IoPort, "ioport");
if (!handles[count]) { return Status::NoResource; }
infos[count++] = handle_info(HandleType::IoportResource, 0);
```

The ABI: `InfoTopic::Resource` is 18, and `InfoResource` is `{RsrcKind kind; RsrcFlag flags; u64 base; u64 size; char name[32];}`, 56 bytes with every offset pinned. `HandleType::IrqResource` (0x51) and `IoportResource` (0x52) were already named.

A resource's range after the review, typed by what it counts:

```cpp
// before
static auto create(KernelHandle<Self> *out, Rights &rights, Kind kind, PhysAddr base,
                   PhysAddr size, Flag flags, Name const &name, Storage *storage = nullptr) -> Status;
auto base() const -> PhysAddr;

// after: a line, a port, a slot, or for MmIo an address
static auto create(KernelHandle<Self> *out, Rights &rights, Kind kind, u64 base, u64 size,
                   Flag flags, Name const &name, Storage *storage = nullptr) -> Status;
auto base() const -> u64;
```

Rejected:
- The list as a deny region set that platform code fills at init: it needs a heap-backed set, an init ordering against the first check, and a lock or a freeze, for seven ranges that are known when the kernel is built.
- Refusing the kernel's ports when a resource is minted, rather than when one is used: with no `resource_create` there is no mint to refuse, and a check at use also covers any range the kernel mints itself.
- A second list for interrupt lines: see Cause; the attach already refuses.

**Reference**
- Zircon `ResourceDispatcher::create()` and `create_locked()`, `create_ranged_root()` and `is_ranged_root()` in `zircon/kernel/object/resource_dispatcher.rs`. A ranged root there is base 0 and size 0, as here; `create_locked()` answers `ZX_ERR_INVALID_ARGS` for a kind `is_valid_kind()` refuses, which `create()` here now matches.
- Zircon `validate_ranged_resource()` and `validate_ranged_resource_dispatcher()` in `zircon/kernel/object/resource.rs`: a ranged root of the asked kind is checked against the filter alone and answers `ZX_ERR_ACCESS_DENIED` for a denied region; another kind is `ZX_ERR_WRONG_TYPE` (BadType here); a range outside is `ZX_ERR_OUT_OF_RANGE`. Three departures: Zircon checks the filter only for a root, since every other resource was cut from a root under the same check, where this kernel checks every IoPort resource, having no `resource_create`; a root there grants any range the filter allows, where here it grants its kind's space and answers OutOfRange past it; and the dispatcher form here is an overload of the handle form rather than a second name.
- Zircon `RootResourceFilter::add_deny_region()` and `is_region_allowed()`, `root_resource_filter_add_deny_region()` and `root_resource_filter_can_access_region()` in `zircon/kernel/lib/root_resource_filter/src/mod.rs`: a region allocator per kind (MMIO, and I/O ports on x86), filled by platform code during boot. On x86 Zircon denies only the debug UART's ports, added in `zircon/kernel/platform/debug.cc`; the PIT, the PICs, 0xCF9 and the i8042 here come from this kernel's accepted design (`docs/design/260923/userspace-interface.md`, section 2.6) and from what this kernel's own drivers still drive.
- Zircon `get_resource_handle()` in `zircon/kernel/lib/userabi/resource.cc`, called from `userboot.cc` for the system capability message, mints each root with `ResourceDispatcher::CreateRangedRoot()`; it names the I/O port root "io_port", where this kernel says "ioport".
- Zircon `ResourceDispatcher::get_info()` and `ZX_INFO_RESOURCE` (18), `zx_info_resource_t` in `zircon/system/public/zircon/syscalls/object.h`, answered through `OB_GET_INFO` in `zircon/kernel/lib/syscalls/object_info_ffi.cc` with `ZX_RIGHT_INSPECT`. The processargs types are `PA_IRQ_RESOURCE` (0x51) and `PA_IOPORT_RESOURCE` (0x52) in `zircon/system/public/zircon/processargs.h`.

**Verification**
- `boot_check.py`: dbg PASS (31 passed, 2 skipped), rel PASS (28 passed, 7 skipped, 3/3 required). dev.dbg_1 prints `shell: resource checks passed`; dev prints `init: holding the IRQ and I/O port resources`, now a required line, and `shell: resource checks passed (the root kept both roots)`.
- `check_resources()` checks, on dev.dbg_1: the IRQ root reports kind Irq, base 0, size 0, name `irq`; the I/O port root kind IoPort, base 0, size 0, name `ioport`; VMEX kind System, base 1, size 1, name `vmex`; one record each; the Resource topic on a port is BadType; on a duplicate without Inspect AccessDenied. On dev, that neither root reached the shell.
- `kshell_run.py` on dev.dbg_1: `resource.test: ok -- 32 checks`. It covers `create()`'s refusals (an empty non-root range, ranges past each space, MmIo NotSupported, the retired kind number 3 InvArgs, a System resource of size 0); the I/O port root granting the RTC's ports and the last port, nothing past 0xFFFF and no interrupt line; for each of the seven ranges on the list, its first and last port and a request reaching into it from either side refused with AccessDenied, and the port beside it on either side granted; the whole port space at once refused; a range over the RTC granting itself and part of itself and nothing outside; a range minted over COM1 granting none of it; the interrupt line root granting line 0 and the last line, nothing past the u32 space and no port.
- The checks fail when they should. With the list's check disabled in `validate_ranged_resource()`, a temporary mutation: `resource.test: FAILED -- 3 of 31 checks` (the root grants none of the kernel's ports, the whole port space, a range over COM1), before the check for kind 3 was added. With the I/O port root left out of `write_bootstrap()`, another: dev.dbg_1 prints `shell: object_get_info(the I/O port root, Resource) = BadHandle (expected Ok)` and `shell: resource checks FAILED`, and dev fails with `no "init: holding the IRQ and I/O port resources" within 60s`, 2/3 required. At review, a third, on dev: with the shell taking its root region in the IRQ root's place, it prints `shell: holds the IRQ or I/O port root the root keeps` and `shell: resource checks FAILED`.
- clang-tidy on every changed kernel and user source: nothing on the lines this change wrote. `scripts/format_cpp.py --check` and `scripts/format_gn.py --check`: clean. The arm and riscv lists are in no build this tree makes; each compiles with `-fsyntax-only` under the x86 kernel's flags.
- Not tested: a system call that asks with an Irq or IoPort resource, since none exists before steps 10 and 11; the list on any platform but the PC.

**Deliberately not done**
- Exclusive sub-resources and a range allocator per kind (D5 (b), D8 (b)): with one driver, the root hands out duplicates of the whole root (step 13).
- MMIO resources and their deny list (RAM, the kernel image, the APICs' and the HPET's registers): nothing maps device memory into user space yet.
- Handing the roots on from the root: step 13's built-ins `ours.resource.Irq` and `ours.resource.Ioport`.
- Taking the i8042's ports off the list: step 12, with the kernel's keyboard driver.
- Keeping COM1's line, ISA 4: no kernel observer is on it, since the console polls, so from step 11 a driver holding the IRQ root can attach to it. Nothing comes of that while COM1's ports stay refused: the boot leaves the UART's interrupt enable register at 0, and only those ports can change it. A COM1 receive interrupt, when the kernel takes one, attaches at boot, before any process exists.
- The SAMPLING slot: userboot already hands it on; neither init nor the shell takes it, as before.

**How it was found**
None, a planned step. It was finished from an earlier attempt's uncommitted work, rebased onto the integration branch's newer comment fixes, which touched none of its files; the SAMPLING resource was already beneath it and is kept. The review of that work found the two corrections under Cause, and comments that said the kernel's lines are all Permanent (the keyboard's is not; what keeps it is that its observer does not share). The review of the step turned the dev shell's skip into the check under Cause, and typed a resource's range as a `u64`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must the kernel's own I/O ports be refused when a port is requested, while its own interrupt lines need no such list?
- A root resource has base 0 and size 0: what would the range check have answered for it had `is_root()` not been there, and what does it answer for a request for port 0x10000?
- Why does the list live in `kernel/platform/pc` rather than beside `resource_dispatcher.cpp`, and what does an ARM kernel answer when asked for it?

## Title: Interrupt objects for physical lines: InterruptEventDispatcher and interrupt_create's physical form (2026-10-05)

**Files**
- `kernel/object/include/ours/object/interrupt_event_dispatcher.hpp`, `kernel/object/interrupt_event_dispatcher.cpp` (new) — `InterruptEventDispatcher`, an interrupt object on a line: `create()` remaps, reads or sets the mode, attaches the object's own observer and unmasks, in that order, and hands the object out only then; the four hardware operations over `irq::mask()`, `irq::unmask()` and `irq::detach()`, with deactivation empty
- `kernel/object/include/ours/object/interrupt_dispatcher.hpp`, `kernel/object/interrupt_dispatcher.cpp` — `handle_interrupt()`, the entry a line's handler calls in interrupt context; `deliver_locked()`, the delivery `trigger()` and `handle_interrupt()` share, which masks a level line before it wakes an unbound waiter, as MaskPostwait's comment now says; comments that described physical lines as still to come
- `kernel/lib/syscall/src/interrupt.cpp` — `interrupt_create` without Virtual checks an Irq resource for `[line, line + 1)` and makes an `InterruptEventDispatcher`, where it answered NotSupported
- `system/public/ours/interrupt.hpp` — `RemapIrq` (0x1) and the mode field (`ModeDefault`, `ModeEdgeLow`, `ModeEdgeHigh`, `ModeLevelLow`, `ModeLevelHigh`, `ModeMask` = 0xE), at the bits the header had kept for them
- `system/public/ours/syscalls.gni` — `interrupt_create` documents the physical form, InvArgs for line 0xFFFFFFFF among its answers; `interrupt_wait` says that the wait which acks unmasks a masked line
- `kernel/include/ours/irq/mod.hpp`, `irq_chip.hpp`, `irq_object.hpp`, `irq_dispatcher.hpp`, `kernel/kernel/irq/mod.cpp`, `irq_object.cpp`, `irq_dispatcher.cpp` — `irq::trigger_mode()`, how a line signals now, read from its controller through a new chip operation, under the controller's lock and without the line's
- `kernel/arch/x86/apic/io_apic.cpp`, `kernel/arch/x86/lib/arch/include/arch/x86/apic/ioapic.hpp` — the IO-APIC chip's `trigger_mode` operation, which reads the pin's redirection entry under LI
- `kernel/include/ours/irq/irq_observer.hpp` — the comment says an interrupt object on a line embeds its observer, now that one does
- `kernel/platform/include/ours/platform/isa.hpp`, `kernel/platform/pc/interrupt.cpp` — `platform::remap_isa_irq()`, the line an ISA IRQ arrives on: on the PC its GSI, through the MADT's overrides
- `kernel/platform/arm/isa.cpp`, `kernel/platform/riscv/isa.cpp` (new), `kernel/platform/arm/BUILD.gn`, `kernel/platform/riscv/BUILD.gn` — the same seam where there are no ISA interrupts: the number comes back unchanged
- `kernel/object/BUILD.gn` — the new source and header; the comment on `//kernel/platform:headers` names `<ours/platform/isa.hpp>` beside the sampler's dependency
- `kernel/commands/interrupt_line_test.cpp` (new), `kernel/commands/BUILD.gn` — `interruptline.test`, delivery and teardown on ISA 3 from the kernel shell
- `kernel/commands/irq_test.cpp` — `irq.test` reads back the modes `configure()` sets, and the PIT's
- `examples/shell/shell_tests.cpp` — `check_interrupt_lines()`, the physical checks on dev.dbg_1; `check_interrupt()` takes whether the root started the shell, and skips the physical checks only then
- `docs/repairs/261005/object.md` (new) — two creates racing on one line's mode; no test sees a level line's mask
- `.agents/tasks/microkernel-interrupts/PLAN.md` — step 11's revisions: the remap seam, the default mode read back, and `interruptline.test`

**Symptom**
None, a planned change: step 11 of `.agents/tasks/microkernel-interrupts/PLAN.md`. Before it an interrupt object could only be virtual: `interrupt_create` without Virtual answered NotSupported, so no program could own an interrupt line, and step 12's PS/2 driver had nothing to wait on. Every layer under the object was ready: lines named by GSI (step 7), a lock per line with a detach that waits for the handler (step 6), the IO-APIC behind its chip and one register lock (step 8), ports posted from interrupt context (steps 3 and 4), and the IRQ root (step 9).

**Cause**
The objects:
- The interrupt object, `InterruptEventDispatcher`, the base's state machine (Idle, Waiting, Triggered, NeedAck, Destroyed) under its lock, L2, plus the line it is on, `m_line`, and its own `irq::IrqObserver`, whose context is the object.
- The line, `irq::IrqObject`, with its lock, L1: dispatch holds it while the handlers run, and `irq::detach()` takes it, so a detach returns only once no handler of the line is running.
- The IO-APIC's redirection entry for the line: its mask bit, and its trigger mode, edge or level. The chip reaches it under LI.
- The local APIC's end of interrupt, sent by `faults.cpp` as soon as the handler returns.

The events, for a level-triggered line bound to a port:
1. `interrupt_create(irq_root, 10, ModeDefault)`: the resource grants `[10, 11)`; no RemapIrq, so the line is GSI 10; the mode is read back, level, so the object gets MaskPostwait and UnmaskPrewait; it is made, its observer attached, the line unmasked, and only then is the handle made.
2. `interrupt_bind` names the port.
3. The device raises the level. The CPU takes vector 32 + 10, the irq layer takes L1 and calls the observer, which calls `handle_interrupt()`: under L2 it records the time, queues the packet on the port, masks the line (MaskPostwait) and sets NeedAck.
4. The handler returns, L1 is dropped, and the end of interrupt goes out. The level is still high, but the line is masked, so nothing fires.
5. The driver's `port_wait` takes the packet; it quiets the device and calls `interrupt_ack`, which unmasks the line under L2 and goes back to Idle.
6. The driver dies. Its last handle closes, `on_zero_handles()` calls `destroy()`: Destroyed is set under L2, then, outside L2, the line is masked and the observer detached. The next `interrupt_create` on GSI 10 succeeds.

Why the object is made, attached and unmasked before the handle exists, and why a failure on the way drops it without `destroy()`: `destroy()` masks the line by number and detaches. Before the attach has succeeded the line is not the object's, and on a line another owner holds -- a kernel driver, or a second interrupt object -- a mask by number would silence that owner's interrupts. So until the handle exists the object is held by a plain reference, which only frees it, and the handle, whose loss runs `on_zero_handles()`, is made last. An AlreadyBound from the attach therefore leaves the owner exactly as it was; the shell checks it on the PIT's line, which is Permanent, and on ISA 3 taken by a first object. The other side of the same rule is the one failure that comes after the line is the object's: the system call writes the handle's value out last, and when that write faults, the handle it then drops is the object's last, so `on_zero_handles()` runs `destroy()` and the line goes back, as on any close. The shell checks that too (found at review): a create on ISA 3 into an unmapped `out` answers InvArgs, and the next create on ISA 3 takes the line.

Why a level line is masked from delivery to ack and an edge line is not (D2):
- The end of interrupt goes out when the handler returns (event 4), long before user space has run, because until it goes out the local APIC holds back vectors of the same and lower priority (the reason DESIGN.md gives, inferred there and not checked against the manual). A level line is still asserted then, since only the driver can quiet the device. Unmasked, it would fire again at once, and again after each end of interrupt, until the driver acks: an interrupt storm on the CPU the line is routed to. So MaskPostwait masks it as the packet goes out, and UnmaskPrewait unmasks it at the ack, or, unbound, in the wait that is the ack.
- An edge line must not be masked: the IO-APIC ignores an edge that arrives on a masked pin, neither delivering it nor holding it pending (the 82093AA datasheet's mask bit). A PS/2 controller raises one edge when its output buffer fills; masked at that moment, the edge is lost, and with the buffer still full no new edge comes (DESIGN.md's inference, not checked). So an edge line stays unmasked, and interrupts that arrive before the ack only fold into the pending time: the object keeps one packet, and an edge storm cannot flood the port.

Why the default mode reads the line back: the object has to know a line's trigger mode to choose its flags, and a driver that leaves the mode to the firmware does not say it. The firmware does: q35's MADT makes ISA 5, 9, 10 and 11 level, active high, and `platform_init_apic()` programmed them so. The IO-APIC's redirection entry is the one place the mode is kept -- `configure()` writes it, and so does the platform at boot without going through the line -- so `irq::trigger_mode()` asks the chip each time rather than keeping a copy that could disagree.

Why `destroy()` masks and detaches outside L2: the handler runs under L1 and takes L2 (event 3), the order L1 → L2. `irq::detach()` takes L1 to wait for a handler in flight. Called with L2 held, it would be L2 → L1: CPU 0 in the handler holds L1 and spins on L2, the destroying thread holds L2 and spins on L1, both with interrupts off, for ever. So Destroyed is set under L2 and the lock dropped first; a handler that then gets L2 finds Destroyed and returns, and the detach, once L1 is free, takes the observer off. The earlier attempt this step was finished from had moved the three calls under L2, below the comment saying they run outside it; the review put them back, and `interruptline.test` shows what the other order does (Verification).

Why the remap goes through the platform: `kernel/object` is the same on every architecture, and which line an ISA IRQ arrives on is a fact of the PC's firmware tables, kept by `apic_io_isa_to_global()` beside the IO-APIC code. `platform::remap_isa_irq()` is the seam, as `platform::kernel_ioports()` was for step 9: the PC's calls `apic_io_isa_to_global()`, and checks the bound first, since that function takes a u8 and a number past the ISA lines would wrap into one of them; arm and riscv return the number as it is. The remap happens before the object is made, so the mode, the attach, the mask and the unmask all use the one number, the line's.

Why the handler always answers Handled: the observer does not share (D2), so an interrupt on the line is this object's device's, even after Destroyed is set, when the object only drops it. A line nobody is on still counts as spurious in the irq layer.

Why the shell skips the physical checks by who started it, not by whether it holds the IRQ root: on dev.dbg_1 the kernel mints the root on every boot, and a missing one is a failure, which a skip on a missing handle would hide; step 9's review made the same correction to `check_resources()`. On dev the root keeps the IRQ root, so there is no line to take.

**Change**
`interrupt_create`, before and after (abridged):

```cpp
// before
if (!(options & InterruptOptions::Virtual)) {
    return Status::NotSupported;
}

// after
if (!!(options & InterruptOptions::Virtual)) {
    status = VirtualInterruptDispatcher::create(&interrupt, &rights, options);
} else {
    status = object::validate_ranged_resource(resource, RsrcKind::Irq, line, 1);   // as given
    // then
    status = InterruptEventDispatcher::create(&interrupt, &rights, line, options);
}
```

`InterruptEventDispatcher::create()`, the order (abridged):

```cpp
auto virqnum = static_cast<VIrqNum>(line);
if (!!(options & InterruptOptions::RemapIrq)) {
    virqnum = platform::remap_isa_irq(line);          // before anything is made
}
// trigger: from the mode in |options|, or irq::trigger_mode(virqnum) for ModeDefault
if (trigger == irq::TriggerMode::Level) {
    flags |= Flags::MaskPostwait | Flags::UnmaskPrewait;
}
auto self = ustl::Arc<Self>::adopt(new (mm::GAF_KERNEL) Self(virqnum, flags));
if (mode.is_some()) {
    irq::configure(virqnum, mode->trigger, mode->polarity);   // refused on an occupied line
}
irq::attach(virqnum, self->m_observer);               // AlreadyBound: |self| just goes
irq::unmask(virqnum);
*out = KernelHandle<InterruptDispatcher>(ustl::move(self));   // last
```

The handler's entry, beside `trigger()`, both ending in the shared delivery:

```cpp
auto InterruptDispatcher::handle_interrupt() -> void {
    Guard<sync::SpinLock, IrqSaveAndPreemptDisable> const guard{m_spinlock};
    if (m_state == State::Destroyed) {
        return;
    }
    if (m_timestamp.is_none()) {
        m_timestamp = time::SteadyClock::now().time_since_epoch().count();
    }
    deliver_locked();   // bound: a packet, which masks with MaskPostwait;
                        // unbound: mask with MaskPostwait, then signal the waiter
}
```

`destroy()`, the order the review restored:

```cpp
{
    Guard<sync::SpinLock, IrqSave> const guard{m_spinlock};
    old_state = std::exchange(m_state, State::Destroyed);
}
if (old_state == State::Destroyed) {
    return Status::Ok;
}
mask_interrupt();                  // LI
deactivate_interrupt();            // empty on this controller
unregister_interrupt_handler();    // irq::detach(): L1, the barrier
```

The read-back, a chip operation like `mask`:

```cpp
// IrqChip
auto (*trigger_mode)(IrqData const &) -> TriggerMode = nullptr;

// io_apic.cpp
auto ioapic_trigger_mode(IrqData const &data) -> TriggerMode {
    auto &ioapic = owner_of(data.hirqnum);
    Guard<sync::SpinLock, IrqSave> const guard{IOAPIC_LOCK};
    ... ioapic.trigger_mode(data.hirqnum) ...   // the redirection entry's bit 15
}
```

Rejected:
- Keeping a line's trigger mode in `IrqObject`, set by `configure()`: the platform programs the boot modes without going through the line, so the copy would start wrong for every line the firmware made level.
- Remapping after the object is made, as the reference does: then the object and the controller would name the line by two numbers, as in the reference, where the handler is registered and the line masked by the number as given while it is configured and unmasked by the GSI.
- A mutex in `create()` around the mode step and the attach, to close the race of two creates on one line: it is a locking decision, put to the user in the repair item with the alternative of doing both under L1.

**Reference**
- Zircon `InterruptEventDispatcher::Create()`, `MaskInterrupt()`, `UnmaskInterrupt()`, `DeactivateInterrupt()`, `RegisterInterruptHandler()` and `UnregisterInterruptHandler()` (`zircon/kernel/object/interrupt_event_dispatcher.cc`): the order mode, construct, remap, configure, register, unmask, handle, with the comment that the KernelHandle is made only once everything has succeeded, since `on_zero_handles()` would otherwise tear down an interrupt already on the vector. Two departures: the remap comes first here, before the mode is read and before the object is made, where Zircon reads the default mode with `get_interrupt_config()` and stores the vector before remapping; and `DeactivateInterrupt()` does something only on arm64 there, so it is empty here.
- Zircon `InterruptDispatcher::InterruptHandler()`, `Destroy()` and `on_zero_handles()` (`zircon/kernel/object/interrupt_dispatcher.cc`): the handler takes the time, sends the packet or masks and signals; `Destroy()` exchanges the state under the spinlock and calls the three hardware operations after dropping it, with the comment on the deadlock against the platform's lock.
- Zircon `sys_interrupt_create()` (`zircon/kernel/lib/syscalls/driver.cc`) and `validate_resource_irq()` (`zircon/kernel/object/include/object/resource.h`): the resource is checked for `[src_num, src_num + 1)` on the number as given, before any remap, as here.
- Zircon `remap_interrupt()` and `get_interrupt_config()` (`zircon/kernel/platform/pc/interrupts.rs`), and `InterruptManager::get_interrupt_config()` (`zircon/kernel/platform/pc/interrupt_manager.rs`), which reads the IO-APIC's entry under the manager's lock: `platform::remap_isa_irq()` and `irq::trigger_mode()` here. Zircon answers a number no line has with ZX_ERR_INVALID_ARGS from `is_valid_interrupt()`; here the irq layer answers NoResource, as it has since step 5.
- Linux's `uio_pdrv_genirq` (`linux/drivers/uio/uio_pdrv_genirq.c`) masks the line on every interrupt and refuses shared lines for that reason; DESIGN.md's D2 explains why masking an edge line loses interrupts without the software pending bits Linux's genirq keeps (`linux/kernel/irq/chip.c`, `handle_edge_irq()`).

**Verification**
- `boot_check.py --out out/s11`: dbg PASS (33 passed, 2 skipped), rel PASS (29 passed, 10 skipped, 3/3 required). dev.dbg_1 prints `shell: interrupt checks passed` with the physical checks: ISA 0, the PIT's, AlreadyBound with and without a mode; ISA 3 into an unmapped `out` InvArgs; ISA 3 Ok, then AlreadyBound while taken; `interrupt_trigger` on a line BadState; destroy Ok and a wait after it Cancelled; ISA 3 Ok again while the destroyed object's handle is still open; closing that handle leaves the new owner the line (AlreadyBound); closing the owner lets it go (Ok); the I/O port root BadType; mode 5 InvArgs; line 200 NoResource; line 0xFFFFFFFF InvArgs. dev prints `shell: interrupt checks on physical lines skipped (the root keeps the IRQ root)` and `shell: interrupt checks passed`.
- `kshell_run.py --out out/s11` on dev.dbg_1: `interruptline.test: ok -- 37 checks` (200 rounds, 200 objects reached by 49587 interrupts), `irq.test: ok -- 33 checks`, `interrupt.test: ok -- 22 checks`, `port.test: ok`, `semaphore.test: ok -- 18 checks`, `resource.test: ok -- 32 checks`. `interruptline.test` raises ISA 3's vector by IPI, which comes in through the interrupt entry, the irq layer and the end of interrupt as the line's own interrupt would, and dispatches the line from a thread: delivery to a parked waiter with a time from while the interrupt came in, delivery to a port with two more interrupts folding into the packet the ack sends, destroy waking a parked waiter with Cancelled, the line handed back by destroy and by the last handle, the modes set and read back, the refusals, and 200 rounds of create and destroy while another CPU raises the line.
- The teardown check fails when it should. With the three hardware calls of `destroy()` moved under L2, as the earlier attempt had them, a temporary mutation: every check up to the teardown passes, then `interruptline.test: FAIL, no "interruptline.test: ok" or FAILED line within 90s`, the two CPUs deadlocked as Cause describes.
- So does the shell's check of a faulting `out`, added at review. With `interrupt_create` leaking the handle when the copy faults instead of dropping it, a temporary mutation: dev.dbg_1 prints `shell: interrupt_create(ISA 3, out = unmapped) = InvArgs`, then `shell: interrupt_create(ISA 3) = AlreadyBound (expected Ok)` and `shell: interrupt checks FAILED`: the leaked handle keeps the object, and the object the line.
- clang-tidy on every changed source, with the diagnostics filtered to the lines this change wrote: none. `scripts/format_cpp.py --check` and `scripts/format_gn.py --check`: clean. `kernel/platform/arm/isa.cpp` and `kernel/platform/riscv/isa.cpp` are in no build this tree makes; each compiles with `-fsyntax-only` under the x86 kernel's flags.
- Not tested: that a level line is masked from delivery to ack, since an IPI goes around the IO-APIC's mask and nothing reads the mask back (`docs/repairs/261005/object.md`, item 2); a device raising the line, which waits for step 12 on ISA 1; the lock order under lockdep, which cannot be switched on yet (`docs/repairs/260917/kernel-task-sched.md` item 6).

**Deliberately not done**
- Closing the window between a create's mode step and its attach (`docs/repairs/261005/object.md`, item 1): a locking decision, for the user.
- A read-back of a line's mask for the tests: an interface with no other caller (`docs/repairs/261005/object.md`, item 2, with a real level source as the other option).
- Which number space a sub-resource names. The resource is checked on the number as given, so with RemapIrq a resource for `[0, 1)` grants ISA 0, which is GSI 2 on q35, as in the reference. Only roots exist today, which grant every line; the question belongs to the exclusive sub-resources of D5 (b).
- Wake vectors and monotonic timestamps (`ZX_INTERRUPT_WAKE_VECTOR`, `ZX_INTERRUPT_TIMESTAMP_MONO`): there is no suspend and one clock.
- Shared lines for user interrupts (D2), MSI (D1) and `object_get_info` for an interrupt.
- The concurrency argument for L1 → L2 → {LI, L3}: skipped for this run by the user's instruction; it stays DESIGN.md's open question.
- The keyboard's line, ISA 1, stays the kernel's until step 12: its observer does not share, so a create on it answers AlreadyBound. COM1's ISA 4 has no kernel observer, as step 9 recorded, so a driver holding the IRQ root can take it.

**How it was found**
None, a planned step. It was finished from an earlier attempt's uncommitted work, made on an older integration branch and rebased onto the one that brought the Sampler object: the one conflict was `kernel/object/BUILD.gn`, where both sides had added to the comment and the dependencies of `//kernel/object`, and both were kept. The review of that work found `destroy()` calling the hardware operations under L2, against its own comment and the design (Cause); a shell check that skipped on a missing IRQ root, the mistake step 9's review had corrected; the repair item the code cited but no document held; and comments that still spoke of interrupt objects on lines as to come. The review of the step found one error path no check reached, the only one that runs after the line is the object's -- the handle's value failing to copy out -- and added the shell check for it (Cause). It also found two comments that did not say what the code does: MaskPostwait's still said the line is masked as a packet is sent, while `deliver_locked()` also masks it before an unbound waiter is woken; and `interrupt_create`'s documentation answered NoResource for every number no line has, while 0xFFFFFFFF, the irq layer's INVALID_V_IRQ_NUM, answers InvArgs, which `InterruptEventDispatcher::create()`'s own comment already said. The shell checks that answer now. And `irq::trigger_mode()` was declared to return `ustl::Result<TriggerMode, Status>` in `mod.hpp` and `ktl::Result<TriggerMode>` in `irq_dispatcher.hpp` and `irq_object.hpp`: one type, spelled two ways; it is `ktl::Result<TriggerMode>` throughout now, as the kernel's headers spell it.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must `create()` attach the observer before the handle exists, and why must an AlreadyBound from the attach drop the object without `destroy()`?
- A level line is masked from delivery to ack and an edge line is never masked: what goes wrong with each line if the rule is swapped?
- Why does `destroy()` mask and detach only after dropping L2, and what did `interruptline.test` do when it did not?

## MessagePacket keeps its bytes and handles in Vecs (2026-10-05)

**Files**
- `kernel/object/include/ours/object/message_packet.hpp`, `kernel/object/message_packet.cpp` — `ktl::Vec<u8>` for the bytes and `ktl::Vec<ustl::Box<Handle>>` for the handles, replacing a `kmalloc` buffer and an array of raw `Handle *`; the destructor is the default; two `static_assert`s hold the allocations in kmalloc

**Symptom**
The packet freed its bytes and handle array by hand, and kept handles in transit as raw pointers, turned back into `Box<Handle>` one by one to be closed or taken. Every failure path in `create()` and `assemble()` freed by hand, in the right order.

**Cause**
The handle array was a `kmalloc` buffer with no constructors run, so it could not hold `Box<Handle>`; ownership was written out instead.

**Change**
A `Vec<ustl::Box<Handle>>` owns the handles: an unread packet closes them when its vector goes, and `take_handles()` moves the boxes out. The order that keeps a handle in exactly one place is unchanged: the slots are allocated first, the packet second, and the handles moved in only once nothing can fail. If the packet's allocation fails, its constructor never runs, so both vectors are still `assemble()`'s and are freed on return.

The class promises no vmalloc, whose regions are few and whose vfree has no TLB shootdown yet. A `Vec` allocates with `new`, which goes to vmalloc above `KMALLOC_MAX_SIZE`, so the promise is now asserted at compile time rather than left to a comment: 4096 bytes and the handle slots, with the one-word count `new[]` puts in front of objects with destructors, are both at most `KMALLOC_MAX_SIZE`.

The cost: the vector's allocator value-initializes, so each message's buffer is zeroed once before the copy overwrites it, where `kmalloc` handed it over uninitialized. That is at most 4 KiB per message, on the write path. Not measured.

**Reference**
None.

**Verification**
`boot_check.py --out out/sampler`: dbg PASS (34 passed, 2 skipped), rel PASS (30 passed, 10 skipped, 3/3 required), including `shell: channel checks passed` and `shell: handle transfer checks passed`, which write, read and close bytes and handles through channels. Both `static_assert`s hold.

**Deliberately not done**
Measuring the extra zeroing, or an allocator that skips it; that is performance work, for the user to take up.

**How it was found**
The user asked for `Vec` in place of hand-written raw pointers in this file.

**The user's prediction**
None.

**Three questions you should be able to answer**
- If the packet's `new` fails, who frees the bytes and the handle slots, and why are the handles still the caller's?
- Why does the handle slots' `static_assert` add a word?
- What does a `Vec` cost a message that `kmalloc` did not?

## Title: vma_map and vma_allocate undo a call they cannot report by the object it made, not by its range (2026-10-05)

**Files**
- `kernel/mm/include/ours/mm/vm_area.hpp`, `kernel/mm/vm_area.cpp` — new `VmArea::destroy_child()`: under the address-space lock, destroys a child if it is still in this area's set, and answers BadState otherwise
- `kernel/mm/vm_mapping.cpp` — `VmMapping::destroy()` withdraws the vDSO registration when the mapping being destroyed is the registered one
- `kernel/mm/include/ours/mm/vm_aspace.hpp`, `kernel/mm/vm_aspace.cpp` — new `withdraw_vdso_locked()`; `set_vdso()` publishes the range under `m_lock`; the registration's comments say when it is withdrawn
- `kernel/object/include/ours/object/vma_dispatcher.hpp`, `kernel/object/vma_dispatcher.cpp` — `MapResult`, which `map()` now returns; new `undo_map()` and `undo_allocate()`; `allocate()` destroys its sub-region again when the sub-region's dispatcher cannot be made
- `kernel/object/process_dispatcher.cpp` — `start()`'s comment: the vDSO range can now be withdrawn
- `kernel/lib/syscall/src/vma.cpp` — `vma_allocate` and `vma_map` undo through `undo_allocate()` and `undo_map()`
- `kernel/commands/vma_undo_test.cpp` (new), `kernel/commands/BUILD.gn` — `vmaundo.test`
- `examples/shell/shell_tests.cpp` — `check_vdso_map()`: the vDSO's code and an ordinary sealed mapping, each with an address the kernel cannot write back; `check_vma_allocate()`: a child handle the kernel cannot write back, and one it cannot make, with the table filled to the program's handle quota
- `kernel/object/include/ours/object/handle.hpp` — `Handle::make()`'s comment: empty at the calling process's quota too, not only when the arena is exhausted
- `kernel/mm/include/ours/mm/vmas_flags.hpp` — `Vdso`'s comment: when the claim is cleared again
- `.agents/skills/ours-kernel-design/references/layering.md` — the undo rule, and the departure for the vDSO
- `docs/repairs/261002/object.md` — items 1, 2 and 3 removed; item 4 becomes item 1, with its line references updated
- `docs/repairs/261005/object.md` — item 6: the acceptance of the former `docs/repairs/261002/object.md` item 3, which cannot run until an allocation can be made to fail

**Symptom**
Three repair items, one mechanism.
- Item 1, reachable before this change: `vma_map(vma, PermRead | PermExecute | Sealed, 0, vdso, code, code_size, (VirtAddr *)KERNEL)` answered InvArgs, yet the vDSO's code stayed mapped, claimed and registered as the process's vDSO range. A retry with a good pointer answered AccessDenied, since the claim was taken, and the caller never learned where the vDSO was. Every sealed mapping behaved the same way: the call reported a failure and the mapping stayed, and since it is sealed, no unmap could ever remove it.
- Item 2, not observed: a failing `vma_allocate` could remove another thread's mapping or sub-region, and a failing `vma_map` another thread's mapping. This needs two threads sharing a region handle. No program can run a second thread today.
- Item 3, latent: if a sub-region's dispatcher could not be made, `vma_allocate` answered OutOfMem and the sub-region stayed reserved until the address space died. Nothing held a handle to it, and with kernel placement the caller was never told where it was. Allocation cannot fail today, so this cannot happen yet.

**Cause**
The objects:
- A region: an `mm::VmArea`, behind a `VmaDispatcher`. Several threads, or several processes, can hold handles to the same region.
- A child: the mapping (`mm::VmMapping`) or sub-region (`mm::VmArea`) that a call makes. It is linked into the region's set, a B-tree keyed by base address, under the address-space lock. From that moment its range is public. Anyone holding the region can unmap it there, or find the range free again afterwards.
- The result the call writes back to the program: an address for `vma_map`, and a handle value plus an address for `vma_allocate`. The write can fail, for example when the pointer is in the kernel half or unmapped.
- The undo, which runs when the write fails. It was `region->unmap(base, size)`: by range.
- Kernel placement: without Specific, a new child goes into the lowest room that fits. Any thread can predict the place.

The events that break an undo by range, with two threads A and B sharing the region handle:
1. A calls `vma_map(region, R, 0, vmo, 0, PAGE, bad)`. The kernel links mapping M_A at X, the lowest free room.
2. A's copy of X to `bad` fails. A has not yet reached its undo.
3. B calls `vma_unmap(region, X, PAGE)`, which destroys M_A, then `vma_map(region, R, 0, vmo2, 0, PAGE, &at)`. Its mapping M_B lands at X, since X is the lowest free room again. B is told X.
4. A's undo runs `unmap(X, PAGE)` and destroys M_B. B's call answered Ok and B holds X, but the next access B makes there faults, or reads whatever B maps at X next.

So the mistake: a range identifies the child only at the instant the child is made. Between the link (step 1) and the undo (step 4) the range can come to name something else. The undo has to name what the call made, the object itself, and destroy it only if it is still linked. `vma_unmap` (2026-10-04) made step 3 possible, and a second thread will make it concurrent. Until then it needs two processes passing one region handle around.

The identity has to be a reference, not an address or a raw pointer. Suppose A kept a raw pointer to M_A. B's unmap destroys M_A, and if nothing else holds M_A, its memory goes back to the mapping object cache. B's map can then take M_B from the same slot. A's pointer now points at M_B, and an "is it still linked?" check would answer yes about M_B. That is the same mistake one level down: an address standing in for an object. The undo therefore keeps an `Arc` to the object. While the undo can still run, the object's memory cannot be reused, so "is this object still linked?" can only be asked about the object the call made.

Item 1 is the same undo meeting a seal. `VmArea::unmap()` refuses any range that holds a sealed mapping, which is the whole point of sealing. So the undo by range of a sealed mapping did nothing, and its answer was thrown away. A seal is a promise, made to whoever knows of the mapping, that the mapping will not change. When the address cannot be written back, nobody knows of the mapping, so its maker taking it back breaks no promise. For the vDSO there was a second half. `map()` had already taken the address space's claim (`claim_vdso()`) and published the range a `syscall` is accepted from (`set_vdso()`), and no undo gave either back. The address space was left with a registered vDSO that nobody knew the address of, and with no way to map another.

The registration has to go together with the mapping, not after it. The range is read without a lock on every system call (`vdso_contains()`). If the mapping were destroyed while the range stayed published, code mapped at that place later, perhaps a program's own code containing the bytes `0F 05`, would be trusted to make system calls. That would undo the one rule the registration exists to enforce. So the registration is withdrawn inside `VmMapping::destroy()`, under the address-space lock, before the pages go. While that lock is held, nothing new can be mapped at the place, and anything mapped there after the lock is released finds the range already withdrawn.

Two details of the withdrawal:
- Only the size is zeroed; the base stays. `vdso_contains()` loads the size first and the base second. A reader that loaded the old size just before the withdrawal will then load a base, and that base must have been written together with that size. Two bases qualify: this registration's, or a later registration's, where the vDSO's code lies again with the same size, since there is one vDSO. Zeroing the base as well would let that reader pair the old size with base 0 and accept a system call from `[0, size)`.
- `set_vdso()` now publishes under `m_lock`. Before this change it published after releasing the lock, because it was the only writer. The size now has two writers, set_vdso() and the withdrawal. With both under one lock, the lock decides which writes last. Otherwise the answer would rest on an argument about which thread can reach the mapping at the time.

Item 3: `VmaDispatcher::allocate()` linked the child with `create_subvma()` and then made its dispatcher with `create()`, which allocates. On OutOfMem the system call returned the status and undid nothing, since its undo only ran after `allocate()` succeeded. The child was linked, and nobody was told of it.

**Change**
The system calls, before and after:

```cpp
// vma_map, before
auto const mapped = region->map(with_ceiling, vma_offset, *object, vmo_offset, length, "vma_map");
...
if (auto const status = mapped_addr.copy_to_user(mapped.unwrap()); status != Status::Ok) {
    auto _ = region->unmap(mapped.unwrap(), length, object::VmaDispatcher::OpChildren::No);
    return status;
}

// vma_map, after
auto mapped = region->map(with_ceiling, vma_offset, *object, vmo_offset, length, "vma_map");
...
if (auto const status = mapped_addr.copy_to_user(mapped->base()); status != Status::Ok) {
    auto _ = region->undo_map(ustl::move(mapped).unwrap());
    return status;
}

// vma_allocate, after: the child's dispatcher is kept past the move into the handle
auto made = sub.dispatcher();
auto handle = object::Handle::make(object::KernelHandle<object::Dispatcher>(ustl::move(sub)), rights);
if (!handle) {
    auto _ = region->undo_allocate(*made);   // was region->unmap(base, size, OpChildren::Yes)
    return Status::NoResource;
}
```

The object layer. `map()` returns the mapping along with its address. The mapping is private, so the system call can read the address and hand the result back, and can reach nothing in mm through it, as the layering rule requires:

```cpp
class MapResult {
  public:
    auto base() const -> VirtAddr;
  private:
    friend class VmaDispatcher;
    ustl::Arc<mm::VmMapping> m_mapping;
    VirtAddr m_base;
};
auto map(...) -> ktl::Result<MapResult>;           // was ktl::Result<VirtAddr>
auto undo_map(MapResult made) -> Status;           // m_vma->destroy_child(*made.m_mapping)
auto undo_allocate(VmaDispatcher &child) -> Status; // m_vma->destroy_child(*child.m_vma)
```

mm's identity undo. "Still linked here" is asked of the area's set: is this very object at its base in the set? A first version asked the child's own parent pointer and Active bit instead. Those are cleared together by `VmAreaOrMapping::destroy()`, but they are set before the insert that links the object, and that insert can fail (`docs/repairs/261002/mm.md`, item 2). An object whose insert failed has both, yet is in no set, and `destroy()` erases by base, so it would have erased whatever the set holds at that address. The set is the one thing that says "linked" by construction:

```cpp
auto VmArea::destroy_child(VmAreaOrMapping &child) -> Status {
    Guard guard{get_lock()};
    auto const found = m_subvmas.lower_bound(child.base());
    if (found.is_none() || *found.value() != &child) {
        return Status::BadState;
    }
    DEBUG_ASSERT(child.m_parent == this && child.is_active(), "a linked child is not ours");
    child.get_lock().assert_held();
    return child.destroy();
}
```

The comparison `*found.value() != &child` is only meaningful because the caller holds a reference: another object at the same base cannot be this object's memory reused.

The vDSO, in `VmMapping::destroy()` and `VmAspace`:

```cpp
// VmMapping::destroy(), beside the last-fault cache it already clears
if (m_aspace->m_vdso_mapping.as_ptr() == this) {
    m_aspace->withdraw_vdso_locked();
}

auto VmAspace::withdraw_vdso_locked() -> void {
    m_vdso_code_size.store(0, std::memory_order_relaxed);   // the base stays; see Cause
    m_flags &= ~Flags::Vdso;                                // the claim
    m_vdso_mapping = {};
}
```

Only the undo reaches that branch today. The mapping is sealed, so no unmap reaches it. The address-space teardown moves `m_vdso_mapping` out before it destroys anything, so the check does not match there, and the teardown leaves the range alone, as before, since no thread of a destroyed address space asks it.

Item 3, in `allocate()`:

```cpp
// before
base_out = child->base();
return create(ustl::move(child), child_out, rights_out);

// after
base_out = child->base();
if (auto const status = create(child, child_out, rights_out); Status::Ok != status) {
    auto _ = m_vma->destroy_child(*child);
    return status;
}
return Status::Ok;
```

Options considered and not taken:
- `map()` returning the `Arc<mm::VmMapping>` itself, or Zircon's `MapResult` with a public `mapping` field. Either would let the system call hold an mm object and call into it, which the layering rule forbids. The private member enforces the rule through the type instead of through discipline.
- Registering the vDSO only after the address has been written back, the alternative item 1 named. That splits the registration across two dispatcher calls, one before and one after the write-back, in the system call and in every kernel caller (the loader, userboot). It is more interface for the same result.
- Withdrawing the registration in `undo_map()` instead of `VmMapping::destroy()`. That needs a second mm entry point that destroys and withdraws under one lock. It would also leave any other path that destroys the registered mapping in a live address space publishing a range for code that is gone. In `destroy()`, the rule holds for every path.
- Renaming mm's locked `destroy()` to `destroy_locked()` and adding a public `destroy()` that takes the lock, as Zircon's `Destroy()` and `DestroyLocked()` pair works. That touches some twenty call sites and comments for a name. `destroy_child()`, on the parent, also states the identity check in its name: still a child of this area.

**Reference**
- Zircon `sys_vmar_allocate()` (`zircon/kernel/lib/syscalls/vmar.cc:65`, the deferred `vmar_dispatcher->Destroy()`) and `vmar_map_common()` (`:186`, the deferred `map_result->mapping->Destroy()`): both undo by identity, and are the counterparts of `undo_allocate()` and `undo_map()`.
- Zircon `VmAddressRegionOrMapping::Destroy()` (`zircon/kernel/vm/vm_address_region_or_mapping.cc:34`) takes the lock and answers BAD_STATE when the object is no longer ALIVE. It is the counterpart of `destroy_child()`'s check.
- Zircon `VmAddressRegionDispatcher::Map()` returns `VmAddressRegion::MapResult {mapping, base}` (`zircon/kernel/vm/include/vm/vm_address_region.h:656`, re-exported by `zircon/kernel/object/include/object/vm_address_region_dispatcher.h:37`). It is the counterpart of `MapResult`; ours keeps the mapping private, for the layering rule.
- A departure, for the vDSO. Zircon's `VmMapping::DestroyLocked()`, through `DestroyLockedObject()`, refuses to destroy the vDSO's code mapping (`zircon/kernel/vm/vm_mapping.cc:1136`: "The vDSO code mapping can never be unmapped, not even by VMAR destruction (except for process exit, of course)"). So `vmar_map_common()`'s undo of a failed copy-out leaves the vDSO mapped and claimed, and item 1 exists in Zircon too. Here the undo withdraws the registration with the mapping, as the repair item asked, so a retry is the first mapping again. `VmAspace::Destroy()` (`zircon/kernel/vm/vm_aspace.cc:312`) resets `vdso_code_mapping_` before tearing down, as `VmAspace::destroy()` here moves `m_vdso_mapping` out.
- Item 3: Zircon's `VmAddressRegionDispatcher::Allocate()` (`zircon/kernel/object/vm_address_region_dispatcher.cc:128`) has the same shape, `CreateSubVmar()` and then `Create()` with no undo between them, so there was nothing to copy.

**Verification**
- Build: `ninja -C out/bringup.x86`, clean, with the thread-safety analysis (an error in this tree) satisfied.
- `boot_check.py`, on the final build: dbg PASS (34 passed, 2 skipped, 0 failed, 0 error lines), rel PASS (30 passed, 10 skipped, 0 failed, 3/3 required). The counts are of summary lines: the new checks extend existing ones and add none. Both images print `vdso map checks passed`, `vma_map checks passed`, `vma_allocate checks passed` and `vma_unmap checks passed`. Both print these new lines: `vma_map(vdso code, RX, sealed, mapped_addr = kernel half) = InvArgs`, then `vma_map(vdso code, RX, sealed) = Ok` (item 1's acceptance), then `vma_map(vdso code) a second time = AccessDenied`; `vma_map(vdso code into s, ..., mapped_addr = kernel half) = InvArgs` and then `process_start(the vDSO's code undone, a slice mapped, no vDSO) = BadState` (the range was withdrawn as well as the claim); `vma_map(an ordinary VMO, sealed, Specific, mapped_addr = kernel half) = InvArgs` and then `... the same place) = Ok`; `vma_allocate(child = kernel half) = InvArgs` and then `vma_allocate(that spot again) = Ok`. The existing write-back checks still pass: `vma_map(Specific, mapped_addr = kernel half) = InvArgs` with the same spot Ok, and `vma_allocate(child_addr = kernel half) = InvArgs` with the same spot Ok. Across the three boot_check runs the dbg shell took 4.8 s, 34.7 s and 26.3 s. In the slow run the time went to the launcher-probe checks (+12.7 s) and the profile checks (+16.6 s), which sample until they have enough, under a host load average of about 4. Each check this change touches took about 0.01 s.
- `kshell_run.py` on dev.dbg_1: `vmaundo.test: ok -- 20 checks` on the final build; `vmaseal.test: ok -- 13 checks` and `vmawx.test: ok -- 5 checks` on the build before the set lookup in `destroy_child()` and three comment edits, which neither test reaches differently (`vmaseal.test` calls mm directly, `vmawx.test` only maps).
- `vmaundo.test` fails when it should. As a temporary mutation, `undo_map()` and `undo_allocate()` were given the old range undo (`unmap(base, size, OpChildren::No / Yes)`): `vmaundo.test: FAILED -- 6 of 20 checks`. The six were the two race checks for a mapping, the two for a sub-region, and the two sealed checks. With the identity undo restored, the rebuilt images were byte-identical (sha256) to the ones that had booted.
- clang-tidy on every changed source, with the diagnostics filtered to the lines this change wrote: none. `scripts/format_cpp.py --check` and `scripts/format_gn.py --check`: clean.
- Boots run: six. That was `boot_check` three times (both images each time: the first build, after comment edits, after the set lookup), `kshell_run` twice, and the mutation's `kshell_run` once.
- Not tested: the race with two real threads, since no program can run a second thread (`vmaundo.test` plays both parts on one thread, in the order of the events in Cause); and the branch of item 3, because no allocation fails today (`docs/repairs/260908/heap.md` item 3) and nothing can inject a failure, so item 3's acceptance ("the same request placed again lands at the same address") cannot be run. What is tested is the call that branch makes, `destroy_child()` on an unreported sub-region, through `undo_allocate()` in `vmaundo.test`; the acceptance itself is carried over as `docs/repairs/261005/object.md` item 6.
- `vma_allocate`'s NoResource path, the handle that cannot be made, was first listed here as untested, as if it needed the whole handle arena exhausted. It does not: `Handle::make()` also refuses a process at its own quota (`charge_current()`), which its comment did not say. Review added the check to `check_vma_allocate()`: the table is filled to the quota with channels, as `check_handle_quota()` fills it; `vma_allocate(at the handle quota) = NoResource`, the one answer that branch gives; one handle is closed, and `vma_allocate(that spot, a handle closed) = Ok`, so the undo freed the range.
- Review, on the build with that check and four comment fixes (`MapResult` made `final`; `destroy_child()`'s header says the caller holds a reference; `vmaundo.test`'s header says all three checks fail under the range undo, the sealed one included; the two comments in the Files list): ninja clean; `boot_check.py` dbg PASS (34 passed, 2 skipped, 0 failed), rel PASS (30 passed, 10 skipped, 0 failed, 3/3 required), both printing the two new lines and `vma_allocate checks passed`; `kshell_run.py` on dev.dbg_1: `vmaundo.test: ok -- 20 checks`, `vmaseal.test: ok -- 13 checks`, `vmawx.test: ok -- 5 checks`; clang-tidy on the changed lines: none. Two boots: one `boot_check`, one `kshell_run`.

**Deliberately not done**
- A `vma_destroy` system call. `destroy_child()` ignores seals, because only the object's maker calls it. A destroy that a program can call has to refuse sealed mappings first, as `unmap()` does, and it is a separate decision.
- `docs/repairs/261002/mm.md` item 2 (`VmArea::create_common()` drops `activate()`'s status). An object whose insert failed is Active and has its parent set, yet is not in the parent's set. `destroy_child()` asks the set, so it answers BadState for such an object and erases nothing, but the object itself stays broken: a mapping is still on its VMO's list when its last reference goes. That object cannot exist today, since no allocation fails. Keeping it from existing at all belongs to that item.
- Older documents that cite these items by number: `docs/changes/261002/object.md` cites item 4, which is now item 1, and `.agents/tasks/component-framework/RESULT.md` lists item 2 as open. Change records are append-only, and a task's result records what was true when it ended.

**How it was found**
The three items were recorded by review in `docs/repairs/261002/object.md`. Item 2's severity was raised on 2026-10-04, when `vma_unmap` made its step 3 possible (`docs/changes/261004/syscall.md`). For this change, reading Zircon's `VmMapping::DestroyLocked()` showed that Zircon's own undo leaves the vDSO's code mapped and claimed, so for item 1 its behaviour could not be copied.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Between linking a new mapping and writing its address back, what can a second thread holding the same region handle do that makes an undo by range destroy the wrong object, and why does kernel placement make that easy?
- Why does the undo keep an `Arc` to the object rather than its address or a raw pointer: what could the object's memory hold by the time the undo runs?
- When the vDSO's registration is withdrawn, why is only the size zeroed and the base left, and why must that happen under the address-space lock, before the mapping's pages go?

## Title: A create sets or reads its line's mode in the attach that takes the line, so two creates racing for one line cannot leave the winner in the loser's mode (2026-10-05)

**Files**
- `kernel/include/ours/irq/types.hpp` — `irq::LineMode`, a trigger mode and a polarity together, which `interrupt_event_dispatcher.cpp` used to keep a copy of for itself
- `kernel/include/ours/irq/mod.hpp`, `kernel/include/ours/irq/irq_dispatcher.hpp`, `kernel/include/ours/irq/irq_object.hpp`, `kernel/kernel/irq/mod.cpp`, `kernel/kernel/irq/irq_dispatcher.cpp`, `kernel/kernel/irq/irq_object.cpp` — `irq::attach(virqnum, observer, mode)`: on a line nobody is on, under one hold of L1, sets the mode asked for or reads the one the line has, leaves the line masked, links the observer and answers the trigger mode; the plain `attach()` and the new one share `IrqObject::link_locked()`; `configure()`'s comments say it is for a line nobody is about to take
- `kernel/object/include/ours/object/interrupt_dispatcher.hpp`, `kernel/object/interrupt_dispatcher.cpp` — `m_flags` is guarded by L2 instead of const, and `set_level_triggered()` adds MaskPostwait and UnmaskPrewait under it; `trigger()` reads Virtual under the lock
- `kernel/object/include/ours/object/interrupt_event_dispatcher.hpp`, `kernel/object/interrupt_event_dispatcher.cpp` — `create()` makes the object edge, attaches with the mode, and adds the level flags when the attach answers Level; the constructor takes no flags
- `kernel/commands/interrupt_line_test.cpp` — `interruptline.test` races two creates with opposite modes for ISA 3 from two CPUs, 200 rounds, and checks the winner's mode; the header's pointer to the level-mask repair item follows its renumbering
- `docs/repairs/261005/object.md` — item 1 removed; the level-mask test is item 1 now
- `docs/repairs/261004/kernel-irq.md` — item 4, the same race seen from the irq layer, removed

**Symptom**
Two callers holding an IRQ resource that create an interrupt object on one free line at the same moment, one asking for an edge and the other for a level, could leave the one that took the line holding it in the other's mode. Nothing does that today (the only resource holders are the root on dev and the shell on dev.dbg_1), so the defect was latent; the new check in `interruptline.test` reproduced it on dev.dbg_1 before the fix, in at least five of 200 rounds:

```text
interruptline.test:   round 50: the edge create took the line, which is level
interruptline.test:   round 77: the edge create took the line, which is level
interruptline.test:   round 98: the edge create took the line, which is level
interruptline.test:   round 99: the edge create took the line, which is level
interruptline.test:   round 108: the level create took the line, which is edge
interruptline.test:   200 rounds of the mode race on CPUs 1 and 2: edge took the line 112 times, level 88
interruptline.test: FAIL  and the line signals as the create that took it asked, never as the other
```

Either way round is wrong. An edge object on a level line delivers each interrupt with the line unmasked; the device still holds the level when the end of interrupt goes out, so the CPU the line is routed to takes the vector again at once, and again, until the driver has serviced the device and acked: an interrupt storm. A level object on an edge line masks the line from each interrupt until the ack, and the IO-APIC drops an edge that arrives on a masked pin, so an interrupt raised in that window is lost.

**Cause**
The objects:
- The line, `irq::IrqObject`, and its lock L1. The line's mode lives in one place only, the IO-APIC redirection entry of its pin, which the chip reads and writes under the register lock LI.
- `irq::configure()`: takes L1, refuses a line that has an observer, writes the entry, drops L1.
- `irq::trigger_mode()`: reads the entry under LI alone.
- `irq::attach()`: takes L1, refuses a line that has an exclusive observer, links the observer, drops L1.
- The interrupt object's flags: MaskPostwait and UnmaskPrewait for a level line, none for an edge one. `InterruptEventDispatcher::create()` chose them from the mode, asked for or read with `trigger_mode()`, then made the object, called `configure()` when a mode was asked for, then `attach()`, then unmasked.

The invariant all of this exists for: while an object holds a line, the line signals the way the object's flags were built for. `configure()` kept half of it, that nobody changes the mode under an owner, by refusing an occupied line. The other half, that the owner takes the line in the mode it chose its flags by, needs the mode step and the taking of the line to be one step, and they were two critical sections with L1 free in between. Each section was right on its own: no torn entry, no mode changed under an owner. The mistake was in the composition, a check-then-act across two lock holds, where the gap between them is exactly where another caller's step lands.

The concurrent actors are creators on other CPUs, in thread context. Nothing else writes the mode: the platform programs every line at boot, before any create. The handler never touches it.

The worst interleaving, creates A (edge) and B (level) on the free GSI 3:
1. A: `configure(3, Edge)` finds no observer and writes edge. A's flags are none.
2. B: `configure(3, Level)` finds no observer either, since A has not attached, and writes level. B's flags are MaskPostwait and UnmaskPrewait.
3. A: `attach()` succeeds; `unmask()` opens the line.
4. B: `attach()` answers AlreadyBound, and B's object is dropped without `destroy()`, as it must be.

A holds a level line with an edge object's flags. With ModeDefault in A the same happens: A reads edge in event 1, B writes level in event 2.

Why the window was easy to hit, which the test shows: both creators reach `configure()` at about the same time and spin on the same L1. The one that gets it first configures and drops it; the other gets it at once and configures; the first then needs L1 again for its attach, so it spins behind the second; `SpinLock` is a test-and-set lock with no order among spinners, so either may attach first. The interleaving above needs no unusual timing, only two CPUs starting together.

Why the object's flags are set after the attach, under L2: the object has to exist before the attach, since its observer is embedded in it, and the mode is only known inside the attach. So the object is made edge (no bits), and `create()` adds the level bits after the attach answers, with `set_level_triggered()`. The handler reads the flags under L2, taken inside L1, so the write is under L2 as well and there is no data race. Between the link and that write the line is masked, since the attach leaves it masked whichever way it settled the mode: the only interrupts that can reach the handler then were already on their way before the mask, or are raised by hand on the line's vector. Such an interrupt is delivered as on an edge line, without the mask, which leaves nothing wrong behind: the line is masked anyway until `create()` unmasks it, and the first interrupt after that is delivered with the mask.

Why the read path masks the line: a free line is normally masked already (the platform programs the lines masked, and a destroy masks before it detaches), but nothing in the irq layer promised it. With the mask in the attach, "the line is masked when the attach returns" holds in both paths, which is what makes the window above harmless. Masking a line nobody is on disturbs no one.

**Change**
`InterruptEventDispatcher::create()`, before and after (abridged):

```cpp
// before: the mode step and the attach are two holds of L1
auto trigger = irq::TriggerMode::Edge;
if (mode.is_some()) {
    trigger = mode->trigger;
} else {
    trigger = *irq::trigger_mode(virqnum);                    // LI only
}
auto flags = Flags::None;
if (trigger == irq::TriggerMode::Level) {
    flags |= Flags::MaskPostwait | Flags::UnmaskPrewait;
}
auto self = ustl::Arc<Self>::adopt(new (mm::GAF_KERNEL) Self(virqnum, flags));
if (mode.is_some()) {
    irq::configure(virqnum, mode->trigger, mode->polarity);   // L1, dropped
}
irq::attach(virqnum, self->m_observer);                       // L1 again
irq::unmask(virqnum);

// after: one hold of L1; the flags come from what the attach saw
auto self = ustl::Arc<Self>::adopt(new (mm::GAF_KERNEL) Self(virqnum));   // edge
auto const trigger = irq::attach(virqnum, self->m_observer, mode);       // set or read, masked
if (trigger.is_err()) {
    return trigger.unwrap_err();
}
if (*trigger == irq::TriggerMode::Level) {
    self->set_level_triggered();      // under L2: the handler may already run
}
irq::unmask(virqnum);
```

The new attach, the one hold of L1 (abridged):

```cpp
auto IrqObject::attach(IrqObserver &observer, ustl::Option<LineMode> const mode)
    -> ktl::Result<TriggerMode> {
    Guard<sync::SpinLock, IrqSave> const guard{m_lock};             // L1
    if (observer.is_attached()) { return ustl::err(Status::BadState); }
    auto const flags = m_flags.load(std::memory_order_relaxed);
    if (!!(flags & IrqFlags::Permanent) || !m_observers.empty()) {
        return ustl::err(Status::AlreadyBound);                    // before the mode is touched
    }
    auto trigger = TriggerMode::Edge;
    if (mode.is_some()) {
        chip->configure(m_data, mode->trigger, mode->polarity);    // LI inside L1; leaves it masked
        trigger = mode->trigger;
    } else {
        chip->mask(m_data);                                        // LI
        trigger = chip->trigger_mode(m_data);                      // LI
    }
    link_locked(observer, flags);
    return ustl::ok(trigger);
}
```

Nothing new is locked, and no lock order changes: L1 → LI is the order `configure()` already took. The plain `attach()` keeps its contract, and the two share `link_locked()`, so the publication of the line's flags (Permanent stored last, with release) is written once. `irq::configure()` stays for a line nobody is about to take; `irq.test` uses it.

The flags word: `Flags const m_flags` became `Flags m_flags TA_GUARDED(m_spinlock)`. Virtual still never changes after construction, but every bit of a flags word is written under one protection, so the word is L2's, and `trigger()` now reads Virtual under the lock it takes anyway.

Rejected, as the repair item laid out, and the user chose (a):
- (b) A mutex in `create()` around the mode step, the attach and the unmask. It serialises creates of interrupt objects only, and leaves the irq layer's own interface as open to the same mistake by its next caller; and the mutex would be global, since the line is the only thing two creators share.
- (c) Leaving it, and documenting that two creates on one line at once may leave the line in either mode.

Rejected while choosing how the flags reach the object:
- A callback from the attach, under L1, to set the object's flags before the observer is linked: the irq layer would call object code under its lock to serve one caller's need.
- Reading the mode before making the object and having the attach compare it to the line's, answering a mismatch for the create to retry: a compare-and-attach interface, and a retry loop in `create()`.

**Reference**
- Zircon `InterruptEventDispatcher::Create()` (`zircon/kernel/object/interrupt_event_dispatcher.cc:41`, `:100`, `:107`) calls `get_interrupt_config()` or `configure_interrupt()`, and then `RegisterInterruptHandler()`, each taking the interrupt manager's lock on its own (`zircon/kernel/platform/pc/interrupt_manager.rs`, `configure_interrupt()` at `:213`, `get_interrupt_config()` at `:241`, `register_interrupt_handler()` at `:269`): the same window, which this change departs from. Its flags are `const Flags flags_` (`zircon/kernel/object/include/object/interrupt_dispatcher.h:121`), fixed at construction from the mode before the handler is registered.
- Linux `__setup_irq()` (`linux/kernel/irq/manage.c:1471`) takes `desc->lock` (`:1588`), sets the trigger through `__irq_set_trigger()` (`:1718`) and installs the first action (`:1788`) in the one critical section: the shape followed here. In Linux the line also carries the behaviour that depends on the mode, since the chip's `irq_set_type()` picks the flow handler (`handle_level_irq()` or `handle_edge_irq()`) under the same lock; here the interrupt object carries it, in its flags, which is why they must come from what the attach saw.

**Verification**
- Before the fix, with only the new check built: `interruptline.test: FAILED -- 1 of 41 checks`, the lines quoted under Symptom. The test logs the first five wrong rounds and that run did not count the rest.
- After: `boot_check.py`: dbg PASS (34 passed, 2 skipped), rel PASS (30 passed, 10 skipped, 3/3 required); `shell: interrupt checks passed` on both.
- `kshell_run.py --out out/bringup.x86 interrupt.test interruptline.test irq.test` on dev.dbg_1: `interrupt.test: ok -- 22 checks`; `interruptline.test: ok -- 41 checks`, with `200 rounds of the mode race on CPUs 1 and 2: edge took the line 96 times, level 104; 0 left it in the other's mode, 0 had no one winner`; `irq.test: ok -- 33 checks`.
- The build has `-Wthread-safety -Werror`, so every read of the now-guarded `m_flags` is checked to hold L2. clang-tidy on every changed source, filtered to the changed lines: none. `scripts/format_cpp.py --check`: clean.
- Not observed, argued: an interrupt reaching the handler between the link and `set_level_triggered()`. The test cannot place one there.

**Deliberately not done**
- A check that a level object's flags come from the mode the attach read with ModeDefault: the flags cannot be seen from outside, and a level line's mask has no read-back (`docs/repairs/261005/object.md`, item 1).
- Removing `irq::configure()`: a line nobody is about to take may still be configured, and `irq.test` checks it.
- `.agents/tasks/microkernel-interrupts/DESIGN.md` (event 1) still describes configure, then attach, as accepted on 2026-10-04; and `.agents/tasks/microkernel-interrupts/PLAN.md` and `RESULT.md` point at the removed items and at the old number of the level-mask item. They are left for the next update of those documents, which other sessions are writing.
- The concurrency argument for this lock decision: the user asked to skip it (see below).

**How it was found**
By reading the code while finishing step 11 of `.agents/tasks/microkernel-interrupts/PLAN.md` (`docs/repairs/261005/object.md` item 1, and `docs/repairs/261004/kernel-irq.md` item 4 from the irq layer's side). This change wrote the check first and ran it on the unfixed kernel, which reproduced the race in at least five rounds of 200 (Symptom), then fixed it and ran the check again.

**The user's prediction**
None written: the user chose the recommended option (a) on 2026-10-05 and asked to skip writing the concurrency argument ("直接做").

**Three questions you should be able to answer**
- `configure()` and `attach()` were each correct under L1: which invariant did their composition break, and why can no lock held inside either one restore it?
- Why are the object's level flags written after the attach and under L2, and why is an interrupt that reaches the handler before that write harmless?
- Why does the interleaving happen readily on two CPUs that start together, given that both creators spin on the same L1?

## Title: One waiter at a time is a field under L2, not a reading of the state, so a destroy no longer leaves a second thread parked on an interrupt (2026-10-05)

**Files**
- `kernel/object/include/ours/object/interrupt_dispatcher.hpp`, `kernel/object/interrupt_dispatcher.cpp` — `m_waiter`, the thread whose wait found nothing to take, set by `begin_wait()` when it returns none and cleared by `park()` when the park returns, together with Waiting; `begin_wait()` answers BadState while it is set, after Cancelled for a destroyed object; `bind()` answers BadState on `m_waiter` instead of on Waiting; `set_before_park_hook_for_test()`, a hook `wait()` calls between `begin_wait()` and `park()`, null unless a test installs one; the comments on `wait()`, `bind()`, `destroy()` and the states
- `system/public/ours/syscalls.gni` — `interrupt_wait` says how long BadState for a second waiter lasts: from the moment the first wait finds nothing to take until it returns, so one thread at most is parked on an object
- `kernel/commands/interrupt_test.cpp` — `interrupt.test` forces the interleaving with the hook: a second wait and a bind both answer BadState after a trigger has moved the state on, the held waiter takes the interrupt and parks in its next wait, and a destroy wakes that one parked thread with Cancelled
- `docs/repairs/261004/object.md` — item 1 removed; the rest renumbered 1 to 3

**Symptom**
Two threads waiting on one interrupt at once could both end up parked on it, and `interrupt_destroy`, or closing the last handle, then woke only one of them: the other stayed parked, against `interrupt_wait`'s documented "Cancelled when the object is destroyed", until its thread was killed. A later trigger likewise woke only one. Latent: it takes two threads waiting on one interrupt, which only kernel tests do today. `interrupt.test`'s new check reproduced it on dev.dbg_1 before the fix:

```text
interrupt.test: pass  the hook holds the first wait between begin_wait() and park()
interrupt.test: pass  trigger while the first waiter is held
interrupt.test:   the second wait answered Ok
interrupt.test: FAIL  a second wait answers BadState though the trigger moved the state on
interrupt.test: FAIL  the held waiter goes on, and parks in its next wait
interrupt.test: FAILED -- 2 of 29 checks
```

The second FAIL came ten seconds later: the held waiter, released, parked in its first wait with nothing left to wake it, and only the test's destroy got it out.

**Cause**
The objects:
- `m_state` and `m_timestamp`, under L2 (`m_spinlock`). Waiting was the only record that a thread waits.
- `m_event`, an auto-unsignal event: `signal()` wakes one parked thread, or latches when none is parked, for the next `wait()` to take.
- `begin_wait()`, under L2: takes a pending interrupt and unsignals `m_event`, or answers why not, or sets Waiting and returns none; the caller then drops L2 and calls `park()`, which waits on `m_event`.
- `destroy()` sets Destroyed and signals `m_event` once.

The invariant: at most one thread is between deciding to park and leaving its park, because `m_event` hands each signal to one thread, and a destroy signals once. It was kept by reading the state: Waiting answered BadState. But the state is not only the waiter's to write. A trigger moves Waiting on to Triggered while the waiter is still on its way to park, and from then on nothing records that someone waits.

The events, threads A and B waiting on one interrupt:
1. A: `begin_wait()` finds Idle, sets Waiting, drops L2. It is not on the event's queue yet.
2. A trigger finds no one parked, latches the signal, and sets Triggered.
3. B: `begin_wait()` finds Triggered, takes the interrupt, sets NeedAck, and unsignals: the latch that would have woken A is gone.
4. B waits again: NeedAck with nothing pending, so it sets Waiting and parks.
5. A reaches `park()`, finds no latch, and parks too. Two threads are parked; the state says Waiting.
6. `destroy()` sets Destroyed and signals once. One thread wakes and answers Cancelled; the other stays parked, since nothing signals again and `on_zero_handles()` finds the object destroyed already.

Who touches these concurrently: waiting threads (`begin_wait()`, `park()`), triggers (`trigger()` from a thread, `handle_interrupt()` in interrupt context), `bind()`, `unbind()` and `ack()`, and `destroy()`, all under L2. Only the waiter itself sets or clears `m_waiter`; every other actor only reads it. That is what lets it stand through events 2 to 5 where the state could not.

The same window strands a single waiter through a bind, found while making the fix: A in event 1; the trigger of event 2; then a bind finds Triggered rather than Waiting, takes the interrupt to the port as a packet, and unsignals the event; A parks with nothing latched on an interrupt that, bound, never signals its event again, and whose destroy signals no one. `interrupt_bind`'s documentation already promised "BadState while a thread waits on it"; the code checked Waiting. In the reference the bind does not unsignal, so its waiter wakes on the latch and answers BadState; here `bind()` unsignals on purpose, so that a latch from before the bind does not wake the first wait after an unbind (`interrupt.test`'s `check_bind_takes_the_latch()`), and that is what turned the window into a stranded thread.

Why the field fixes both: `m_waiter` is set by the waiter in `begin_wait()` when it returns none and cleared by the same waiter in `park()`, both under L2, and nothing else writes it. So under L2, "a thread is between deciding to park and leaving its park" is exactly "`m_waiter` is set". A wait answers BadState while it is, and so does a bind; at most one thread is ever parked, and a destroy's single signal reaches it, or is latched for it if it has not parked yet, and its park then takes the latch and finds Destroyed.

Two details:
- `park()` clears Waiting together with `m_waiter`, on every return, not only on an early one. So Waiting stands only while `m_waiter` is set, by two lines that write them together, and `begin_wait()` treats a Waiting with no waiter as unreachable. Before, an Ok return relied on the trigger or destroy that woke it having moved the state on, which holds, but only by an argument over every signal and every latch.
- A destroyed object answers Cancelled before the waiter is looked at, so a wait that comes in after a destroy, while the woken waiter has not left yet, is told the object is gone rather than BadState.

Why `m_waiter` is cleared when the park returns, not when `wait()` does: between the two the waiter goes round its loop to `begin_wait()`, and a wait from another thread may get L2 first and take the interrupt that woke it. The woken waiter then parks again. Either way one thread at most is parked; which of the two gets the interrupt is the race two waiters have under any one-waiter rule.

Why a hook in the kernel for the test: the window is the few instructions between `begin_wait()` dropping L2 and `park()` taking the event's queue lock, and no timing from a test lands a trigger and a second wait inside it reliably. The hook is a function pointer in an atomic, null by default, read with one acquire load on the way to every park; only `interrupt.test` installs one, and only for the object it checks.

**Change**
`begin_wait()` and `park()`, before and after (abridged):

```cpp
// before: the state is the only record of a waiter
switch (m_state) {
    case State::Destroyed: return Status::Cancelled;
    case State::Triggered: /* take it, unsignal */ return Status::Ok;
    case State::NeedAck:   /* take a pending one, or unmask */ break;
    case State::Idle:      break;
    case State::Waiting:   return Status::BadState;   // overwritten by a trigger
}
m_state = State::Waiting;
return ustl::NONE;
// park(): on an early return only, Waiting -> Idle

// after: a field only the waiter writes
if (m_state == State::Destroyed) { return Status::Cancelled; }
if (m_waiter != nullptr) { return Status::BadState; }
switch (m_state) { /* Triggered, NeedAck, Idle as before; Waiting, Destroyed: unreachable() */ }
m_state = State::Waiting;
m_waiter = task::Thread::Current::get();
return ustl::NONE;

auto InterruptDispatcher::park() -> Status {
    auto const status = m_event.wait(time::Deadline::infinite());
    Guard<sync::SpinLock, IrqSave> const guard{m_spinlock};
    m_waiter = nullptr;                      // however the park ended
    if (m_state == State::Waiting) {
        m_state = State::Idle;
    }
    return status;
}
```

`bind()`:

```cpp
// before
if (m_state == State::Waiting) { return Status::BadState; }
// after
if (m_waiter != nullptr) { return Status::BadState; }
```

The test's interleaving, through the hook (`wait()` calls it between `begin_wait()` and `park()`): a thread's first wait is held there; the test triggers, then waits (BadState) and binds (BadState); it releases the thread, whose wait takes the trigger's time and whose next wait parks; a wait then answers BadState; `destroy()` wakes the thread with Cancelled.

Rejected, as the repair item laid out, and the user chose (a):
- (b) An `Event` operation that wakes every waiter, for `destroy()`: it fixes the destroy only. A trigger would still wake one of two parked threads, and the bind would still strand a single one.
- (c) Leaving it as the reference has it, documented as undefined.

Rejected while making it:
- Removing Waiting now that `m_waiter` says whether someone waits: the state machine keeps the reference's five states, and Waiting still tells Idle from "nothing pending, someone on the way to park or parked".
- Clearing `m_waiter` only when `wait()` returns: it would hold the object for the waiter across its loop, which the item did not ask for and which changes nothing about how many threads park.

**Reference**
Zircon `InterruptDispatcher::BeginWaitForInterrupt()` (`zircon/kernel/object/interrupt_dispatcher.cc:47`), whose TRIGGERED case returns `event_.Unsignal()` (`:65`) and whose WAITING answers ZX_ERR_BAD_STATE through `default` (`:108`); `DoWaitForInterruptBlock()` (`:121`); `Destroy()` (`:259`), which calls `Signal()` once on the AutounsignalEvent; and `Bind()` (`:299`), which refuses only `state_ == WAITING` (`:305`) and does not unsignal. The same two-waiter race, which this change departs from; the reference has no single-waiter strand through a bind, since its bind leaves the latch.

**Verification**
- Before the fix, with the hook and the check built: `interrupt.test: FAILED -- 2 of 29 checks`, the lines quoted under Symptom. The bind check was added after that run, so its failure before the fix is argued (Cause), not observed.
- After: `boot_check.py`: dbg PASS (34 passed, 2 skipped), rel PASS (30 passed, 10 skipped, 3/3 required); `shell: interrupt checks passed` on both.
- `kshell_run.py --out out/bringup.x86 interrupt.test interruptline.test irq.test` on dev.dbg_1: `interrupt.test: ok -- 32 checks`, among them the new ones; `interruptline.test: ok -- 41 checks`; `irq.test: ok -- 33 checks`.
- The build has `-Wthread-safety -Werror`, so every access to `m_waiter` is checked to hold L2. clang-tidy on the changed sources, filtered to the changed lines: none. `scripts/format_cpp.py --check` and `scripts/format_gn.py --check`: clean.

**Deliberately not done**
- The shell still cannot park a wait (`docs/repairs/261004/object.md`, now item 1): a program cannot start a second thread yet, so the two-thread checks stay on kernel threads.
- A hook compiled only into kernels with a kernel shell: one null check of an atomic on the way to a park does not justify a build switch.
- The concurrency argument for this lock decision: the user asked to skip it (see below).

**How it was found**
By reading the code in the review of step 2 (`docs/repairs/261004/object.md` item 1). This change wrote the check first, with the hook, and ran it on the unfixed kernel, which failed as the item predicted (Symptom). The bind's single-waiter strand was found while writing the fix, by asking of every other reader of Waiting what it meant by it: `bind()` used it to mean "a thread waits", which a trigger makes untrue.

**The user's prediction**
None written: the user chose the recommended option (a) on 2026-10-05 and asked to skip writing the concurrency argument ("直接做").

**Three questions you should be able to answer**
- Why could the state not say whether a thread waits, and what makes `m_waiter` able to, under the same lock?
- In events 1 to 5, which single operation takes away the first waiter's wakeup, and why does it take it away?
- Why did the bind strand even a single waiter here, when the reference's bind does not?
