# irq-redesign: final six-requirement acceptance audit

**Date**: 2026-11-03

## Goal and decisions

The user requested a full IRQ design/implementation refactor, selected model B, suspended learning exercises and retained HIrqNum/VIrqNum spelling. Accepted current design: docs/design/261103/irq-redesign.md. Source delivery ends at 561afe94 on codex/irq-final-review; the final documentation commit records this audit. The active verification worktree is .worktrees/codex-irq-final-review, out/irq-final. Shared address edits and unrelated vendor deletions are excluded.

## Requirement evidence

| Requirement | Authoritative source evidence | Executed evidence | Verdict |
|---|---|---|---|
| 1. Meaningful names, objects and methods | Public Registration, ControllerOps/ControllerSource, DomainEndpoint and entry interface; private Line/Domain/directory; one ordinary control facade; no unused labels, initial-policy/factory duplication, root-only alias or empty init calls | Final source/caller audit; legacy-name and public-pointer searches; all nine suites and both images | Satisfied within the generic IRQ scope |
| 2. Required safety guards | line.cpp actual Guard at dispatch/revocation/control; line.hpp TA_GUARDED/TA_REQUIRES; private callback invocation; no analysis suppression or permanent bypass | Permanent handler observes held lock/preemption off; detach/revocation/control stress; compile-time thread-safety checks; lock-order/lifetime argument | Satisfied; global runtime lockdep class support remains an external infrastructure repair |
| 3. Independent reasoning | Accepted design explains owners, reader/writer phases, worst completion interleaving, receiving-CPU completion and nested frames; reference systems only cross-check roles | Readiness-gate removal fails both new ack assertions; clearing nested frame fails its assertion; exact fixed sources restored and pass | Satisfied |
| 4. Responsible extension | Distinct scoped HIrqNum/VIrqNum/vector types; per-instance context; validated protocol/capability; endpoint hides storage; unique ownership and reader-safe removal | Compile-time interface rejection; overlapping hardware source zero in two controllers; independent withdrawal; two handshake protocols over both triggers; concurrent readers/writers/removers | Satisfied for the declared contracts; unimplemented hardware support is not advertised |
| 5. Useful C++ instead of repetition | with_line structural constraint/std::invoke; immutable callback records and constexpr validation; existing Vec/Box ownership; explicit noncopyability and scope-bound EOI | Real compiler/tidy, domain transaction/refusal tests, unchanged control error contracts and all nine suites | Satisfied |
| 6. Consider alternatives and concepts | Accepted design compares manual/refcounted/unique ownership, repeated/macro/virtual/structural controls, token/revalidation/exclusion/reverse locking, controller flows, identity wrappers/enums and nesting strategies | Chosen alternatives are implemented; counterfactuals and fake adapters test the distinguishing properties | Satisfied |

## Final verification

- Source increments were verified before each isolated commit. Final source is 561afe94; no source edits follow its verification.
- LLVM 23 formatting/tidy ran against every touched C++ file. Both GN changes used the repository formatter with list order preserved. No new changed-line diagnostics remain.
- Both images pass: dbg 46 passed, 5 skipped, 0 failed; rel 44 passed, 8 skipped, 0 failed and 7/7 required. Host tests: 161 passed, 0 failed.
- Kernel suites: irq 82; irqcontroller 49; irqrevoke 37; irqregistry 85; irqdomain 87; irqrcu 34; interrupt 67; interruptline 83; pinlines 28. Each reports ok.
- Final verification logs: /tmp/irq-entry-final-verify.log and /tmp/irq-entry-final-tests.log. Persistent session logs: the ours-verify-irq-final-701ad0f5ed and ours-kshell-irq-final-701ad0f5ed temporary directories.
- Counterfactuals: readiness-gate removal fails both ack-window assertions; nested exit clearing fails one of 82 IRQ checks. Restored source hashes/bytes and positive rebuilds were checked. No failing variant is committed.

## Limits and independent work

Performance was not measured. ARM/RISC-V runtime, other physical controllers, MSI, dynamic affinity/vectors and unsupported handshake sequences are not claimed. Existing ISA override precedence and named lockdep class repairs remain separately scoped. The user's shared VirtAddr/PhysAddr migration is preserved and is not validated by this branch's x86 results. No Fair edits or unrelated vendor changes belong to this delivery. No implementation decision remains open within the accepted generic IRQ design.

Shared-checkout cleanup: the exact task-owned pending patch was moved to this branch and reversed from the original checkout only after its HEAD/path and reverse-application checks passed. Unchanged task-owned new files were removed there. The external types.hpp SHA-256 was identical before/after (45080379eb6b1d62b9fe775f775316d3eae4aec749caa1a982be9172c66f4f10). Kernel paths in the shared checkout have no remaining task-owned uncommitted edits; user type changes, unrelated vendor deletions and .mimosa remain preserved. The last two source commits and this audit are intentionally delivered on codex/irq-final-review, pending integration with that independent work.
