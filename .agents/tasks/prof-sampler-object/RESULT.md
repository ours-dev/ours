# prof-sampler-object: a Sampler object user space creates, with kernel and user stacks

**Date**: 2026-10-05

## Delivered

The sampling profiler is a kernel object a program holding the SAMPLING resource creates and drives (`sampler_create`, `sampler_start`, `sampler_stop`, `sampler_read`), sampling kernel stacks in the timer interrupt and user stacks on the interrupted thread's way back to user space. Nothing is allocated and nothing runs while no session exists, and the kernel shell's `prof.*` commands are gone. `examples/profile` drives it, and `prof-fold` folds what it prints.

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

## Verification

- Host: `//kernel/lib/prof` sampler tests 17/17, format tests 4/4, at the last step.
- Boot: `boot_check.py --out out/sampler` after every commit; at the last step dbg PASS (31 passed, 2 skipped), rel PASS (27 passed, 9 skipped, 2/2 required).
- System calls: `shell: sampler checks passed` on dbg covers every call and refusal, including one session at a time across a duplicated handle, BadState in each wrong state, the read's size query and BufferTooSmall, and a create succeeding after the last handle of a running session was closed; on rel the checks that need no resource pass.
- End to end: on dbg, `profile: 74 kernel samples, 17 user samples, 0 dropped`; `prof-fold` folds the kernel records to kernel functions and the user records four frames deep (`spin_inner` → `main` → `ort_start` → `_start` against `bin/profile`).
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

- Kernel samples unwind no further than the interrupted pc: the unwinder finds no FDE for a pc that `.eh_frame` covers. `docs/repairs/261004/prof.md` item 1.
- Timer samples in the dbg image pile up on the instruction after the `popfq` ending `SerialLogger::write()`'s interrupts-off section: the blind spot the design predicts, measured (change record of 2026-10-04, step 2).
- `kshell_run.py` loses the shell prompt to other CPUs' log lines on multi-CPU dbg, about one try in three; the script already says so.

## Not done

- A program on dev cannot get the SAMPLING resource: init holds it and serves it to nobody (DESIGN, Open questions).
- User frames are not symbolised, and a record carries no process identity (DESIGN, Open questions).
- A leaf function's caller is missing from user stacks (`-momit-leaf-frame-pointer`; DESIGN, Open questions).
- Streaming reads (D9) and the PMU as a facility of its own (non-goals).
- The timer's resolution is one tick, whatever the period (DESIGN, Open questions; fixed by HpTick, elsewhere).
