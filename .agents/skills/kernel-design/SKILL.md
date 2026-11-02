---
name: kernel-design
description: Settled design decisions for the ours kernel that the code alone does not reveal. Load it first, before reading any code, for every question or task about how something in kernel/ or system/ should be designed, structured, placed or reviewed -- "which layer should X go in", "how should X work", "review X", "X 该放哪一层", "X 怎么设计", "帮我 review 一下" -- including new syscalls and kernel objects, userabi, mm, CpuLocal, scheduler and locking, timers, phys and boot, userboot/init and filesystems, and whenever choosing between the Zircon and the Linux approach.
---

# ours kernel design rules

The kernel is largely a reimplementation of Zircon, often keeping Zircon's
names, with a few deliberate Linux-derived exceptions. The code shows what
exists; these files record what was decided and why, so a decision does not
get "fixed" back into a default.

## Before designing anything

1. **Find the reference first.** Zircon is the default spec. Read the
   counterpart in the local Fuchsia checkout
   (`~/Workspaces/projects/fuchsia/zircon` on the main Mac) before explaining
   what an interface is meant to do or shaping a mechanism, and cite the file.
   Ask which layer Zircon lets own the decision (a boot option, a package
   member, a handoff field), not only what a call does; Zircon's tests and
   alternate images usually show the seam. The Linux exceptions are listed in
   the subsystem files.
2. **Build on the layer below, through its own abstractions.** A defect in
   that layer is a reason to fix it there or report it with evidence, never to
   route around it with a parallel implementation.
3. **The user-facing interface comes first.** Work is chosen by what a
   userspace requirement forces into existence, and each dependency gets its
   real, minimal interface before anything builds on it. No stand-in syscalls,
   no workaround "to be replaced later".
4. **Suspect the API, not just the call site.** When something is awkward, ask
   whether the interface, or the abstraction itself, should exist in this form.
5. **For a review, also load `cpp-code-review`.** It sets the method and the
   report; this skill supplies the decisions the code is reviewed against.

## Where to look

| Working on | Read |
|---|---|
| syscalls, kernel objects, userabi, vDSO, `ours_*`, system/lib vs kernel/lib, reusing a library, trust boundaries | [references/layering.md](references/layering.md) |
| physical memory, MemMap, VmObject, user copies, CpuLocal, allocators | [references/mm.md](references/mm.md) |
| scheduler, threads, locking, wait queues, interrupt lines (kernel/irq), timers, prof | [references/task.md](references/task.md) |
| phys, boot shim, physload modules, linker script, boot options, userboot/init, filesystems | [references/boot.md](references/boot.md) |

## Hands off

- `kernel/kernel/task/sched/fair.*`, the Fair/EEVDF discipline, belongs to the
  user: no edits of any kind, including mechanical renames. Record bugs in it
  in `docs/repairs` instead.
- An empty class, stub function or uncalled init step is often scaffolding for
  planned work. Never delete one as cleanup; ask, or leave it alone.
- `WEAK_LINK` on a generic implementation (libc's `mem*`, NUMA stubs) is the
  hook for a later arch-optimised strong definition. Keep it.

When a new decision gets made or a correction repeats, add it to the matching
reference file, with the reason.
