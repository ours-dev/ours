# io-space: port I/O is a privilege the process holds, not part of its address space

**Date**: 2026-10-25
**Status**: Proposed

## Problem
The user asked on 2026-10-25 whether I/O ports belong in `ArchVmAspace`, given that not every architecture has them.

- **Only x86 has a port space.** It has 65536 ports reached with `in` and `out`. Each CPU's TSS holds a permission bitmap that every ring-3 port access is checked against.
- **Other architectures have MMIO only.** arm64 and RISC-V reach a PCI I/O BAR through an MMIO window, which a program gets as a physical VMO like any other device memory.
- **Where the bitmap lives today.** The x86 `ArchVmAspace` owns the process's `IoBitmap`, with `open_ioports()` and `close_ioports()`, and loads it in `switch_context()`. That is practical for two reasons:
  - the address-space switch is a per-CPU hook that already runs whenever the process changes;
  - `m_active_cpus` is the set of CPUs to reload when the ports change.

That arrangement has two problems.

**It imposes x86 on every architecture's memory interface.**

- The arch-neutral `ProcessDispatcher::open_ioports()` (`kernel/object/process_dispatcher.cpp`) calls `m_aspace->arch_aspace().open_ioports()` unconditionally.
- `ioports_request` and `ioports_release` are in the common syscall table.
- So every architecture's `ArchVmAspace` must grow port methods just to compile. This is latent only because arm and riscv have no `ArchVmAspace` yet.

**The address space stands in for the process, and that stand-in is about to break.**

- A port permission is a privilege of the process, not an address translation. The address space represents the process only because a process has exactly one address space today.
- `ProcessDispatcher` already reserves a shared range above its private one: `SHARED_ASPACE_BASE`, and the `create_shared()` its comment names.
- Once a process holds a private and a shared address space, it is no longer clear which one's bitmap applies, or which CPUs "run the process".

## Goals and non-goals
Goals:

- On an architecture without port I/O, generic code compiles no port code, and `ioports_request` and `ioports_release` answer NotSupported.
- The port permission belongs to the process. The CPU loads it when it switches to a different process, whatever address space that process's thread is running in.
- Keep what works today:
  - grants are per process and checked against an IoPort resource;
  - an open or close is in force on every CPU before the call returns;
  - a switch touches only the two processes' runs, and the version scheme keeps that safe against a change still on its way.

Non-goals:

- Changing the ABI of `ioports_request` or `ioports_release`.
- Per-thread grants (Linux `ioperm()`).
- Virtualising ports for guests.

## Design
Objects:

- **`arch::HAS_PORT_IO`**: a `constexpr bool` each architecture defines; true on x86 only.
- **`arch::IoSpace`** (x86): the ports ring 3 may use, kept as sorted runs. This is today's `IoBitmap`, moved out of `ArchVmAspace` without change:
  - the runs and their version;
  - each CPU's loaded version;
  - the spin lock, taken with interrupts off.
- **The process holds it.** `ProcessDispatcher` has an `m_io_space` member:
  - where `HAS_PORT_IO` is true, an `arch::IoSpace`;
  - elsewhere, an empty type marked `[[no_unique_address]]`, so the member costs nothing.

  `open_ioports()` and `close_ioports()` exist only where `HAS_PORT_IO` is true.
- **The CPUs running the process.** `IoSpace` keeps its own mask of CPUs that have it loaded. It sets and clears the mask in the switch hook, the way `ArchVmAspace` sets and clears `m_active_cpus` today. An open or close reloads exactly those CPUs.
- **The switch hook.** Today `ArchVmAspace::switch_context()` loads the incoming address space's bitmap. Instead, the arch context switch compares the outgoing thread's process with the incoming one's, and loads the incoming `IoSpace` when they differ. A switch between threads of one process touches nothing.

Events, `ioports_request` on x86:

1. The syscall checks the resource, as today.
2. `ProcessDispatcher::open_ioports()` calls `m_io_space.open(base, count)`.
3. `open()` updates the runs and bumps the version.
4. It reloads every CPU in its own mask before it returns, as `IoBitmap::open()` does today.

Events, `ioports_request` on an architecture without port I/O: the syscall answers NotSupported before it touches the process. `ProcessDispatcher` has no port methods to call.

Events, a context switch from a thread of process A to a thread of process B:

1. The arch hook sees that the processes differ.
2. It loads B's `IoSpace` into the TSS. This closes A's runs and opens B's.
3. It moves this CPU's bit from A's mask to B's.

This is independent of which address space either thread is running in.

The divergent part: treat the port space as a second, tiny address space. Its 65536 one-byte "pages" are granted from IoPort resources, the way device memory is granted from MMIO resources through a VMO. This design takes the first step of that idea: the grants live in a per-process object of their own. The open question below takes it further, with grants as handles.

Reference:

- Zircon keeps `IoBitmap` in the x86 address space (`arch/x86/include/arch/x86/ioport.h`). It is loaded by `x86_set_tss_io_bitmap()` in the context switch of `arch/x86/mmu.cc`.
- Zircon compiles `sys_ioports_request()` only `#if ARCH_X86` (`lib/syscalls/driver.cc`), and answers `ZX_ERR_NOT_SUPPORTED` elsewhere.
- Linux keeps the bitmap per thread (`ksys_ioperm()`, `struct io_bitmap` shared by reference on fork, `arch/x86/kernel/ioport.c`). It copies the bitmap into the TSS lazily, on the way back to user mode (`tss_update_io_bitmap()`, `arch/x86/kernel/process.c`).

## Alternatives considered
- **Gate only (Zircon's shape).** Keep `IoBitmap` in the x86 `ArchVmAspace`, add `arch::HAS_PORT_IO`, and compile `ProcessDispatcher`'s port methods and the syscalls' bodies only where it is true.
  - Smallest change, and it removes the cross-architecture constraint at once.
  - It leaves the address space standing in for the process, which the shared address space will break.
- **Per thread (Linux `ioperm()`).** Finer than any driver here needs, and every thread switch, not only a process switch, may have to touch the TSS.
- **Load lazily on return to user mode (Linux).** It saves switch work for threads that never reach ring 3 with a different bitmap, at the cost of a hook on the return path. It could be combined with either placement, and is not needed at this scale.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| R1. Where the port permission lives | the x86 address space, gated (alternative 1); the process, as an `IoSpace`; the thread (alternative 2) | the process | None (homework paused) | |
| R2. When | all now; the gate now and the move to the process before the shared address space lands; only with the shared address space | the gate now, the move before the shared address space | None | |

## Testing and verification
- `ioport.test` (kshell) passes on both images, including its reload checks, which today read `ArchVmAspace` and will read `IoSpace`'s own mask.
- The shell's ioport checks pass on both images.
- Control run: in a local x86 build with `HAS_PORT_IO` forced false, `ioports_request` answers NotSupported, and the shell's ioport check reports that.
- After the move, two threads of one process keep the ports through a switch between them, with no TSS write. A port counter or a trace shows it.
- `ours-verify` on every step.

Cannot be verified: a build of another architecture, since none has an `ArchVmAspace` yet.

## Open questions
- **Grants as handles.** Should `ioports_request` return a grant object, as a mapping is one?
  - Closing the handle would release the ports.
  - A grant could be passed to another process by handle transfer.
  - A process's `IoSpace` would then be the union of the grants it holds.
  - "Who can touch port 0x3f8" would be answered by looking at handles.

  This is an ABI change, and worth it only if drivers come to share ports.
- **How the shared address space changes the switch.** It is to be settled with that design: does a thread that moves between its process's private and shared address space switch anything here? With the permission on the process, the answer should be no.
