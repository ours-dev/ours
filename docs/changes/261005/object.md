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
None, a planned step. It was finished from an earlier attempt's uncommitted work, made on an older integration branch and rebased onto the one that brought the Sampler object: the one conflict was `kernel/object/BUILD.gn`, where both sides had added to the comment and the dependencies of `//kernel/object`, and both were kept. The review of that work found `destroy()` calling the hardware operations under L2, against its own comment and the design (Cause); a shell check that skipped on a missing IRQ root, the mistake step 9's review had corrected; the repair item the code cited but no document held; and comments that still spoke of interrupt objects on lines as to come. The review of the step found one error path no check reached, the only one that runs after the line is the object's -- the handle's value failing to copy out -- and added the shell check for it (Cause). It also found two comments that did not say what the code does: MaskPostwait's still said the line is masked as a packet is sent, while `deliver_locked()` also masks it before an unbound waiter is woken; and `interrupt_create`'s documentation answered NoResource for every number no line has, while 0xFFFFFFFF, the irq layer's INVALID_V_IRQ_NUM, answers InvArgs, which `InterruptEventDispatcher::create()`'s own comment already said. The shell checks that answer now.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must `create()` attach the observer before the handle exists, and why must an AlreadyBound from the attach drop the object without `destroy()`?
- A level line is masked from delivery to ack and an edge line is never masked: what goes wrong with each line if the rule is swapped?
- Why does `destroy()` mask and detach only after dropping L2, and what did `interruptline.test` do when it did not?
