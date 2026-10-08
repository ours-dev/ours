# prof outstanding repairs

## 1. SamplerDumpHeader::num_dropped silently wraps: truncating cast of a u64 sum to u32

**Severity**
Low: The truncation is real and silent:

- make_header() stores static_cast<u32>(totals.dropped).

**Symptom**
A 4 MiB-per-CPU buffer fills in about 4.6 minutes at the 100 Hz tick, and every later tick on that CPU is a drop. A 4-CPU session left running reaches 2^32 drops after about 124 days at 100 Hz (about 31 days on a 16-CPU build). Once HpTick makes the 10 us minimum period real (100 kHz per CPU), it takes about 3 hours on 4 CPUs. sampler_read then reports a small num_dropped for a session that lost billions of samples.

**Cause**
make_header() stores static_cast<u32>(totals.dropped), where totals.dropped is the u64 sum of every CPU's (write_index - capacity). The ABI documents the field as "Samples a full buffer had no room for, summed over the CPUs" and gives the reader no way to see that it wrapped. num_records cannot overflow (at most 441,504), but drops grow without bound for as long as a session with a full buffer keeps running. Saturate at UINT32_MAX, or widen the field in the version-2 header the open questions already plan.

**Change**
The options are in Cause. Where they differ in what they cost or decide, the choice is the user's.

**Reference**
See Cause where it names one; otherwise none, this project's own code.

**Acceptance**
The scenario in Symptom no longer occurs, shown by a test where one can be written. Found by the audit of 2026-10-05 (syscall-security-3, file:line as of commit 43adb3de, before the session moved to kernel/lib/thread_sampler).

## 2. Known #4 is misdescribed: the frame-pointer walk drops the caller at any function's entry or ret, not only in leaves without a frame pointer

**Severity**
Low: walk_frame_pointers() records pc, then [fp+8] at each level.

**Symptom**
A user function f is called from g, both compiled with frame pointers, and the tick lands at f+0 or at f's ret. sample_user_stack() walks from pc=f and fp=g's rbp, and records [f, return address into g's caller]. g is missing and no flag says so, and the ABI comment does not warn about this case.

**Cause**
walk_frame_pointers() reads the return address at fp+8. rbp still holds the caller's frame pointer in two windows of every function: from its first instruction until its `mov rbp, rsp`, and again from its `pop rbp` or `leave` until its `ret`. In those windows the walk records, right after the pc, the caller's return address (a pc in the grand-caller), so the caller itself is skipped. This holds for every function, with or without a frame pointer.
The documentation a reader uses to interpret FromUser records describes only the leaf case: system/public/ours/sampler.hpp:84 ("A function built without one leaves its caller out") and prof/sampler.hpp:216.
Ticks at a function's first byte are common here: 39 of the 629 kernel records in the captured runs sit at one, including ProcessDispatcher::current+0 ten times and x86_user_copy+0 twice.

**Change**
The options are in Cause. Where they differ in what they cost or decide, the choice is the user's.

**Reference**
See Cause where it names one; otherwise none, this project's own code.

**Acceptance**
The scenario in Symptom no longer occurs, shown by a test where one can be written. Found by the audit of 2026-10-05 (unwinder-7, file:line as of commit 43adb3de, before the session moved to kernel/lib/thread_sampler).

## 3. prof-fold never checks records against the header: losses go unreported, a newer format is silently empty, and several dumps merge under the last load base

**Severity**
Low: Reproduced each case with the built prof-fold.

**Symptom**
I ran the built prof-fold on crafted dumps. Header num_records 5 with one good, one truncated and one corrupted SAMPB line: it prints 'prof-fold: 5 samples (0 dropped)' and folds one stack. A version-2 dump with record_size 160: '3 samples', empty output, exit 0. Two dumps with load bases 0xffffff8840000000 and 0xffffff8860000000: the first dump's _start sample prints as '0xffffffffe008c6c4', and the summary says '1 samples'.

**Cause**
parse_dump() checks only the header's magic. It ignores version, header_size and record_size, although sampler.hpp:133-134 promises that 'a reader can step over what it does not know'. A record must be exactly 152 bytes, and rejected lines are dropped without a count. The summary prints the header's num_records, not what was decoded. Every PROF1 line replaces the header, so records from several dumps are all resolved with the last header's load_base. And one PROF1 line that fails to decode, for example because of a trailing space, aborts the whole parse with 'no PROF1 header found'.

**Change**
The options are in Cause. Where they differ in what they cost or decide, the choice is the user's.

**Reference**
See Cause where it names one; otherwise none, this project's own code.

**Acceptance**
The scenario in Symptom no longer occurs, shown by a test where one can be written. Found by the audit of 2026-10-05 (tools-tests-8, file:line as of commit 43adb3de, before the session moved to kernel/lib/thread_sampler).

## 4. prof-fold's section-table bounds check wraps, so a crafted ELF crashes it

**Severity**
Low: Reproduced.

**Symptom**
Feed prof-fold a 1 MiB ELF with e_shoff = 2^64 - 0x4000*64 and e_shnum = 0x4000. The sum wraps to 0, so the check passes, shdrs points 1 MiB before the buffer, and the first section-header read takes SIGBUS (exit 138 on this host). Verified with the built prof-fold and scratch wrap.elf.

**Cause**
The section-table bounds check, `e_shoff + e_shnum * sizeof(ElfSectionHeader) > m_bytes.size()`, overflows when e_shoff is near 2^64. shdrs is also formed before the check. Known item 9 describes the section table's bounds as checked; they are not checked reliably. Host tool, malformed input only.

**Change**
The options are in Cause. Where they differ in what they cost or decide, the choice is the user's.

**Reference**
See Cause where it names one; otherwise none, this project's own code.

**Acceptance**
The scenario in Symptom no longer occurs, shown by a test where one can be written. Found by the audit of 2026-10-05 (tools-tests-10, file:line as of commit 43adb3de, before the session moved to kernel/lib/thread_sampler). Also reported as claims-5.

## 5. profile and check_sampler size their dump buffers for 16 CPUs, while MP_MAX_CPUS is a GN argument

**Severity**
Low: The mismatch is real.

**Symptom**
Build with mp_max_cpus = 32 and boot with SMP=32. Idle CPUs fill their 64 records each, so dump_size = 32 + 32*64*152 exceeds sizeof(g_dump): profile prints 'profile: sampler_read = BufferTooSmall' and exits 1, and the shell reports 'profile checks FAILED'. In check_sampler, about 32 x 4-8 records exceed 16 x 8, so `needed <= sizeof(dump)` and the final read fail: 'sampler checks FAILED'.

**Cause**
profile's g_dump (MAX_CPUS = 16) and check_sampler's `static u8 dump[HEADER_SIZE + 16 * RECORDS_PER_CPU * RECORD_SIZE]` copy the default of mp_max_cpus, which is a declare_args value in kernel/kernel_params.gni:5. Nothing ties the two together. A machine with more CPUs than the buffers allow cannot cause an overflow: Session::read answers BufferTooSmall when capacity < dump_size, and the shell's one-byte-short read gets the same answer. But both checks report a false failure.

**Change**
The options are in Cause. Where they differ in what they cost or decide, the choice is the user's.

**Reference**
See Cause where it names one; otherwise none, this project's own code.

**Acceptance**
The scenario in Symptom no longer occurs, shown by a test where one can be written. Found by the audit of 2026-10-05 (tools-tests-9, file:line as of commit 43adb3de, before the session moved to kernel/lib/thread_sampler).

## 6. No end-to-end check notices a broken kernel unwind or user walk; the kernel halves of 311b0275 and 5b47eaf5 are unguarded

**Severity**
Low: Real coverage gap.

**Symptom**
Drop the address argument at kernel/lib/thread_sampler/src/thread_sampler.cpp:74 (Section address 0), or pass stack_base/stack_top = 0 at line 60. Every kernel record is now one frame deep (NoFde, or the empty-window Truncated). Yet profile still prints 'N kernel samples, M user samples' and exits 0, check_sampler still sees records > 0, and ours-verify reports dbg PASS. The same holds for a walk_frame_pointers() regression that returns only the pc.

**Cause**
profile exits 0 when at least one record has FromUser and at least one does not. check_profile passes on exit 0, and check_sampler only requires records > 0. Nothing looks at depth, the Truncated flag or tid. With SMP=4, idle CPUs record a kernel sample every tick, so the kernel condition holds even if profile's syscalls are never sampled. check_sampler's doc promises 'at least one sample from a loop of system calls', but only the count is checked. No host test reaches kernel code. So the kernel-side wiring that took kernel stacks from one frame to eleven has no automated guard: the stack bounds in interrupted_context() (311b0275) and Section::address = __eh_frame_start in kernel_eh_frame() (5b47eaf5). The 908/908, 150/150 and four-user-frame evidence in RESULT.md was gathered by hand.

**Change**
The options are in Cause. Where they differ in what they cost or decide, the choice is the user's.

**Reference**
See Cause where it names one; otherwise none, this project's own code.

**Acceptance**
The scenario in Symptom no longer occurs, shown by a test where one can be written. Found by the audit of 2026-10-05 (tools-tests-2, file:line as of commit 43adb3de, before the session moved to kernel/lib/thread_sampler).

## 7. Profiler shell checks pass vacuously on dbg when the shell has no SAMPLING resource

**Severity**
Low: Traced.

**Symptom**
A change drops the SAMPLING handle from userboot's handoff (userboot.cpp:334-338). The dbg log then shows 'shell: profile checks skipped (no SAMPLING resource)', 'shell: sampler sessions skipped ...' and 'shell: sampler checks passed', and boot_check prints 'dbg PASS ... 31 passed, 4 skipped, 0 failed'.

**Cause**
Without SAMPLING, check_profile prints 'profile checks skipped' and returns. check_sampler skips its whole session half, yet still prints 'sampler checks passed' when the resource-free probes pass. userboot reports a missing resource only as 'userboot: started without the sampling resource', with no FAILED or error tag. boot_check has REQUIRED["dbg"] = [] and counts 'skipped' lines without letting them affect the verdict. dbg already shows two routine skips (launcher service, spawn), so two more blend in. A regression in minting SAMPLING or handing it to the dbg shell would turn off every profiler check while ours-verify reports PASS.

**Change**
The options are in Cause. Where they differ in what they cost or decide, the choice is the user's.

**Reference**
See Cause where it names one; otherwise none, this project's own code.

**Acceptance**
The scenario in Symptom no longer occurs, shown by a test where one can be written. Found by the audit of 2026-10-05 (tools-tests-4, file:line as of commit 43adb3de, before the session moved to kernel/lib/thread_sampler).

## 8. prof-fold has no tests: 5089fd28, fd51ccb9's prof-fold half and the SAMPB path are unguarded, and format_test's hex tests only test themselves

**Severity**
Low: Real gap, partly acknowledged already.

**Symptom**
Revert 5089fd28 (main.cpp:381 back to `for (usize i = 0; i < record.depth; ++i)`): every flame graph comes out upside down. Or drop `- static_cast<u64>(i != 0)` at line 387: a caller whose call ends the function is named after the next function. Or break base64_decode's padding: every SAMPB record is silently dropped. In each case all 46 host tests and both boots still pass.

**Cause**
prof-fold is a single executable whose parse and fold logic sits in main.cpp's anonymous namespace. No test target links it, and no script runs it on a boot log. Nothing therefore checks root-first output (5089fd28), naming at ip - 1 (fd51ccb9), or the SAMPB format actually in use (profile.cpp's base64 encoder and main.cpp's base64_decode). The README says format_test does 'a hex round-trip -- the contract the host parser depends on'. In fact FormatLayout.HexLineRoundTrip and HexDecodeRejectsGarbage run the test file's own hex_encode/hex_decode helpers. They exercise neither prof-fold's decoder (which, unlike them, accepts uppercase) nor the kernel's write_hex_line, so no product change can make them fail.

**Change**
The options are in Cause. Where they differ in what they cost or decide, the choice is the user's.

**Reference**
See Cause where it names one; otherwise none, this project's own code.

**Acceptance**
The scenario in Symptom no longer occurs, shown by a test where one can be written. Found by the audit of 2026-10-05 (tools-tests-5, file:line as of commit 43adb3de, before the session moved to kernel/lib/thread_sampler).

## 9. Sampler::unwind_context's stepping loop never runs in a host test, so 82872e1f's sampler change and the RA-undefined branch can be reverted silently; one StackWindow test never reaches its check

**Severity**
Low: All three parts hold.

**Symptom**
Change sampler.cpp:49 back to `if (step != unwind::StepResult::NoFde)`: every complete kernel-thread stack, which ends at a zero return slot, is marked Truncated again, yet sampler still passes 22/22 and unwind 9/9. Or delete `if (m_hi - m_lo < sizeof(u64)) return false;` from StackWindow::contains(): on_stack(0, 0, 4).contains(0) would accept an 8-byte read past the window's top, and the StackWindow tests still pass.

**Cause**
SamplerTest::SetUp gives the sampler no .eh_frame or FDE index, and the only kernel-sample test uses an empty window. So the loop's Ok, End, NoFde, BadFrame and depth-cap branches never run on the host. 82872e1f's 'End is not Truncated' condition is unguarded. So is the unwinder's DW_CFA_undefined-on-RA -> End branch (unwinder.cpp:420-423), which nothing reaches today because the kernel's .eh_frame has no DW_CFA_undefined. Separately, StackWindow.AWindowNarrowerThanAWordContainsNothing (sampler_test.cpp:350-357) passes even with contains()'s size check deleted. At TOP = 0x7000'4000, `m_hi - 8` cannot underflow, so both probes are refused by the range test anyway. The case the check exists for, a window whose top is below 8, is never built.

**Change**
The options are in Cause. Where they differ in what they cost or decide, the choice is the user's.

**Reference**
See Cause where it names one; otherwise none, this project's own code.

**Acceptance**
The scenario in Symptom no longer occurs, shown by a test where one can be written. Found by the audit of 2026-10-05 (tools-tests-6, file:line as of commit 43adb3de, before the session moved to kernel/lib/thread_sampler).

## 10. No test would fail if 89b0d6e4, ed09eb11 or 7a105fe4 were reverted

**Severity**
Low: Verified from the diffs.

**Symptom**
Revert ed09eb11. sampler_create with buffer_size = 100 MiB is accepted again and, per that commit, hangs the creator on the 512 MiB dbg VM. Meanwhile 'sampler_create(buffer too large)' with 1 GiB + 1 still gets InvArgs, and dbg PASSes.

**Cause**
ed09eb11: the only buffer-size check probes SAMPLER_MAX_BUFFER_SIZE + 1 through the symbol, so putting the constant back to 1 GiB still passes. Nothing pins the bound against the 512 MiB dbg VM, such as a create at 4 MiB plus one record expecting InvArgs, or a static_assert on MAX_BUFFER_SIZE x MP_MAX_CPUS. 89b0d6e4: the deadlock needed allocate() to fail while the guard was held, and nothing injects that failure. The old code's AlreadyExists path never built a session, so the shell's 'sampler_create(a second)' passes before and after the fix. It does catch a reordering of the new code, which would deadlock there. 7a105fe4: no test marks a thread under one session and acts on the mark under the next, so reverting the generation compare at kernel/lib/thread_sampler/src/thread_sampler.cpp:443 passes everything.

**Change**
The options are in Cause. Where they differ in what they cost or decide, the choice is the user's.

**Reference**
See Cause where it names one; otherwise none, this project's own code.

**Acceptance**
The scenario in Symptom no longer occurs, shown by a test where one can be written. Found by the audit of 2026-10-05 (tools-tests-7, file:line as of commit 43adb3de, before the session moved to kernel/lib/thread_sampler).

## 12. 65611df1's change record misstates which sampler_read sentence it changed

**Severity**
Low: 65611df1's diff to syscalls.gni changed only the fault sentence, adding ', if any were'.

**Symptom**
A program author who trusts the change record treats actual == 0 after BufferTooSmall as possible and adds a retry path for it. The case never happens, and the record contradicts both syscalls.gni and sampler.cpp:355-357.

**Cause**
The record's Files list says 'sampler_read's BufferTooSmall case says |actual| may be 0'. The diff actually changed the fault sentence, to 'with |actual| at the bytes copied before it, if any were'. The BufferTooSmall text is unchanged and correct: Session::read always sets *actual = needed before returning BufferTooSmall, and the syscall copies it out. A reader of this learning document comes away believing BufferTooSmall can leave |actual| at 0.

**Change**
The options are in Cause. Where they differ in what they cost or decide, the choice is the user's.

**Reference**
See Cause where it names one; otherwise none, this project's own code.

**Acceptance**
The scenario in Symptom no longer occurs, shown by a test where one can be written. Found by the audit of 2026-10-05 (claims-6, file:line as of commit 43adb3de, before the session moved to kernel/lib/thread_sampler).
