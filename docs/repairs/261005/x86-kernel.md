# x86-kernel outstanding repairs

## 1. Every address space switch takes both I/O bitmaps' locks, even when neither process opened a port

**Severity**
Low, performance, and not measured: a cost on every switch between address spaces, added by `ioports_request` (`docs/changes/261005/x86-kernel.md`), that nearly every switch pays for nothing, since only drivers open ports.

**Symptom**
None observed. `ArchVmAspace::switch_context()` now ends with `IoBitmap::load()`, which saves and disables interrupts, takes and releases the outgoing address space's bitmap lock, then the incoming one's, and walks two run lists that are empty for every process but a driver.

**Cause**
`load()` cannot know whether the outgoing bitmap has runs, or whether its version moved on, without that bitmap's lock: both are written under it by `IoBitmap::change()` on another CPU. So the switch pays two lock round trips, each an atomic read-modify-write on a cache line another CPU may have written, plus the interrupt save and restore, whatever the two processes hold. Against the CR3 write beside it, which flushes the TLB because no PCID is ever allocated, this is likely small; nobody has measured it.

One of the two locks is often the same lock on every CPU. The kernel's address space has an `IoBitmap` too, and every CPU loads it at each switch to or from a kernel thread: the idle thread, a Dpc worker, the kernel shell. So that one lock's cache line moves between all the CPUs on the scheduler's path, where a process's bitmap is touched only by the CPUs that run that process. Of the two costs, this is the one that grows with the number of CPUs.

**Change**
Measure first, as CLAUDE.md asks for performance work: the time `load()` adds to a switch between two processes without ports, against the whole switch, under a benchmark in `benches/` that switches between two user threads of different processes on one CPU, with the median and the tail; and, for the kernel's lock, the same switch between a user thread and a kernel thread run on every CPU at once, against one CPU alone. Only if it matters, the candidates to put to the user:
- a per-bitmap flag, written under the lock and read without it, saying the bitmap has no runs and its version has not moved since the last load here; the switch skips the lock when both bitmaps say so. The ordering argument for reading it unlocked has to be written first: a change that makes the flag false must still be seen by a CPU that loaded the old version, which is what the mask and the reload give today.
- skipping `load()` when neither address space has ever had a port opened, kept as a sticky bit set once under the lock by the first open.
- leaving the kernel's address space out of the locking: its bitmap never has runs and its version never moves, since only a user address space's ports are opened (`ArchVmAspace::open_ioports()` asserts it), so a switch to or from it needs neither its lock nor its runs and takes only the user bitmap's lock. The argument to write first is that nothing ever edits the kernel's bitmap, which today is a debug-build assertion and the fact that every system call acts on a process's address space.

**Reference**
Zircon `X86ArchVmAspace::ContextSwitch()` in `zircon/kernel/arch/x86/mmu.cc` takes both bitmaps' locks on every switch as well, through `x86_clear_tss_io_bitmap()` and `x86_set_tss_io_bitmap()` in `zircon/kernel/arch/x86/ioport.cc`; each returns early under the lock when the address space never allocated a bitmap.

**Acceptance**
A benchmark in `benches/` that reports the switch's cost with and without the change, the user's decision recorded in the change record, and `ioport.test` still `ok` on dev.dbg_1.
