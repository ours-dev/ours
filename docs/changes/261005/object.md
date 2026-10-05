# object changes

## Title: IRQ and I/O port resources, handed to the root, and the I/O ports no resource grants (2026-10-05)

**Files**
- `kernel/object/include/ours/object/resource_dispatcher.hpp`, `kernel/object/resource_dispatcher.cpp` — `create()` mints Irq and IoPort resources, a kind's root (base 0, size 0) or a range inside its space, and answers InvArgs for a number that names no kind; `is_root()`; `get_info()`; `validate_ranged_resource()` gains a form that takes the dispatcher, treats a root as its kind's whole space, and refuses a request that touches one of the kernel's own ports with AccessDenied
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
- `examples/shell/shell.cpp`, `examples/shell/shell_tests.hpp`, `examples/shell/shell_tests.cpp` — the shell takes both; new `check_resources()`, on dev.dbg_1, skipped where the root started the shell
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
- The earlier attempt at this step let the shell skip its check whenever both handles were missing. That was too wide: on dev.dbg_1, where the kernel mints both on every boot, a missing handle is a failure, and a skip would have hidden it. The check now skips only where the root started the shell.

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
- `boot_check.py`: dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 8 skipped, 3/3 required). dev.dbg_1 prints `shell: resource checks passed`; dev prints `init: holding the IRQ and I/O port resources`, now a required line, and `shell: resource checks skipped (the root keeps them)`.
- `check_resources()` checks, on dev.dbg_1: the IRQ root reports kind Irq, base 0, size 0, name `irq`; the I/O port root kind IoPort, base 0, size 0, name `ioport`; VMEX kind System, base 1, size 1, name `vmex`; one record each; the Resource topic on a port is BadType; on a duplicate without Inspect AccessDenied.
- `kshell_run.py` on dev.dbg_1: `resource.test: ok -- 32 checks`. It covers `create()`'s refusals (an empty non-root range, ranges past each space, MmIo NotSupported, the retired kind number 3 InvArgs, a System resource of size 0); the I/O port root granting the RTC's ports and the last port, nothing past 0xFFFF and no interrupt line; for each of the seven ranges on the list, its first and last port and a request reaching into it from either side refused with AccessDenied, and the port beside it on either side granted; the whole port space at once refused; a range over the RTC granting itself and part of itself and nothing outside; a range minted over COM1 granting none of it; the interrupt line root granting line 0 and the last line, nothing past the u32 space and no port.
- The checks fail when they should. With the list's check disabled in `validate_ranged_resource()`, a temporary mutation: `resource.test: FAILED -- 3 of 31 checks` (the root grants none of the kernel's ports, the whole port space, a range over COM1), before the check for kind 3 was added. With the I/O port root left out of `write_bootstrap()`, another: dev.dbg_1 prints `shell: object_get_info(the I/O port root, Resource) = BadHandle (expected Ok)` and `shell: resource checks FAILED`, and dev fails with `no "init: holding the IRQ and I/O port resources" within 60s`, 2/3 required.
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
None, a planned step. It was finished from an earlier attempt's uncommitted work, rebased onto the integration branch's newer comment fixes, which touched none of its files; the SAMPLING resource was already beneath it and is kept. The review of that work found the two corrections under Cause, and comments that said the kernel's lines are all Permanent (the keyboard's is not; what keeps it is that its observer does not share).

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why must the kernel's own I/O ports be refused when a port is requested, while its own interrupt lines need no such list?
- A root resource has base 0 and size 0: what would the range check have answered for it had `is_root()` not been there, and what does it answer for a request for port 0x10000?
- Why does the list live in `kernel/platform/pc` rather than beside `resource_dispatcher.cpp`, and what does an ARM kernel answer when asked for it?
