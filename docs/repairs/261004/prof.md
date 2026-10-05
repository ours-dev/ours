# prof outstanding repairs

## 1. Kernel samples unwind no further than the interrupted PC

**Severity**
Medium: every kernel sample's stack is one frame deep, so `prof-fold` gives leaf functions instead of call stacks; nothing else in the kernel reads these records, but step 5 of `.agents/tasks/prof-sampler-object` (folded kernel stacks from a user program) cannot pass until this is fixed.

**Symptom**
On `dev.dbg_1` (`SMP=1`), `prof.start 1000000`, `prof.stop`, `prof.dump` gives 9 records, every one with `depth` 1 and `flags` 0, at runtime PC `0xffffff8840031f28` with `load_base` `0xffffff8840000000`, so link address `0x31f28`, inside `SerialLogger::write()`. `llvm-readelf --unwind` on the same `vmours` shows an FDE covering it (`initial_location 0x31ea0`, `address_range 0xe4`), and the boot log says `prof: indexed 1450 unwind frames`.

**Cause**
Not found yet. `flags` 0 means `Sampler::unwind_context()` stopped on `StepResult::NoFde` at the first step, not on a failed stack read or a CFI error: `Unwinder::locate()` asked `FdeIndex::find(pc)` and got nothing, although an FDE covers the PC. So the index and the PC disagree. Candidates, none checked: the index keys are link addresses while the PC is a runtime address (they differ by `load_base`), which would happen if `pc_begin` is decoded relative to something other than the runtime address of the field it is read from; or the index `prof_init()` sorts by hand (an insertion sort in `kernel/commands/prof.cpp`) is not in the order `find()`'s binary search expects. The unwinder's inputs did not change in the 2026-10-04 seam removal (the pc, sp and bp are the same frame fields as before), so this predates it.

**Change**
First measure, then fix: log the first and last entries' `pc_begin` after `build_fde_index()` and compare them with `__executable_start`; that tells the two candidates apart. Then fix wherever the address spaces part, and add a host test in `//kernel/lib/unwind` that builds an index from an `.eh_frame` copied to a different address than it was linked for and finds a PC in it.

**Reference**
None, this project's own unwinder.

**Acceptance**
The same `prof.start`/`prof.stop`/`prof.dump` run on `dev.dbg_1` gives records with `depth` above 1, and `prof-fold` prints call stacks, `SerialLogger::write` under its callers.
