# prof-sampler-object: a Sampler object user space creates, with kernel and user stacks

**Date**: 2026-10-05

## Delivered

The sampling profiler is a kernel object a program holding the SAMPLING resource creates and drives (`sampler_create`, `sampler_start`, `sampler_stop`, `sampler_read`), sampling kernel stacks in the timer interrupt and user stacks on the interrupted thread's way back to user space. While no session exists nothing runs and nothing is allocated but the per-CPU timers, which the first start allocates and every later session reuses; the kernel shell's `prof.*` commands are gone. `examples/profile` drives it, and `prof-fold` folds what it prints.

| Step | Commit | What |
|---|---|---|
| 1 | `8e7557f1` | per-CPU sampling timers replace the scheduler-tick hook |
| 2 | `a52bd7ea` | the `prof::arch` seam and the PMU source go; the kernel hands its facts in |
| | `0c8710ec` | the comment rule applied to the sampler and `faults.cpp` |
| 3 | `01d32768` | the SAMPLING system resource (slot 15), minted for the first process |
| 4 | `97a891f2` | a record buffer's depth is set at run time |
| 4 | `69678910` | `ours::sampler::Session` owns its sampler, buffers and index from create on |
| | `d25c6bc9` | the upstream research folded in; D7 to D9 decided |
| 4 | `50d9a4ce` | `SamplerDispatcher` and the four system calls |
| 5 | `53f9cbf5`, `0d9a1be3` | the dump's structures move to `<ours/sampler.hpp>` |
| 5 | `06624fad` | `examples/profile`; the kernel shell's prof commands retire |
| 6 | `d3bacb99`, `61638461` | user stacks, walked by frame pointers in `process_pending_signals()` |
| after | `311b0275` | the kernel unwind reads only the interrupted thread's stack, through a tested `StackWindow` |
| after | `5b47eaf5`, `74e2a5e4`, `c1ee0984`, `fd51ccb9`, `82872e1f` | five unwinder fixes: kernel stacks unwind in full |
| after | `89b0d6e4`, `3d7d1953`, `ed09eb11`, `7a105fe4` | a failed create no longer deadlocks; a maximum period; a lower buffer limit; marks stay with their session |
| after | `5089fd28` | `prof-fold` writes stacks root first, as flame graphs read them |
| after | `refactor(sampler)`, 2026-10-05 | the session moves to the kernel library `kernel/lib/thread_sampler`, namespace `sampler` (D6, the user's call) |

## Verification

- Host: `//kernel/lib/prof` sampler tests 22/22 and format tests 4/4, `//kernel/lib/unwind` unwind tests 9/9 and cfi tests 11/11, at the last commit.
- Boot: `boot_check.py --out out/sampler` after every commit; at the last commit dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required).
- System calls: `shell: sampler checks passed` on dbg covers every call and refusal, including one session at a time across a duplicated handle, BadState in each wrong state, the read's size query and BufferTooSmall, and a create succeeding after the last handle of a running session was closed; on rel the checks that need no resource pass.
- End to end: on dbg, `profile: 56 kernel samples, 8 user samples, 0 dropped` when the workload runs at normal speed, and `236 kernel samples, 20 user samples, 4043 dropped` with both kinds kept when it lands on a slow CPU (`docs/repairs/261005/kernel-top.md` item 1). `prof-fold` folds kernel records to whole stacks, system calls down to `x86_syscall` and kernel threads to `Thread::trampoline`, every return address following a `call` in `vmours` (908 of 908 and 150 of 150 in the two runs above); user records go four frames deep (`spin_inner` → `main` → `ort_start` → `_start` against `bin/profile`).
- Multi-CPU: a kernel-shell session on 4 CPUs (step 4) sampled on all four and stopped cleanly.
- Not measured: the cost of building the unwind index at create; the sample path's cost.

## Deviations from the design

- D7, D8 and D9 took the recommendations without the user's invariant argument or prediction: the user asked on 2026-10-04 for the steps to go ahead without questions.
- Steps 4 and 5 landed as several commits each (PLAN says which), so that each change built, booted and was recorded on its own.
- The dump's structures became part of `system/public`, which the design did not plan: `sampler_read` writes them into user memory, so they are system interface.
- A program prints records as `SAMPB <base64>`, not `SAMP <hex>`: a hex record line does not fit one 256-byte `debug_write`.
- A kernel-only session skips user-mode ticks instead of recording a one-frame user pc.
- Ticks no target records are not counted, which the design had planned for step 6: the dump header has no field for it (DESIGN, Open questions).
- The handle's rights follow upstream: Transfer, Duplicate and Inspect, with no right asked by any call.

## Found along the way

- Kernel samples unwound no further than the interrupted pc. It was five unwinder bugs (`docs/changes/261005/unwind.md`), fixed after the task: kernel stacks now run to their thread's first frame.
- CPU 0's idle thread records a stack it never runs on, so its samples are not unwound: `docs/repairs/261005/kernel-task.md` item 1.
- Timer samples in the dbg image pile up on the instruction after the `popfq` ending `SerialLogger::write()`'s interrupts-off section: the blind spot the design predicts, measured (change record of 2026-10-04, step 2).
- `kshell_run.py` loses the shell prompt to other CPUs' log lines on multi-CPU dbg, about one try in three; the script already says so.

## Not done

- On dev, init serves the SAMPLING resource as `ours.resource.Sampling` (2026-10-05), but `bin/profile` still takes it as a startup handle, so it profiles only on dbg_1 (DESIGN, Open questions).
- User frames are not symbolised, and a record carries no process identity (DESIGN, Open questions).
- A leaf function's caller is missing from user stacks (`-momit-leaf-frame-pointer`; DESIGN, Open questions).
- Streaming reads (D9) and the PMU as a facility of its own (non-goals).
- The timer's resolution is one tick, whatever the period (DESIGN, Open questions; fixed by HpTick, elsewhere).
- Typed commands get no SAMPLING resource, so nobody can profile interactively on dbg either; only the shell's own check hands it on.
- Tests not written: a read racing the last close, and the stop barrier against a writer on another CPU. Closing the last handle of a running session is covered by one boot check, not by stress.
- `DW_CFA_set_loc` is read as a fixed 8 bytes rather than in the CIE's pointer encoding. Latent: the kernel's `.eh_frame` has none.
- prof-fold checks that an ELF's section table fits the file but not its symbol table or strings, so a malformed file can make it read out of bounds. Host tool, low.
- D7 to D9 still want the user's own argument, and the milestone's hand-written function (`walk_frame_pointers()`, offered as the exercise) has not been written.
