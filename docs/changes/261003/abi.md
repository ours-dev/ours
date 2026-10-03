# abi changes

## Title: Status is [[nodiscard]] (system-abi-surface step 2, the attribute) (2026-10-03)

**Files**
- `system/public/ours/status.hpp` — `enum class [[nodiscard]] Status : u32`, with a comment saying how a deliberate drop is written.
- `.agents/tasks/system-abi-surface/DESIGN.md` — step 2 marked done.

**Symptom**
A function that answers a Status could be called as if it answered nothing, and nothing said so. The count before this step found 44 such calls, all in the kernel. Some were safe, some rested on an assumption nothing checked, and a few hid a real gap: an unimplemented preemption, an `activate()` failure, an IOAPIC EOI that is never sent.

**Cause**
The objects:
- `Status`, the one error type of both the kernel and the system-call ABI.
- A call that returns one, and the caller that ignores the answer.
- The compiler, which only warns about an ignored value when the function or its return type asks it to.

The events:
1. A function returns a Status.
2. A caller writes `f();`, because it thinks f cannot fail here, or because it does not know what to do if it does.
3. Nothing marks the difference between those two reasons, so a later reader cannot tell a considered drop from a forgotten one, and a later change to f that adds a failure reaches no caller.

**Change**

```cpp
// before
enum class Status : u32 {
// after
enum class [[nodiscard]] Status : u32 {
```

On the type, every function returning a Status by value is marked at once, including ones written later. The 44 drops were answered in the four commits before this one, module by module (mm, kernel/task, top with kshell and object, then time with arch, driver and ktl), each in one of three ways:
- `static_cast<void>(...)` with the reason, when the callee only ever answers Ok, or when an error changes nothing for this caller;
- `[[maybe_unused]] auto const status = ...; DEBUG_ASSERT(Status::Ok == status)`, when the callee can fail but not in this caller's state;
- real handling, where the drop was a bug: `VmAspace::create()` returning `init()`'s failure, `protect_locked()` keeping its first failure, `MemCache::init()` panicking, and `ClockEventDevice::enroll()` returning void.

**Reference**
Zircon's `zx_status_t` is `typedef int32_t`, a C type, and standard `[[nodiscard]]` applies to functions, classes and enumerations, not to a typedef. So Zircon marks functions one at a time, `[[nodiscard]] zx_status_t Foo()`, which covers only the functions someone remembered to mark. An enum class can carry the attribute itself, which is why this design chose the type (decision of 2026-10-02 in the design).

**Verification**
- After the attribute, every target in `out/vmo-snapshot` rebuilt with `ninja -k 0`: the kernel in both configurations, phys, the host tools and ring 3. There were 444 steps and no unused-result diagnostic; the only warnings were the known `-lto_library` linker notes.
- A snippet compiled with the kernel's own flags: a bare `f();` that drops a Status is `error: ignoring return value of type 'Status' declared with 'nodiscard' attribute [-Werror,-Wunused-value]`, and `static_cast<void>(f())` compiles.
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped).

**Deliberately not done**
- Nothing in `ou`: its entries return `ours::Status`, so the type already covers them, and the generated `ou` of step 9 will be covered the same way.
- The status codes themselves (NotFound against NoResource, NoSpace against BufferTooSmall), which are the design's open decision.
- The FIXMEs and repair items the drops pointed to: `activate()` in `docs/repairs/261002/mm.md` item 2 and `docs/repairs/260908/heap.md` item 4, `unpark_preempt()` in `docs/repairs/260917/kernel-task-sched.md` item 2, the IOAPIC EOI in `docs/repairs/261003/x86-apic.md` item 1.

**How it was found**
Step 2 of `.agents/tasks/system-abi-surface/DESIGN.md`.

**The user's prediction**
"Many, the kernel far more than ring 3." Measured: 44, all in the kernel and none in ring 3. The direction held, but there were fewer than "many".

**Three questions you should be able to answer**
- Why does putting the attribute on the type cover functions written after it, when Zircon's per-function marking does not?
- Given a dropped Status, how do you choose between `static_cast<void>`, `DEBUG_ASSERT` and handling it?
- What does a `DEBUG_ASSERT` on a callee's answer buy in a release build, and what does it not?

## Title: RsrcKind takes Zircon's values, the object-name size is published, and unused names leave types.hpp (system-abi-surface step 3, part 1) (2026-10-03)

**Files**
- `system/public/ours/syscall/resource.hpp` — `RsrcKind : u32` with Zircon's values, `Root` and `NumKinds` gone; `RsrcFlag : u32`.
- `system/public/ours/syscall.hpp` — `MAX_NAME_LEN`, the size of an object's name with its terminator.
- `kernel/object/include/ours/object/process_dispatcher.hpp`, `thread_dispatcher.hpp`, `resource_dispatcher.hpp`, `kernel/mm/include/ours/mm/vm_object.hpp`, `kernel/lib/syscall/src/process.cpp`, `task.cpp` — their four private `MAX_NAME_SIZE = 32` replaced by it.
- `system/public/ours/syscalls.gni` — process_create's and thread_create's names are "cut to MAX_NAME_LEN - 1 (31)".
- `system/public/ours/types.hpp` — `TicksSteady`, `InstantSteady`, `DurationTicksSteady`, `TicksSystem`, `InstantSystem`, `DurationTicksSystem` and `enum class Clock` deleted.

**Symptom**
Three numbers a program and the kernel have to agree on were not fixed, or not where a program could see them:
- `RsrcKind` had implicit values and no underlying type, with a `Root` kind at 3 and `System` at 4. Zircon's System is 5, and its 4 is SMC, so code ported from Zircon would ask for the wrong kind.
- The longest name an object keeps was 32 bytes in four kernel classes, each with its own constant, and in none a program could include. process_create's description said "cut to 31" as a bare number.
- `types.hpp`, the ABI's vocabulary, named seven things nothing in the tree uses, kernel or ring 3. A name in the ABI header reads as ABI.

**Cause**
The objects:
- An enum or a limit that crosses the system-call boundary, now or once a call reports it (Zircon's `zx_info_resource_t` reports a kind, and `zx_resource_create` takes one).
- The two sides that compile against it: the kernel, and every program built against one version of the header.

The events:
1. A value is written as the compiler's default (the next integer), or as a private constant of the one class that needed it first.
2. A second user appears (another dispatcher, a program, a port from Zircon) and copies or assumes the number.
3. Inserting a kind, or changing one copy, silently changes what one side means.

**Change**

```cpp
// resource.hpp, before
enum class RsrcKind { MmIo, Irq, IoPort, Root, System, NumKinds };
// after
enum class RsrcKind : u32 { MmIo = 0, Irq = 1, IoPort = 2, System = 5 };

// syscall.hpp, new
inline constexpr auto MAX_NAME_LEN = usize{32};
```

The design called it `NAME_MAX`. It is `MAX_NAME_LEN`, Zircon's spelling, because `<limits.h>` defines `NAME_MAX` as a macro (the longest file name, 255 in musl), which would replace the constant in any program that includes both.

**Reference**
Zircon: `ZX_RSRC_KIND_MMIO 0`, `IRQ 1`, `IOPORT 2`, `SMC 4`, `SYSTEM 5` (`zircon/system/public/zircon/syscalls/resource.h`), and `ZX_MAX_NAME_LEN 32` (`zircon/types.h`), which its kernel uses for every object's name, as `fbl::Name<ZX_MAX_NAME_LEN>`.

**Verification**
- `ninja -k 0` on `out/vmo-snapshot`: kernel, phys, host and ring 3 build with no error.
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped).

**Deliberately not done**
- The kernel-only names that do have users (`PhysAddr`, `CpuNum`, `INVALID_CPU_NUM`, `NnId`, `HIrqNum`, `VIrqNum`, `PgOff`, `Ticks`, `Time`) stay in `types.hpp` until the user decides where they go: part 2.
- `resource.hpp`'s GPL-2.0 header inside the MIT `system/public`, which is the copyright holder's decision, and its absence from `public_headers.gni` (step 7).

**How it was found**
Step 3 of `.agents/tasks/system-abi-surface/DESIGN.md`; the unused names by counting each name's users in every source file and in every header a ring-3 object depends on (`ninja -t deps`).

**The user's prediction**
None; the user approved the step without one.

**Three questions you should be able to answer**
- Why does an enum that crosses the system-call boundary need both written-out values and a fixed underlying type?
- What would code ported from Zircon have asked for with `ZX_RSRC_KIND_SYSTEM` against the old numbering?
- Why can a constant in a public header not be called `NAME_MAX`?
