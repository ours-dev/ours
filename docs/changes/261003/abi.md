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

## Title: A program calls ours::name; the vDSO's symbol stays ours_name (2026-10-03)

**Files**
- `build/abi/gen_syscalls.py` — the declarations it generates (`vdso_decls.inc`) are `auto <name>(...) -> ours::Status __asm__("ours_<name>");`.
- `system/public/ours/vdso.hpp` — includes them inside `namespace ours`, where it had an `extern "C"` block, and says why the name and the symbol differ.
- `system/lib/ort/src/start.cpp`, `debug.cpp`, `system/lib/launch/src/launch.cpp`, `system/service/init/init.cpp`, `kernel/lib/userabi/userboot/userboot.cpp`, `kernel/lib/syscall/test/fault_probe/fault_probe.cpp`, `examples/shell/shell.cpp` — 470 calls `ours_<name>(` → `ours::<name>(`.
- `system/public/ours/syscalls.gni`, `signals.hpp`, `system/user_executable.gni`, `system/lib/ort/include/ort/debug.hpp`, `.agents/skills/ours-kernel-design/references/layering.md` — the texts that named a call as `ours_<name>`.

**Symptom**
The system-call API a program calls was the only part of the ABI outside namespace `ours`: `ours::Status`, `ours::HandleValue` and `ours::Signals`, but `ours_channel_create`.

**Cause**
The objects:
- A symbol: the string the linker leaves undefined in a program and the loader looks up in the vDSO's `.dynsym`. The vDSO exports `ours_<name>`.
- C++ name mangling: a function declared as `ours::channel_create` refers, by default, to the symbol `_ZN4ours14channel_create...`, which the vDSO does not export.
- `extern "C"`: no mangling, so the symbol is the function's own name; declared inside a namespace, the namespace is dropped from the symbol (`channel_create`).

The events, for a call:
1. The compiler emits a call to a symbol.
2. The linker leaves it undefined.
3. The loader binds it by that exact string.

So the declarations were `extern "C"` and outside any namespace: the only standard way to have a call refer to `ours_<name>`.

**Change**
An asm label sets the symbol a declaration refers to, whatever its C++ name.

```cpp
// vdso_decls.inc, before (inside extern "C")
auto ours_handle_close(ours::HandleValue handle) -> ours::Status;
// after (inside namespace ours)
auto handle_close(ours::HandleValue handle) -> ours::Status __asm__("ours_handle_close");
```

The vDSO, its `.dynsym`, `libours.ifs` and the ABI check are untouched: the symbols are the ABI and did not change. The user chose this over generated inline forwarders (two names for every call, a generated `ou` again) and over exporting bare names (`channel_create`, which any program's C code could also define).

**Reference**
Zircon's API is C, so its names and symbols are both `zx_<name>`. The asm label is a GNU extension, which clang and GCC both honour; `<ours/vdso.hpp>` is C++ only already.

**Verification**
- `ninja -k 0` on `out/vmo-snapshot`: no error, the vDSO ABI check included.
- `llvm-nm -D --undefined-only` on the linked programs: shell 20, init 9, userboot 8 and fault_probe 6 undefined `ours_*` symbols, and no mangled `_ZN4ours` one.
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped); the shell's groups as before (19 passed in dbg; 13 passed and 5 skipped in rel).
- No syscall name clashes with a name already in `ours::` in ring-3 code. `ort::debug_write(std::string_view)` shares a name with `ours::debug_write(char const *, usize)` in another namespace.

**Deliberately not done**
- The kernel's texts that name a call by its symbol (`ours_vma_map` and the like) stay: the symbol is still the call's name in the ABI.

**How it was found**
The user's naming convention, 2026-10-03: the generated API a program calls goes in namespace `ours`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What symbol does a call to `ours::handle_close` refer to without the asm label, and what with `extern "C"` instead?
- Why could the vDSO, its `.dynsym` and `libours.ifs` stay exactly as they were?
- What would a generated inline `ours::handle_close` that calls `ours_handle_close` cost, compared with the label?

## Title: A handle as a program holds it is ours::Uandle, a scoped enum, not an integer (2026-10-03)

**Files**
- `system/public/ours/types.hpp` — `using HandleValue = u32` and `HANDLE_INVALID` become `enum class Uandle : u32 { Invalid = 0 }`, documented as a coined word; `HANDLE_FIXED_BITS_MASK` is a `u32`.
- `system/public/ours/syscalls.gni`, `build/abi/gen_syscalls.py` — arguments typed `Uandle`; the generated kernel declarations qualify every non-primitive type (`ours::Uandle`), so a declaration means the same thing in every file that includes it.
- `kernel/object/handle_table.*`, `wait.*`, `resource_dispatcher.*`, `process_dispatcher.cpp`, `kernel/lib/syscall/src/*.cpp`, `argument.hpp` — the handlers and the handle table take and return `ours::Uandle`; `channel.cpp` and `vmo.cpp` no longer bring `object::Handle` in with a using-declaration.
- `system/public/ours/vdso.hpp`, `system/lib/ort/**`, `system/lib/launch/**`, `system/service/init/init.cpp`, `examples/shell/shell.cpp`, `kernel/lib/userabi/**`, `kernel/lib/syscall/test/fault_probe/fault_probe.cpp`, `kernel/lib/user_copy/test/user_copy_test.cpp` — the users.
- `docs/architecture/object/handle-lookup.md`, `docs/design/260923/userspace-interface.md`, `docs/design/260924/userspace-filesystem.md`, `.agents/tasks/system-abi-surface/DESIGN.md` — English notes; `.agents/skills/ours-kernel-design/references/layering.md` — the naming rule.

**Symptom**
A handle was a `u32` by another name. Anything that took a handle took any integer, and a count, a size or an option word passed where a handle was expected compiled. Arithmetic on a handle compiled too, and so did a handle where a count was expected.

**Cause**
The objects:
- The handle as a program holds it: a number naming an entry in the calling process's handle table, meaningless anywhere else.
- The handle as the kernel holds it, `object::Handle`: the table entry, a reference to an object and the rights it grants.
- The system call between them, which is given the first and resolves it to the second.

The events:
1. The number was declared `using HandleValue = u32`, an alias, which C++ treats as the same type as `u32`.
2. Every signature that took one took any `u32`.
3. Nothing but a test at run time could catch a wrong argument.

**Change**
A scoped enum with a fixed underlying type is a type of its own: no implicit conversion to or from an integer, no arithmetic. It still has `u32`'s size, alignment and calling convention, so it crosses the ABI in the same register as before.

```cpp
// before
using HandleValue = u32;
constexpr auto HANDLE_INVALID = HandleValue{0};
// after
enum class Uandle : u32 {
    Invalid = 0,
};
```

The only conversions are where a number really becomes a handle or the reverse:
- the argument register, in either direction: `from_register()`, which already truncated an enum to its underlying type, and `ProcessDispatcher::start()`'s `arg1`;
- the handle table encoding and decoding a value: `value_of()`, `lookup_locked()`.

Only two places in the whole tree did anything with a handle but pass it on: a smoke test's `handle & 1`, now a comparison with `Uandle::Invalid`, and the processargs test's literal handles, now named constants.

The name is the user's, a coined word: "u" for the user side that holds it, and "handle". The kernel's `object::Handle` is the entry it names, and a system call has both in view at once, so they are never spelled alike. The user decided to type handles strictly over keeping the alias, and chose the name over `HandleValue`, `Handle` and `UHandle`.

How it was done: a workflow.
1. One agent made the change as `ours::Handle` and built and booted it.
2. Three reviewers read the diff, one each for integer semantics, the ABI and generated code, and naming and documentation.
3. A skeptic checked each finding against the code: 12 findings, 11 confirmed, all comments or naming.
4. A last agent fixed them.
5. The rename to `Uandle` followed, by hand, driven by the compiler.

**Reference**
Zircon's `zx_handle_t` is a plain `uint32_t` typedef, so its C API cannot tell a handle from an integer. Its C++ `lib/zx` does, with classes that also own the handle. This is the middle step: typed against integers, not an owner.

**Verification**
- The type change, before the rename, was checked for being behaviour-neutral by building the tree with the change reversed and comparing the loaded sections. Both kernel images, the vDSO, userboot, init, fault_probe and the runtime smoke program were byte-identical. Only the shell differed, by seven relabelled strings and the offsets to them; its disassembly with addresses normalised was instruction for instruction the same.
- `ninja -k 0` on `out/vmo-snapshot`: no error, no new warning.
- `boot_check.py --out out/vmo-snapshot`: dbg PASS (19 passed), rel PASS (13 passed, 10 skipped). The shell's groups: 19 passed in dbg, 13 passed and 5 skipped in rel. 12 probes killed, launcher checks passed.
- Host tests: processargs 16 passed, user_copy 6 passed, with `CopyAllowed<ours::Uandle>` asserted.
- No `HandleValue`, `HANDLE_INVALID` or `ours::Handle` left in code, build files or skills.

**Deliberately not done**
- Dropping the `ours::` prefix at call sites, so that a call such as `channel_write(h, ...)` finds `ours::channel_write` by argument-dependent lookup on a `Uandle` argument. It is possible now, and left for the user to decide. Against it: calls with no `ours` argument (`debug_write`, `process_exit`, `command_list`) would still need the prefix; the prefix marks where a call enters the kernel; and a local function of the same name could be picked or made ambiguous.
- The older Chinese task plans that still spell `HandleValue` describe what they planned at the time.

**How it was found**
The user's request: rename `HandleValue`, make it an enum with an `Invalid` variant, and prefer type safety.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What does a scoped enum with a fixed underlying type forbid that an alias of `u32` allows, and what does it keep the same at the ABI?
- Where does a number legitimately become a `Uandle`, and why only there?
- Why are `Uandle` and `object::Handle` named apart, and what would argument-dependent lookup do with a `Uandle` argument?
