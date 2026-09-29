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
  `//system/user_executable.gni`, relocations resolved by `bind_imports()` in
  `kernel/lib/userabi/src/userboot.cpp`. Never reintroduce a hand-resolved
  export table.
- There is one vDSO VMO, laid out once by the kernel (`userboot_init()`, like
  Zircon's `VDso::Create()`), and it is mapped by whoever starts the process
  -- today `userabi::launch()`, after K23 the user-space launcher through
  `vma_map` on a vDSO VMO handle it is given, as Zircon's userboot and
  process_builder do. `ProcessDispatcher::create` does not map it. The user
  chose Zircon's shape over "the kernel maps it at process creation"
  (2026-09-28); never copy the vDSO per process.
- Adding a syscall means: kernel handler, vDSO stub exported in `.dynsym`,
  declaration in `system/public/ours/vdso.hpp`, and exactly one named entry in
  `//system/lib/ou` (even if it only forwards). Ring-3 code calls `ou::...`,
  never raw `ours_*` symbols. Limits callers must respect (such as
  `MAX_DEBUG_WRITE`) live in `system/public/ours/syscall.hpp`.
- The address-region object is **Vma** at every layer: `VmaDispatcher`,
  `ours_vma_*`, `ou::vma_*`, `HandleType::VmaRoot`, `ObjectType::Vma`, after
  `mm::VmArea` -- not Zircon's "Vmar" (the user's call, 2026-09-28, after a
  rename to Vmar was reverted). Values stay Zircon's.
- The kernel handler for `ours_<name>` is `ours::syscall::<name>`, with no
  `sys_` prefix: Zircon needs `sys_` because its handlers share the global
  namespace, and here the namespace already says it (the user's call,
  2026-09-28). Internal handlers in `table.cpp` follow suit.

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
