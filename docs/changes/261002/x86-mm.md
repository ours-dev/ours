# x86-mm changes

## Title: An address space knows which CPUs are running on it (2026-10-02)

**Files**
- `kernel/arch/x86/mm/include/ours/mm/arch_vm_aspace.hpp` — `m_active_cpus` becomes `std::atomic<CpuMask>`, with what it means; `active_cpus()` says it is a snapshot.
- `kernel/arch/x86/mm/arch_vm_aspace.cpp` — `switch_context()` sets this CPU's bit in the incoming address space before writing CR3 and clears it in the outgoing one after; `active_cpus()` is defined; `destroy()` asserts the mask is empty.

**Symptom**
Latent, cannot be triggered today: nothing reads the mask yet. It is the first half of the TLB shootdown, which needs to know which CPUs to ask.

**Cause**
`m_active_cpus` was declared, initialised and never written, and `active_cpus()` was declared and never defined. A TLB shootdown has to interrupt every CPU that may hold a translation from the page table it changed, and on x86 without PCIDs that is exactly the CPUs whose CR3 holds it now: loading another root flushes every non-global translation, and user leaves are never Global. Without the mask the only choices are to interrupt every online CPU, or (what the code does today) none but this one.

The order around the CR3 write is what makes the mask safe to read. The shootdown will do: change the entry, full barrier, read the mask, interrupt who is in it. A CPU switching in does: set its bit by a sequentially consistent read-modify-write, write CR3 (serialising), then walk the tables. With a full barrier on both sides one of two things must be true: the shootdown sees the bit and interrupts the CPU, or the CPU's walk comes after the entry changed and caches the new one. Without the barriers, x86 lets each side's later load pass its earlier store, and both can miss each other: the textbook store-buffer case. The outgoing bit is cleared only after the CR3 write, because until then the CPU may still hold the outgoing tables' translations.

**Change**
```cpp
auto const cpu = Cpu::id();
set_cpu(to->m_active_cpus, cpu, true);   // before CR3
// ... CR3 write ...
if (from != nullptr && from != to) {
    set_cpu(from->m_active_cpus, cpu, false);   // after CR3
}
```
`set_cpu()` is a compare-and-swap loop, because `std::atomic<CpuMask>` has no `fetch_or` or `fetch_and`, as in `IpiService::handle_generic()`. `from == to` is skipped, or the bit just set would be cleared on a CPU running on those tables.

**Reference**
Zircon `X86ArchVmAspace::ContextSwitch()` and `active_cpus_`, and `X86ArchVmAspace::Destroy()`, which asserts `active_cpus_` is zero. Linux keeps the same set as `mm_cpumask()`, updated in `switch_mm_irqs_off()`.

**Verification**
- Every changed C++ line formatted; `boot_check.py --out out/vmo-snapshot`: dbg PASS (18 passed), rel PASS (13 passed, 10 skipped).
- Mutation: the outgoing bit never cleared. dbg fails at once with `Assertion failed on active_cpus().none() with message: freeing page tables another CPU is running on`, when the first process is destroyed: the new assertion is reached and holds only because the mask is kept. Restored.

**Deliberately not done**
- The shootdown itself, which reads the mask: the next commit.
- PCIDs. With them a CPU that switched away keeps the old translations, and the set to interrupt becomes "every CPU that has run on these tables since they were last flushed there", which is what Linux tracks lazily. None are allocated today.

**How it was found**
None; part of the TLB shootdown step of `.agents/tasks/vmo-snapshot-child`.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is the incoming bit set before CR3 is written and the outgoing one cleared after?
- What goes wrong without a full barrier between changing an entry and reading the mask?
- Why is "the CPUs whose CR3 holds it now" enough without PCIDs, and not with them?
