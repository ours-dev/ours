# kernel-arch-seam changes

## Title: The arch-neutral user-copy, multiprocessor and global clock-event declarations move to generic headers; x86 keeps the bodies (2026-10-29)

**Files**
- `kernel/include/ours/arch_user_copy.hpp` — new: `arch_copy_from_user()`, `arch_copy_to_user()`, the two `_capture_faults` copies, `UserCopyFault` and `UserCopyCaptureResult`, with their contracts stated without x86's SMAP details
- `kernel/include/ours/arch_mp.hpp` — new: `arch_mp_send_ipi()`, `arch_mp_send_ipi_mask()`, `arch_mp_reschedule_mask()`, `arch_mp_suspend()`, `arch_mp_send_nmi()`, `arch_cpu_halt_offline()`, `arch_deactivate_cpu()`, `arch_activate_cpu()`
- `kernel/include/ours/time/clock_event_device.hpp` — declares `time::global_clock_event()`, after the class it returns
- `kernel/arch/x86/kernel/include/ours/arch/user_copy.hpp` — deleted: everything in it was the arch-neutral interface
- `kernel/arch/x86/kernel/include/ours/arch/mp.hpp` → `ours/arch/x86/mp.hpp` — keeps only `x86_bring_aps()`, `x86_start_nonboot_cpu()` and `arch_mp_reschedule()`, which nothing outside x86 calls
- `kernel/arch/x86/kernel/include/ours/arch/time.hpp` → `ours/arch/x86/time.hpp` — keeps only `set_global_clock_event()` and the two old `hpet_init()`/`pit_init()` declarations
- `kernel/arch/x86/kernel/user_copy.cpp` — includes the generic header; the SMAP note that was on the declarations sits on the definitions now
- `kernel/arch/x86/kernel/{mp,bootstrap}.cpp`, `kernel/arch/x86/kernel/time/{time,hpet}.cpp`, `kernel/platform/pc/init.cpp` — include x86's headers at their new paths, and `mp.cpp` the generic one too
- `kernel/kernel/task/{ipi_service,cpu_hotplug,idle_thread}.cpp`, `kernel/lib/lockup_detector/src/report.cpp`, `kernel/commands/lockup.cpp` — include `<ours/arch_mp.hpp>`
- `kernel/lib/user_copy/include/user_copy/user_copy.hpp`, `kernel/lib/user_copy/src/user_copy.cpp` — include `<ours/arch_user_copy.hpp>`
- `kernel/kernel/time/tick_device.cpp`, `kernel/commands/tick_replace_test.cpp` — include `<ours/time/clock_event_device.hpp>` for `global_clock_event()` instead of x86's `ours/arch/time.hpp`
- `kernel/lib/user_copy/BUILD.gn`, `kernel/lib/lockup_detector/README.md` — name the new header

**Symptom**
Latent on x86. In the riscv probe build of vmours, 17 compiles stopped at `'ours/arch/user_copy.hpp' file not found`, 4 at `ours/arch/mp.hpp` and 2 at `ours/arch/time.hpp`. Those three headers hold no type whose layout depends on the CPU: only function declarations and two plain structs. To get past them, step 1b would have had to copy x86's declarations, comments and all, into riscv's `kernel/arch/riscv/kernel/include/ours/arch/`.

**Cause**
The objects:
- A per-arch header, `kernel/arch/<cpu>/kernel/include/ours/arch/<name>.hpp`, is for what has a different shape on each CPU: `ArchThread`'s saved registers, `ArchCpuLocal`'s register, `ArchVmAspace`'s page table. Generic code includes `<ours/arch/<name>.hpp>` and gets the current CPU's version.
- An interface declaration (`auto arch_copy_to_user(VirtAddr dst, void const *src, usize size) -> Status;`) has the same shape on every CPU. Only its body differs, and a body lives in a `.cpp`, not in a header.
- A C++ declaration is a promise the compiler checks only against the definitions and calls it sees in the same translation unit. Two architectures with their own copy of a declaration have two promises, each checked against its own definition and never against each other.

Why it was a mistake. Kept in x86's per-arch header, the declaration of an interface that generic code calls is in the wrong place twice over. First, generic code includes an x86 file to call it; on any other CPU that file does not exist, which is the riscv failure above. Second, the fix that failure invites, a riscv copy of the header, makes one contract into two texts: if riscv's copy says `-> void` where x86's says `-> Status`, or drops `[[nodiscard]]`, each architecture builds clean against its own copy, the generic caller is compiled against whichever copy its CPU's include path holds, and nothing ever compares the two. The contract's comments drift the same way, and x86's already said things that are true of x86 only: "bracketed by stac/clac", "a send whose ICR never goes idle", "an INIT, after which it waits for a startup IPI". Declared once in a generic header, there is one text, every architecture's definition is checked against it, and the comment can only say what every architecture must do.

What counts as arch-neutral here was decided by the callers, not the names. Each function moved is called from generic code today:
- `arch_copy_*`: `kernel/lib/user_copy/src/user_copy.cpp:27,38,51,62`; the two structs are part of `kernel/lib/user_copy`'s own API (`user_copy.hpp:73,79`).
- `arch_mp_send_ipi` and `_mask`, `arch_mp_reschedule_mask`, `arch_mp_suspend`: `kernel/kernel/task/ipi_service.cpp:145,155,172,176`. `arch_mp_send_nmi`: `kernel/lib/lockup_detector/src/report.cpp:379`, `kernel/commands/lockup.cpp:72`. `arch_cpu_halt_offline`, `arch_activate_cpu`: `kernel/kernel/task/idle_thread.cpp:288,333,351`. `arch_deactivate_cpu`: `kernel/kernel/task/cpu_hotplug.cpp:97`.
- `time::global_clock_event`: `kernel/kernel/time/tick_device.cpp:524`, `kernel/commands/tick_replace_test.cpp:673`.

These are also exactly the 13 symbols (4 + 8 + 1) of these headers that the riscv link will need, by the research's `llvm-nm` comparison of the x86 build.

Left in x86 on purpose:
- `arch_mp_reschedule(CpuNum)`: defined in `mp.cpp:62` but called from nowhere, not even inside x86. Declared generically it would read as something every architecture owes; it stays x86's, uncalled, as it was.
- `x86_bring_aps()`, `x86_start_nonboot_cpu()`, `set_global_clock_event()`: x86's own, called only by x86 and the PC platform.
- `hpet_init()` and `pit_init()` with no arguments: declared in the old `time.hpp` and defined nowhere (the HPET's is `hpet_init(PhysAddr)` in `ours/arch/hpet.hpp`). Left untouched, since an uncalled declaration may be a placeholder someone means to fill; worth a question, not a silent deletion.

**Change**
Before, generic code reached into x86's per-arch directory:

```cpp
// kernel/lib/user_copy/src/user_copy.cpp, kernel/kernel/task/ipi_service.cpp,
// kernel/kernel/time/tick_device.cpp
#include <ours/arch/user_copy.hpp>   // kernel/arch/x86/kernel/include/...
#include <ours/arch/mp.hpp>          // kernel/arch/x86/kernel/include/...
#include <ours/arch/time.hpp>        // kernel/arch/x86/kernel/include/...
```

After, it includes headers under `kernel/include`, which the kernel toolchain puts on every compile whatever the CPU:

```cpp
#include <ours/arch_user_copy.hpp>          // kernel/include/ours/
#include <ours/arch_mp.hpp>                 // kernel/include/ours/
#include <ours/time/clock_event_device.hpp> // global_clock_event()
```

and x86's definitions include the same headers, so the compiler checks each body against the one declaration.

The placement, and what was rejected:
- Generic declarations go under `kernel/include/ours`, where the tree keeps generic kernel headers, and follow the pattern already there: an interface the architecture implements is declared in a generic header with a note naming x86's file (`init_arch_early()` in `ours/start.hpp`, `arch_dump_exception_context()` in `ours/task/exception.hpp`).
- `global_clock_event()` joins `ours/time/clock_event_device.hpp` rather than a header of its own: it is one function of the time subsystem, returning the class that header defines, the way `exception.hpp` holds the one exception hook.
- Rejected: generic files named `ours/arch/mp.hpp` and `ours/arch/user_copy.hpp` under `kernel/include`, which would match Zircon's spelling. In this tree `ours/arch/` is the per-arch include namespace, and two directories providing the same `ours/arch/` name is exactly the silent resolution by `-I` order that the previous change (`docs/changes/261029/kernel-top.md`) removed: a riscv `ours/arch/user_copy.hpp` written later by mistake would shadow, or be shadowed by, the generic one without a word.
- Rejected: declaring the copies in `kernel/lib/user_copy`, the library that calls them. That would make x86's arch code depend on a library above it to see its own interface.
- `arch_mp.hpp` and `arch_user_copy.hpp` are named for the `arch_mp_*` and `arch_copy_*` calls they declare. Their include guards are `OURS_ARCH_MP_HPP` and `OURS_ARCH_USER_COPY_HPP`, the same as the old x86 headers' had: the old x86 `user_copy.hpp` is gone, and x86's leftovers moved to `ours/arch/x86/mp.hpp` and `ours/arch/x86/time.hpp` (guards `OURS_ARCH_X86_MP_HPP`, `OURS_ARCH_X86_TIME_HPP`), where x86 keeps its other x86-only headers (`bootstrap.hpp`, `init.hpp`, `percpu.hpp`). That also frees the names `ours/arch/mp.hpp` and `ours/arch/time.hpp`, so nothing left in x86's per-arch directory looks like part of the protocol every CPU provides.

**Reference**
Zircon declares the same interfaces once, generically, in `zircon/kernel/include/arch/user_copy.h` (`arch_copy_from_user()`, `arch_copy_to_user()`, `arch_copy_from_user_capture_faults()`, `arch_copy_to_user_capture_faults()`, `UserCopyCaptureFaultsResult`) and `zircon/kernel/include/arch/mp.h` (`arch_mp_send_ipi()`, `arch_mp_reschedule()`, `arch_mp_cpu_hotplug()`, `arch_mp_cpu_unplug()`), with x86's own MP declarations in `zircon/kernel/arch/x86/include/arch/x86/mp.h`. Zircon can share the `arch/` include namespace between the two because its per-arch protocol headers are named `arch_*.h` (`arch_ops.h`, `arch_thread.h`); this tree's per-arch protocol uses plain names, hence the different spelling here. `global_clock_event` has no Zircon counterpart; the name is Linux x86's (`global_clock_event` in `arch/x86/kernel/time.c`), where it is x86's own variable rather than a generic interface.

**Verification**
- x86 `ours-verify`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required), host-tests PASS (152 passed).
- The x86 dbg vmours defines exactly the same symbols as after the previous commit: `llvm-nm -C --defined-only`, names compared, 0 lines of difference.
- `kshell_run.py`: `usercopy.test`, `tickreplace.test` and `hotplug.test` ok, the kernel tests that drive the copies, `global_clock_event()` and the CPU on/off calls.
- riscv phys check: PASS, `handoff ok at 0.1s`.
- riscv probe: still 144 failing compiles, but none now stops at `ours/arch/user_copy.hpp` (was 17), `ours/arch/mp.hpp` (4) or `ours/arch/time.hpp` (2); those compiles get as far as `ours/arch/cpu_local.hpp` (115 now) or `ours/arch/thread.hpp` (13), headers that do have a per-arch shape.
- `gn check` on `//kernel/arch/x86/kernel/*`, `//kernel/kernel/*`, `//kernel/lib/user_copy/*`, `//kernel/lib/lockup_detector/*`, `//kernel/commands/*`, `//kernel/platform/*`: OK. clang-format clean on every changed line; clang-tidy reports nothing on a changed line (six pre-existing findings on untouched lines of `bootstrap.cpp`, `hpet.cpp` and `idle_thread.cpp`).

**Deliberately not done**
- riscv's bodies for these 13 functions: step 1b writes them, as real one-line answers where one exists (`global_clock_event()` returning null) and named placeholders otherwise.
- x86's other per-arch headers whose calls generic code makes (`ours/arch/entry.hpp`, `ours/arch/idle_thread.hpp`): not in this change's set. `entry.hpp`'s `arch_enter_uspace()` and `arch_build_uspace_irqframe()` are inline bodies that fill x86's `IrqFrame`, and `ArchIdleThread` is a type with per-arch state, so each is a per-arch header for real and riscv writes its own in step 1b.
- `hpet_init()`/`pit_init()` without arguments, defined nowhere: left as they were (see Cause).

**How it was found**
The research's riscv probe (`ninja -k 0`, 2026-10-29) grouped the failing compiles by their first missing header; reading the three smallest groups' headers showed they declare nothing per-arch.

**The user's prediction**
None

**Three questions you should be able to answer**
- Why can two architectures each carry their own copy of `arch_copy_to_user()`'s declaration with different return types and both build clean?
- What decides whether a declaration belongs in a generic header or a per-arch one: its name, or something else?
- Why is a generic `kernel/include/ours/arch/mp.hpp` riskier in this tree than Zircon's generic `kernel/include/arch/mp.h` is in Zircon's?

## Title: The arch-seam move's comments: x86's INIT and startup IPIs return to its definitions, the HPET pointer names the real function, and global_clock_event() is the platform's (2026-10-29)

**Files**
- `kernel/arch/x86/kernel/mp.cpp` — comments above `arch_deactivate_cpu()` and `arch_activate_cpu()` say what x86 does: an INIT, then two startup IPIs naming the trampoline, and `x86_start_nonboot_cpu()`
- `kernel/arch/x86/kernel/include/ours/arch/x86/time.hpp` — the comment on `set_global_clock_event()` names `hpet_init(PhysAddr)` in `ours/arch/hpet.hpp`
- `kernel/include/ours/time/clock_event_device.hpp` — `global_clock_event()` is defined by the platform's timer code, not "implemented by the architecture"
- `docs/repairs/261029/kernel-arch-seam.md` — new: item 1, the three declarations nothing uses, put to the user

**Symptom**
Latent, comments only. A reader of x86's `mp.cpp` could no longer learn how x86 takes a CPU offline and back; a reader of `x86/time.hpp` was pointed at a declared-but-undefined `ours::time::hpet_init()`; and step 1b would have been told to put riscv's `global_clock_event()` in the arch layer while its sibling `arch_tick_hz()` goes in the platform's.

**Cause**
The objects:
- A generic declaration (`ours/arch_mp.hpp`) says what every architecture's body must do; the definition's comment says how this architecture does it. The previous entry moved the declarations out of x86's header and rewrote their comments in generic terms, as it should.
- `global_clock_event()`: the one clock-event device that can interrupt several CPUs, which a CPU whose own device failed falls back on.

Why each was a mistake:
- The x86-specific sentences of the old `arch_deactivate_cpu()` and `arch_activate_cpu()` comments (an INIT, after which the CPU waits for a startup IPI; INIT then two startup IPIs; the CPU runs `x86_start_nonboot_cpu()`, which sets its hardware up again) were dropped from the generic header, correctly, but not moved to the definitions, unlike the SMAP note in `user_copy.cpp` and the ICR note above `arch_mp_send_nmi()`. The commit message said they had moved.
- `x86/time.hpp`'s comment said "hpet_init() calls it" two lines above a declaration of `ours::time::hpet_init()`, which is not that function: the caller is `ours::hpet_init(PhysAddr)`. The comment was right before the move and became ambiguous when the header shrank around it.
- `global_clock_event()` said "Implemented by the architecture". Its device on a PC is the PIT or the HPET on ISA IRQ 0, chips of the PC, not of x86: the PC platform probes the HPET (`kernel/platform/pc/hpet.cpp`), and the logz4 change in this series put the PC's COM1 on the platform seam for the same reason. A riscv board has no such device, so its answer is null; where that one line goes should follow the same rule as the console's.

**Change**
`mp.cpp`, before `arch_deactivate_cpu()`: no comment. After:

```cpp
/// On x86 the state a halted CPU is held in is INIT's: one INIT IPI resets
/// it, after which it does nothing until a startup IPI arrives, and only
/// arch_activate_cpu() sends one.
```

Before `arch_activate_cpu()`, a paragraph is added above the existing one:

```cpp
/// On x86, an INIT and then two startup IPIs, as at boot, naming the
/// real-mode trampoline prepare_bootstrap_area() fills in. The CPU runs
/// x86_start_nonboot_cpu(), which sets its hardware up again and hands it
/// back to its idle thread.
```

`x86/time.hpp`: "hpet_init() calls it" becomes "hpet_init(PhysAddr) (ours/arch/hpet.hpp) calls it".

`clock_event_device.hpp`, before:

```cpp
/// Implemented by the architecture (x86: the device on the legacy timer
/// line, ISA 0, in time/time.cpp; the PIT's until the HPET takes the line
/// over).
```

After:

```cpp
/// Defined by the platform's timer code, because such a device is the
/// board's, not the CPU's. On a PC it is the device on the legacy timer line,
/// ISA 0 (the PIT's until the HPET takes the line over), whose code lives in
/// kernel/arch/x86/kernel/time/time.cpp. A platform with no such device
/// answers null.
```

The declaration stays in `ours/time/clock_event_device.hpp`; moving it to `ours/platform/timer.hpp` beside the tick rate was considered and left, since its callers are the time subsystem's (`kernel/kernel/time/tick_device.cpp` and the `tick_replace` test command) and the type it returns is declared there. x86's body stays in `time/time.cpp`, where the PC's timer code lives today.

The two unused declarations in `x86/time.hpp` and the uncalled `arch_mp_reschedule()` are not deleted: repair item 1 in `docs/repairs/261029/kernel-arch-seam.md` asks the user.

**Reference**
Zircon keeps the PC's PIT and HPET in its PC platform (`zircon/kernel/platform/pc/timer.cc`). Its x86 `arch_mp_*` bodies are in `zircon/kernel/arch/x86/mp.cc`, with the INIT/startup sequence in `x86_bringup_aps()` (`zircon/kernel/arch/x86/smp.cc`).

**Verification**
- x86 `ours-verify` and the riscv phys check: in the commit message; comments only.
- `git grep -n "hpet_init\|pit_init"`: `ours::hpet_init(PhysAddr)` is declared in `ours/arch/hpet.hpp` and defined in `time/hpet.cpp`; `ours::time::hpet_init()` and `pit_init()` are declared only.

**Deliberately not done**
- Deleting or renaming the three unused functions: the user's call (repair item 1).
- An `#error` in `ours/arch_mp.hpp` and `ours/arch_user_copy.hpp` for a reintroduced per-arch `ours/arch/mp.hpp` or `ours/arch/user_copy.hpp`, whose include guard would be the same `OURS_ARCH_MP_HPP`. The review suggested it. Left: such a header would not fail silently, since whichever of the two a translation unit includes second is empty and its callers stop at an undeclared identifier; and nothing in step 1b's list writes either header.

**How it was found**
Review of step 1a: the reviewer compared the old `ours/arch/mp.hpp` with the comments at the definitions, read `x86/time.hpp` against `git grep hpet_init`, and read `global_clock_event()`'s comment against the logz4 record's reason for the platform seam.

**The user's prediction**
None

**Three questions you should be able to answer**
- What belongs in the comment on a generic declaration, and what in the comment on one architecture's definition of it?
- Why is `global_clock_event()`'s device a platform's and not an architecture's, though x86's body sits under `kernel/arch/x86`?
- Why does the series ask before deleting a declared-but-undefined function, rather than deleting it?
