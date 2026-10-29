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
- `ElfImage::load_at()` ends with `arch::sync_instruction_fetch_range()` over
  the image, so every image phys loads, whoever loads it, is safe to enter
  once `load_at()` returns. On riscv that is `fence.i`. On x86 it is
  deliberately empty, a compiler barrier only: x86 relies on the first option
  of Intel's SDM vol. 3, "Handling Self- and Cross-Modifying Code" (8.1.3
  in the edition Zircon cites), a jump to the newly written code, and every
  entry into a loaded image is a call or a jump. Zircon takes the
  second option, a serializing `cpuid` when its `GlobalCacheConsistencyContext`
  is destroyed; under virtualization `cpuid` is a VM exit, paid for no case
  this tree has, so do not "fix" x86 back to Zircon's (the user, 2026-10-29).
  The sync covers only the CPU that ran it, and only the stores made before
  it: anything that writes an image's text after `load_at()` (code patching,
  a relocation into a segment both writable and executable) must sync again.

## Firmware

- On riscv, phys and the kernel know the M-mode firmware only through the SBI
  specification and the device tree, never as OpenSBI: no address, size,
  message or behaviour of one implementation that the specification does not
  promise. OpenSBI is only what QEMU's `-bios default` loads; another SBI
  implementation must boot the same image unchanged (the user, 2026-10-28).
  Where the firmware's own memory lies comes from the tree's
  `/reserved-memory`, never from a constant.
- The legacy `console_putchar` (extension 0x01) is the one call in use that a
  conforming firmware may leave out: the specification deprecates it. It is
  kept because it needs no probe. If a firmware without it turns up, the fix
  is the debug console extension, probed through the base extension first.
- The shim's fixed load address, 0x80200000, comes from the platform, not the
  firmware: QEMU's `virt` loads a flat `-kernel` at the first 2 MiB boundary
  above the firmware, and its RAM starts at 0x80000000.
- Test tooling may know which firmware it starts (`riscv_boot.py` matches
  OpenSBI's banner), because it chose that firmware itself. Only a boot under
  a second implementation shows the rule holds; that is
  `docs/repairs/261028/riscv-boot.md`.

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
`ours.process.Launcher`. dev.dbg_1 has no root: userboot starts the shell
directly, as Fuchsia's userboot tests run without component_manager.

Programs reach capabilities by path, not by startup handle (the user's
call, 2026-10-05): a resource is a protocol the root serves in `/svc`
(`ours.resource.*`, with `ours_resource::get()` the client), granted by
routing. Only what runs without a root -- userboot, and dev.dbg_1's shell --
takes resources as startup handles.

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
Files are served over ours.io, a hand-written subset of fuchsia.io renamed
into ours's own namespace (2026-10-04), by servers built on one server
library, `system/lib/ioservice`, ours's counterpart of `src/storage/lib/vfs`.

A program's namespace is bound before main (the user's call, 2026-10-05):
ort, a default dependency of every `user_executable()`, calls
`ort_extensions_init()` between reading processargs and main, and fsio
defines it to run `fsio::install()`, as Fuchsia's libc calls fdio's
`__libc_extensions_init`. ort holds an empty weak definition, not a weak
reference, because the loaders' binder resolves only against the vDSO and
cannot leave an undefined weak symbol at 0.

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
