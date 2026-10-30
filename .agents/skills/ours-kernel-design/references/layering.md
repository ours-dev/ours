# Layering and interfaces

## kernel/object is userspace's only door

- `kernel/object` holds the objects that serve users: dispatchers, handles,
  rights. Everything in `kernel/kernel` (`task::Thread`) and `kernel/mm`
  (`VmAspace`, `VmArea`, `VmMapping`) serves the kernel itself.
- `kernel/lib/userabi` (userboot bring-up) and `kernel/lib/syscall` touch
  dispatchers only: resolve the handle, check rights, call a dispatcher method.
  Never `task::`, `mm::` or an aspace pointer directly. userboot must be created
  the way a syscall would create a process on userspace's behalf, so each
  dispatcher gets its shape from a real caller.
- Rights live on handles, not on objects. Anything that will one day need
  "may this caller do this" (map, start a thread, kill a process) gets its
  entry point on the dispatcher.
- If something cannot be expressed through `kernel/object` yet, the missing
  dispatcher is the work to do. For new syscalls, first identify and plan the
  service objects they need, broadly: objects, interfaces and tests.

## Syscalls only through the vDSO

- No user program contains a `syscall` instruction; the vDSO is the only code
  that traps. The kernel side is finished only when a trap from outside the
  vDSO's code range is refused (Zircon: `VDso::ValidSyscallPC`).
- Programs bind to the vDSO by dynamic linking at load time: PIEs built with
  `//system/user_executable.gni`, relocations resolved by `elfload::Binder`
  (`system/lib/elfload`), which both loaders run: `userabi::launch()` in
  `kernel/lib/userabi/src/userboot.cpp` and `launch::start()` in
  `system/lib/launch`. Never reintroduce a hand-resolved export table.
- There is one vDSO VMO, laid out once by the kernel (`userboot_init()`, like
  Zircon's `VDso::Create()`), and it is mapped by whoever starts the process
  -- `userabi::launch()` for the one program the kernel starts, userboot,
  and the user-space launcher for every other, through `vma_map` on a vDSO
  VMO handle it is given, as Zircon's userboot and process_builder do. Since
  K25 the kernel loads no program but userboot (no `process_spawn_boot`).
  `ProcessDispatcher::create` does not map it. The user chose Zircon's shape
  over "the kernel maps it at process creation" (2026-09-28); never copy the
  vDSO per process.
- The vDSO is mapped by segment, as Zircon maps it: its data read-only, its
  code read and execute. An executable mapping of its VMO must be exactly
  its code segment, sealed, once per address space, and only that is
  registered as the range a `syscall` is accepted from
  (`VmaDispatcher::map()`, Zircon's `VDso::valid_code_mapping()`); any other
  mapping of it is read-only. The kernel reads the code segment from the
  vDSO's own ELF when it lays it out, rather than from build-time constants
  as Zircon does, because here the vDSO is a separate file of the kernel
  package that constants could disagree with; it refuses to boot with a
  vDSO that has a writable segment or not exactly one executable one. The
  vDSO is linked with `-z rodynamic` so that it has neither.
- A `vma_map` or `vma_allocate` that cannot write its result back undoes
  itself by identity: it destroys the object it made, if still linked
  (`VmaDispatcher::undo_map()`, `undo_allocate()`), never its range, which
  another holder of the region may have reused by then. The undo ignores
  seals, since nobody was told of the object, and for the vDSO's code it
  withdraws the registration too (claim and range), so a retry is the first
  mapping again. Zircon keeps the vDSO's code mapped and claimed there
  (`VmMapping::DestroyLocked()` refuses it); departed from on 2026-10-05
  (`docs/changes/261005/object.md`), because a failed call must not cost a
  process its only chance to map its vDSO.
- Adding a syscall means: an entry in `system/public/ours/syscalls.gni` and
  the body of `ours::syscall::<name>()` in `//kernel/lib/syscall`. The vDSO
  stub, the declaration `<ours/vdso.hpp>` includes and the kernel shim are
  generated from the entry; never write them by hand. Limits
  callers must respect (such as `MAX_DEBUG_WRITE`) live in
  `system/public/ours/syscall.hpp`.
- Ring-3 code calls the vDSO's entries directly, as `ours::<name>()` from
  `<ours/vdso.hpp>`; there is no wrapper library. Each declaration carries
  an asm label naming the vDSO's symbol, `ours_<name>`, so the C++ name is
  in namespace `ours` and the symbol stays a plain C name (the user's call,
  2026-10-03; Zircon's C API has `zx_<name>` for both). `//system/lib/ou`,
  one forwarding function per call, was removed on 2026-10-03 (the user's
  call: the vDSO's entries are already the user-level API, and a layer that
  only forwards is one more copy of each contract to keep true). Do not
  reintroduce one without a concrete need; a typed-handle layer in the shape
  of Zircon's `lib/zx` was planned and deferred for the same reason. A
  program prints with `ort::debug_line` (`<ort/debug.hpp>`, in the ours
  runtime, `//system/lib/ort`).
- A handle as a program holds it is `ours::Uandle`, a coined word ("u" for
  the user side, and "handle"): a scoped enum over u32 with
  `Uandle::Invalid`, so that it cannot be mixed with an integer. The
  kernel's table entry it names is `object::Handle`. A system call has both
  in view at once, so they are never spelled alike (the user's call,
  2026-10-03; it was `HandleValue`, an alias of u32). Zircon: `zx_handle_t`
  and `Handle`.
- The address-region object is **Vma** at every layer: `VmaDispatcher`,
  `ours::vma_*`, `HandleType::VmaRoot`, `ObjectType::Vma`, after
  `mm::VmArea` -- not Zircon's "Vmar" (the user's call, 2026-09-28, after a
  rename to Vmar was reverted). Values stay Zircon's.
- The kernel handler for `ours::<name>` (symbol `ours_<name>`) is
  `ours::syscall::<name>`, with no `sys_` prefix: Zircon needs `sys_`
  because its handlers share the global namespace, and here the namespace
  already says it (the user's call, 2026-09-28). Internal handlers in
  `table.cpp` follow suit.

## system/lib versus kernel/lib/k*

`system/lib/<name>` is the generic framework and stays free of kernel headers
so userspace can use it; `kernel/lib/k<name>` is the kernel's specialisation
(per-CPU state, scheduler, interrupt and preemption control). `concurrent` and
`kconcurrent` are the model, mirroring Zircon's `system/ulib/concurrent` and
`kernel/lib/kconcurrent`. Decide placement by dependency, not by which
directory is closer to the caller.

## Reusing other code

- Use a library only through what it offers its callers, never its internal
  machinery (the case that set this: `CpuLocal::Pinned` hooking into
  `lockdep::StateSaver`). If reuse would need internals, write the small piece
  locally and say in a comment why.
- Don't hand-roll what `eftl`, `ustl` and friends already provide. There is no
  Boost; `ustl` provides the containers.
- A shared primitive with a known defect is still the one to use: report the
  defect in the running review document and keep the standard spelling; never
  write a local `LockGuard` or other fork. (The case that set this: an
  `IrqSave` guard that did not disable interrupts, since fixed in the shared
  code, which is the point.)
- Every new third-party library needs the user's approval first;
  `vendor-import` is how an approved one is added. GN calls `obit` directly; do
  not write a parallel script to reimplement or diff-test it.

## Trust boundaries

Decide what crosses a boundary with an explicit allow/deny list, defaulting to
deny, generated from GN metadata on the producing targets. Never a rule that
special-cases the one item known to be sensitive today.

## Sequencing a feature

Get a slice running end to end before guaranteeing its semantics, but the
running path must go through the real, if minimal, interfaces of the layers
below. Routing around a missing interface with a different existing one (say,
`vmo_read` because `vma_map` does not exist yet) is a stand-in too: propose
adding the missing syscall first.

## Architecture, platform and drivers

- `kernel/arch/<cpu>` holds the CPU and the devices every CPU of that ISA
  carries itself: instructions, registers, traps, the MMU, context switch,
  CPU-local storage, and on x86 the local APIC, on riscv the hart timer, IPIs
  and hart start and stop, which go through SBI.
- Every other chip is a driver, in `kernel/driver/<class>/<chip>`, however
  many boards have it: on a PC the IO-APIC, HPET, PIT and 8259, on QEMU virt
  the PLIC; a 16550 is one driver both use.
- `kernel/platform/<board>` is the board: it finds the chips in its
  firmware's description (ACPI, the device tree), starts their drivers, and
  sets the policy over them, such as which timer serves which job.
- Zircon keeps the IO-APIC in arch and puts a chip only one board has inside
  that board's platform (`platform/pc/hpet`). The user chose the single rule,
  "is it inside the CPU", so that x86's IO-APIC and riscv's PLIC sit in the
  same place and no chip's home depends on guessing how many boards will
  share it (the user, 2026-10-30).
- Seams are named by prefix, as Zircon names them. A function the generic
  kernel calls and every architecture defines is `arch_<name>`; one every
  platform defines is `platform_<name>`; both are free functions in `ours::`.
  A name private to one architecture is `x86_<name>` or `riscv_<name>`, and
  never starts with `arch_` or `platform_`. The user chose one rule for both
  layers over namespaces (`ours::platform::`, and a new namespace for arch
  seams, since an `ours::arch` would hide libarch's `::arch` from code in
  `namespace ours`), 2026-10-30. Syscall handlers keep
  `ours::syscall::<name>`: they are not seams between layers.
- Where seams are declared: a function an architecture defines for the
  generic kernel is declared in `kernel/include/ours/arch_<topic>.hpp`, one a
  platform defines in `kernel/platform/include/ours/platform/`. `ours/arch/`
  holds only the headers every architecture provides under the same name, its
  per-arch set; a header private to one architecture lives in
  `ours/arch/<cpu>/`. So `arch_` means a cross-arch seam on a file name as on
  a function. Zircon shares one `arch/` directory between the generic and the
  per-arch halves; the user kept the split riscv-kernel step 1a made, which
  stops two directories from providing one `ours/arch/` name (2026-10-30).
- How an architecture provides a type the generic kernel embeds or calls
  (`ArchThread`, `ArchCpuLocal`, `ArchVmAspace`, `ArchIdleThread`): each
  architecture defines a class under its own name (`X86Thread`,
  `Riscv64Thread`) and the per-arch header aliases the generic name to it, as
  Zircon does (`using ArchVmAspace = X86ArchVmAspace`). The generic owner
  states the shape in a C++20 concept beside its own header and asserts it
  where it includes the per-arch one, so a port's missing member fails there
  with the concept's name instead of at the first caller. No virtual
  functions: these are on the paths that map, switch and read CPU-local
  storage. Zircon's other style, a generic class declaration with per-arch
  out-of-line members (`ArchIdlePowerThread`), was left out: it cannot hold a
  type that other objects embed. The user chose "A+B", 2026-10-30.
- An architecture that has not ported a libarch header fails by the header's
  file being missing, never by an `#if` ladder with a fallback. GN puts
  `kernel/arch/<cpu>/lib/arch/include` on the include path, and that
  directory provides the header itself under its public name
  (`arch/cache.hpp`): no generic file includes a per-arch one, and a host
  stub directory serves the host tests. This is how Zircon's `lib/arch` is
  built. The user chose it over an `#error` at the end of each ladder, and
  over a generic header that includes a fixed per-arch file name
  (`arch/impl/<name>.hpp`), 2026-10-30. What it gives: the compiler names the
  missing file, and no branch can fall through to a declaration that
  compiles and fails at link. Where the declarations and comments that every
  architecture's header must agree on live, in one shared header under
  another name or repeated, is settled with the first header converted.
