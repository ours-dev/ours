# Boot chain and userspace startup

The chain is GRUB (Multiboot2), bootshim, physload, physboot, vmours.

## How bring-up work is ordered

Follow the runtime execution order from `start_kernel` (`kernel/top/main.cpp`)
and fix whatever the boot reaches next. A known defect further down the boot
path is recorded in `docs/repairs` and deferred, however severe; jumping
between modules is what the user wants to avoid.

## phys

- physload owns the entire phys runtime: console, BootMem, Aspace, ELF loaders,
  panic, print, OBI parsing. Modules are dynamically linked against it, GRUB
  style, so the system has exactly one of each. The point is one copy of that
  machine code shared by physboot and physload, not the freedom to swap builds.
- Modules share raw C++ struct layouts with physload, which is only safe when
  both come from the same build. Every module carries a PT_INTERP build id
  (`kernel/phys/kernel_elf_interp.gni`), and `ModuleLoader` refuses a module
  whose id differs from physload's own. Keep that check. Zircon uses a fixed
  entry symbol and relative-only relocation; this tree deliberately keeps
  descriptor-based entry and GOT symbol binding, and borrows only the build-id
  pairing.
- `PHYS_EXPORT` marks the module ABI; adding a symbol to it is one
  `PHYS_EXPORT`, never a source file copied into a module.
- A module declares its entry with `PHYS_MODULE("name", init_fn)`, never a
  fixed entry symbol or ELF entry address.
- Modules are GN `loadable_module`s on `default_shared_library_configs`, never
  `kernel_elf_binary`. Package them with `--strip-debug`, never
  `--strip-sections`: the loader needs the section headers.
- Treat the existing `kernel/phys` structure as provisional and check it
  against this shape before extending it.
- OBI bakes its item and string tables at build time and cannot be appended to
  at run time (unlike Zircon's ZBI), so some shim facts travel in a
  side-channel struct.

## Image layout

physboot loads vmours as one contiguous allocation and the kernel later frees
the `.init*` region, so those sections sit at the tail of the loadable image
(`kernel/kernel.ld`: `... .bss, .init.text, .init.rodata, .init.data,
.ccrt_init`, with `__init_end == __executable_end`). Don't move `.bss` back
behind them.

## Boot options

The bootloader command line is a phys-stage input only: it reaches
`ours::phys::BootOptions` (`kernel.phys.next`, `kernel.name`, `aslr.disabled`)
and stops there. vmours reads its options from the OBI Config item through
`ours::boot::boot_package()`. Never carry the command line in `Handoff`; a new
kernel option belongs in the OBI container.

## userboot and init

userboot is always the first user process, and it starts what
`userboot.next` names: Zircon's `kernel.select.userboot` picks the userboot
binary. The dev image sets `userboot.next=bin/init`, dev.dbg_1
`userboot.next=bin/shell`. Say "what userboot starts next", and never name a
field `userboot` when its value is the next program.

On dev, init is the root of the component framework, ours's counterpart of
Fuchsia's component_manager (`docs/design/261004/component-framework.md`,
decided 2026-10-04). It keeps VMEX for life, serves `/boot` from the BootFS
itself, starts the components its root manifest (`meta/root.cm`) names,
routes their capabilities, serves each one's `/svc`, and hosts
`fuchsia.process.Launcher`. dev.dbg_1 has no root: userboot starts the shell
directly, as Fuchsia's userboot tests run without component_manager.

User programs install under `bin/` in the user BootFS, as Zircon's do under
`/boot/bin`; userboot stays at the top level of the kernel package, where the
kernel looks it up. Code never spells a program's name: it includes the header
`user_program_names()` generates from the install names
(`//system/user_program_names.gni`).

## Filesystems and namespaces

Follow Fuchsia, not Linux (decided 2026-10-04, reversing the Linux-style VFS
of 2026-09-26). There is no VFS process and no global mount table: each
process has its own namespace, a client-side table from path prefix to
directory channel (fdio's `fdio_ns_t`), filled from its startup handles.
Files are served over a hand-written fuchsia.io subset by servers built on one
server library, ours's counterpart of `src/storage/lib/vfs`.

A file may be executed only if the VMO handle its server hands out carries
the Execute right. Only a VMEX holder can add that right; the root marks the
BootFS files under `bin/` at ingest, as component_manager's BootfsSvc does.
The Launcher takes an executable VMO and resolves no names.

A filesystem library such as `system/lib/bootfs` only parses, looks up and
reads flat names; any directory tree lives in the server. Design documents
that describe a VFS process with a mount table, the standalone launcher or
launching by BootFS name predate this decision.

## Debug switches

Debug-gated code keys off `KERNEL_DEBUG_LEVEL` (enabled when non-zero). No
parallel `OURS_DEBUG`.
