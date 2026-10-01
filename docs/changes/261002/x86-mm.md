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

## Title: A page-table change reaches every CPU running the address space: the TLB shootdown (2026-10-02)

**Files**
- `kernel/arch/x86/mm/arch_vm_aspace.cpp` — `ArchVmAspace::invalidate()`: after `unmap()` or `protect()` of a user address space, every CPU in `m_active_cpus` drops its translations of the range, through `IpiService::sync_exec()`, and the call returns only once they all have. A kernel address space keeps the local `invlpg` it had. `switch_context()`'s comment corrected about Zircon's order.
- `kernel/arch/x86/mm/include/ours/mm/arch_vm_aspace.hpp` — `invalidate()` declared.
- `kernel/arch/x86/mm/page_table.cpp` — `unmap_pages()` and `protect_pages()` no longer flush this CPU's TLB; the address space does it, for every CPU.
- `kernel/commands/vmo_snapshot_test.cpp` — a two-CPU check in `vmosnapshot.test` (6 checks, 200 in all).

**Symptom**
A write could go through a translation that had lost Write. Reproduced by the new check before the fix, on the dbg image: a writer pinned to one CPU writes a mapped page, a cloner on another CPU clones the object (which takes Write off the entry), and the writer's next write, with no switch in between, went through without a fault, into the page the clone now shares: `FAIL its next write ... faults` and `FAIL the clone holds what the page held when it was made`. Since `vmo_create_child(SnapshotFull)` is reachable from user space, a program with a thread on another CPU, or another process mapping the same VMO, could write into a snapshot after it was taken.

**Cause**
`protect_pages()` and `unmap_pages()` ran `invlpg` on the CPU that called them and nowhere else. A TLB is per CPU: changing an entry in memory does nothing to the copies other CPUs have cached, and they go on using them until they flush. For a clone that is a broken promise: the clone takes Write off every mapping of the shared pages so that the next write faults and copies the page; a CPU that cached the entry while it was writable never faults. The same holds for any unmap: a CPU that cached the old translation keeps reading and writing the frame after it has been freed and handed to someone else.

Where the shootdown happens matters as much as that it happens, and both rules came out of the user's questions:

- After the page table's spin lock is dropped. That lock is held with interrupts off, and the targets answer from their interrupt handlers. A target spinning for the same lock with interrupts off would never answer, and the sender would never release it: a deadlock with a single shootdown, not only with two CPUs shooting at each other.
- Before the operation returns, which is under the object's lock the caller holds. Until the shootdown completes a stale CPU can still write through its old entry. Under the object's lock no fault can copy the page meanwhile, so such a write lands before any copy is taken, as if it had come before the clone. With the object's lock dropped first, a fault on a third CPU could copy the page (X to X') and the stale write would land in X after the copy: lost to the writer, and visible to the clone.

**Change**
Before, in `PageTable::protect_pages()`:
```cpp
// The local TLB only. What another CPU running this address space still
// holds is the open question of the clone's design ...
for (usize i = 0; i < pages; ++i) {
    arch::x86_invlpg(va + i * PAGE_SIZE);
}
```
After, in `ArchVmAspace::invalidate()`, called by `unmap()` and `protect()` once the page table has dropped its lock:
```cpp
task::AutoPreemptDisabler stay;
std::atomic_thread_fence(std::memory_order_seq_cst);
auto const targets = m_active_cpus.load(std::memory_order_relaxed);
IpiService::sync_exec(targets, invalidate_here, &range);
```
The fence pairs with the sequentially consistent bit set in `switch_context()` (previous entry): the entry was changed with a plain store, and x86 may let the mask load pass it. Preemption is off because `sync_exec()` serves "this CPU" itself rather than interrupting it, and that has to be the CPU the mask was read on.

Rejected: interrupting every online CPU (correct, but every protect of every process would stop every CPU); flushing under the page-table lock (the deadlock above).

**Reference**
Zircon `X86PageTableMmu::TlbInvalidate()` (`zircon/kernel/arch/x86/mmu.cc:332`), which targets `aspace->active_cpus()` and every CPU for a global entry, and `mp_sync_exec()`. Linux `flush_tlb_mm_range()` over `mm_cpumask()`.

**Verification**
- Every changed C++ line formatted; `boot_check.py --out out/vmo-snapshot`: dbg PASS (18 passed), rel PASS (13 passed, 10 skipped).
- `vmosnapshot.test` over COM1: 200 checks ok on both images (194 before, 6 new). `usercopy.test` 18, `vmaseal.test` 13 and `pmnode.test` 20 on dbg, and `usercopy.test` 18 and `vmaseal.test` 13 on rel, still pass.
- Before the fix the new check failed on dbg as described under Symptom. Mutation after it: `sync_exec()` replaced by the local flush alone; both images FAIL the same two checks. Restored.
- The first version of the check pinned the writer to the CPU the command was running on: the writer spins with preemption off, so the command could not spawn the cloner, and the writer timed out (rel, `FAIL the writer's first write goes through, and the clone is made while it waits`). The writer now goes on another CPU and the cloner is spawned first.
- Not verified: the fence. A missing fence is a reordering QEMU's TCG does not produce; it rests on the argument above, not on a test.

**Deliberately not done**
- The kernel address space: its entries are Global and aliased into every address space, so all CPUs would have to be asked, and some of its unmaps run with interrupts off. `docs/repairs/261002/x86-mm.md` item 1.
- Batching and a full-flush ceiling: one round of IPIs per call, `invlpg` per page. Performance, to be measured with the user: `docs/repairs/261002/x86-mm.md` item 2.
- `sync_exec()` reading `Cpu::id()` with preemption on: a `kernel/task` bug that `invalidate()` works around; `docs/repairs/261002/kernel-task.md` item 1.

**How it was found**
The open question in `.agents/tasks/vmo-snapshot-child/DESIGN.md` since 2026-09-29, live since step 10 exposed snapshots to user space on a four-CPU machine. The new check reproduced it before the fix.

**The user's prediction**
- Asked whether the shootdown may be sent while holding `PageTable::m_lock`: "it deadlocks, the two wait for each other". Right; the answer added the case that needs only one shootdown (a target spinning on the same lock with interrupts off).
- Asked whether it has to be done under the object's lock: "it can be done holding it; the point is to synchronise the page tables, and only the CPUs using the same page table need to run invlpg once". The target set is right and is what was built. "Can" should have been "must": the reason is the lost write above, which the answer did not give.

**Three questions you should be able to answer**
- Why can a shootdown not be sent while holding the page table's spin lock, even when only one CPU is shooting?
- What is lost, and where does it go, if the object's lock is dropped before the shootdown completes?
- Why does the writer in the test keep preemption off between its two writes?
