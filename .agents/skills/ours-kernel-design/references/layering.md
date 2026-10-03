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
  -- `userabi::launch()` for the programs the kernel starts (the first one
  and `process_spawn_boot`'s), and since K23 the user-space launcher through
  `vma_map` on a vDSO VMO handle it is given, as Zircon's userboot and
  process_builder do. `ProcessDispatcher::create` does not map it. The user
  chose Zircon's shape over "the kernel maps it at process creation"
  (2026-09-28); never copy the vDSO per process.
- Adding a syscall means: an entry in `system/public/ours/syscalls.gni` and
  the body of `ours::syscall::<name>()` in `//kernel/lib/syscall`. The vDSO
  stub, the `extern "C"` declaration `<ours/vdso.hpp>` includes and the
  kernel shim are generated from the entry; never write them by hand. Limits
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
