# unwind outstanding repairs

## 2. The sampling tick adds about 3.4 KiB to the interrupted thread's 16 KiB stack; evaluate() alone has the kernel's third-largest frame

**Severity**
Low: Every number checks out against the built vmours.

**Symptom**
A kernel thread about 10 KiB deep takes a sampling tick, for example on a fault or ELF-load path through the kernel's 0.6-4.8 KiB frames. The tick's frames then run below the stack's base into whatever is mapped there, with interrupts off:

- IRQ frame
- arch_handle_exception 720 B
- IRQ handling about 600 B
- timer queue about 650 B
- on_sample_due 128 B
- record_sample 192 B
- unwind_context 144 B
- step 672 B
- evaluate 2256 B

**Cause**
evaluate() keeps two whole-state copies and a four-deep remember stack on the stack. EvalState is 424 bytes, and there is one for initial_snapshot plus remember_stack[4]. Its frame is therefore 2200 bytes (`subq $0x898, %rsp` in vmours), and only elfload::mapping_pages and check_program are larger. Unwinder::step() adds 616 bytes and record_sample, unwind_context and on_sample_due about 460 more.
The timer vector has no IST: idt.cpp:136-138 gives one only to #DF, NMI and #MC. So a tick from ring 0 runs all of this on the interrupted thread's own kernel stack, which is 16 KiB (stack.hpp:16) and mapped by Stack::init with no guard reserved. 1.7 KiB of the cost is the remember stack, and none of the kernel's 1475 FDEs uses DW_CFA_remember_state.
The margin today is ample. The deepest stacks in the captured runs are about 3.7 KiB deep at the tick, plus roughly 5.6 KiB of tick path. But the sampler spends most of that tick path, in a context where an overflow silently corrupts the neighbouring mapping instead of faulting. Options for the user to decide between: a shallower or lazily filled remember stack, or a per-CPU scratch EvalState.

**Change**
The options are in Cause. Where they differ in what they cost or decide, the choice is the user's.

**Reference**
See Cause where it names one; otherwise none, this project's own code.

**Acceptance**
The scenario in Symptom no longer occurs, shown by a test where one can be written. Found by the audit of 2026-10-05 (unwinder-4, file:line as of commit 43adb3de, before the session moved to kernel/lib/thread_sampler).

## 3. FdeIndex::find misses a pc inside an outer FDE past a nested one, contrary to the header's overlap contract

**Severity**
Low: find() takes the rightmost entry with pc_begin <= pc and checks only whether that entry contains pc.

**Symptom**
Entries {[0x1000,0x1100), [0x1010,0x1020)} are sorted by pc_begin. find(0x1050) returns nullptr although the outer FDE covers it, so that pc gets NoFde, and per unwinder-1 its stack is recorded as complete at that frame. With {[0x1000,0x1010), [0x1000,0x1100)}, find(0x1005) returns the [0x1000,0x1100) entry, not the exact-range one the header promises.

**Cause**
find() takes the rightmost entry with pc_begin <= pc and checks containment for that entry only. So a pc that lies inside an outer range, but after a nested range's end, returns nullptr. For equal pc_begin it returns the last entry, which is the widest when sorted by (pc_begin, pc_end) ascending.
fde_index.hpp:39-42 tells callers that "overlapping ranges are resolved by first match, which favours exact-range entries", and find()'s own comment (lines 88-91) repeats it. Neither holds.
Not reached. The kernel's 1475 FDEs have no duplicate pc_begin and no overlap (checked against llvm-objdump --dwarf=frames), and the kernel sorts by pc_begin only.

**Change**
The options are in Cause. Where they differ in what they cost or decide, the choice is the user's.

**Reference**
See Cause where it names one; otherwise none, this project's own code.

**Acceptance**
The scenario in Symptom no longer occurs, shown by a test where one can be written. Found by the audit of 2026-10-05 (unwinder-5, file:line as of commit 43adb3de, before the session moved to kernel/lib/thread_sampler).

## 4. slot_address() multiplies a factored offset by the data alignment factor in int64 with no overflow check

**Severity**
Low: slot_address() multiplies two int64 values taken from CFI.

**Symptom**
A CIE or FDE carries DW_CFA_offset r16 with a factored offset of 0x2000000000000000 (ULEB128), and data_align is -8. step() calls slot_address(), and 0x2000000000000000 * -8 overflows int64, which is UB. A ubsan-trap kernel traps in the tick handler. A normal build computes some slot address, and the window check refuses it, or the optimiser assumes the overflow cannot happen.

**Cause**
rule.offset comes from a ULEB128 cast to int64 (lines 125, 179, 265) or from a SLEB128, and data_align is an arbitrary SLEB128. `rule.offset * cie.data_align` overflows int64 for a factored offset above 2^60 with data_align -8, which is undefined behaviour.
The rest of the arithmetic on CFI-derived values is unsigned and wraps harmlessly, and every byte read is bounds-checked, as the README promises.
Only a corrupted .eh_frame reaches this, and the kernel's .eh_frame is its own read-only data, so this is a robustness gap, not a live bug. Under the ubsan-trap variant (build/config/variants/BUILD.gn) it would become a trap inside the timer interrupt. Multiplying in uint64, or using __builtin_mul_overflow and returning BadCfi, closes it.

**Change**
The options are in Cause. Where they differ in what they cost or decide, the choice is the user's.

**Reference**
See Cause where it names one; otherwise none, this project's own code.

**Acceptance**
The scenario in Symptom no longer occurs, shown by a test where one can be written. Found by the audit of 2026-10-05 (unwinder-6, file:line as of commit 43adb3de, before the session moved to kernel/lib/thread_sampler).
