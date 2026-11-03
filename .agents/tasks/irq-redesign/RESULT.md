# irq-redesign: guarded IRQ core and explicit extension contracts delivered

**Date**: 2026-11-03

## Delivered

The accepted object model replaces the public descriptor/table scaffold with caller-owned Registration, immutable ControllerOps/ControllerSource, public DomainEndpoint and private Line/Domain/DomainRegistry. Ordinary controls share one reader scope; callbacks execute only through their guarded owner; storage is uniquely owned and transactionally published/retired. HIrqNum/VIrqNum retain the user's spelling as different scoped types. Controller handshake and electrical trigger are independent. Trap entry is separate and restores nested frames.

Commits: 29198710 vocabulary; fc11366b immutable policy validation; 8c19c7bf uniform Guard; 0786ae6b owning Domain; 3972cbb9 deferred completion revalidation; 3c669864 owning endpoint/directory; b6cda258 common control resolution; a4b6b080 controller contracts; 891797e9 distinct identities; 8a96f1e2 private storage/callback boundary; b109dca0 user-selected type spelling; 561afe94 nested entry fix. Design/review preceded implementation in 02d20ed6 and 76a75be5.

Final source branch: codex/irq-final-review, in .worktrees/codex-irq-final-review. It starts from the shared checkout's last verified IRQ commit, 8a96f1e2, and excludes independent address edits. Accepted design: docs/design/261103/irq-redesign.md. STATUS.md audits all six requirements against current source and executed tests.

## Verification

All changed C++ files ran repository changed-line formatting and actual LLVM 23 clang-tidy; the two GN files ran the repository formatter. No new changed-line diagnostics remain. Final out/irq-final build and both x86 boot images pass: dbg 46/5/0, rel 44/8/0 with 7/7 required, host tests 161/0. The source changes to entry touched 14 C++ files; earlier increments record their own complete file/check scope in docs/changes/261103/kernel-irq.md.

All nine kernel suites pass: irq.test 82, irqcontroller.test 49, irqrevoke.test 37, irqregistry.test 85, irqdomain.test 87, irqrcu.test 34, interrupt.test 67, interruptline.test 83, pinlines.test 28. These cover numeric and protocol rejection, callback/control lifetime, shared source-number isolation, receiving-CPU ACK/EOI ordering, typed syscall boundaries, domain retirement and nested frame/count restoration.

Removing only deferred readiness makes both new ack interleaving checks fail. Clearing instead of restoring the nested frame makes its new check fail. The exact fixed sources were restored and rebuilt before final positive verification. Logs: /tmp/irq-entry-final-verify.log, /tmp/irq-entry-final-tests.log; raw boot/kernel logs are in the ours-verify-irq-final-701ad0f5ed and ours-kshell-irq-final-701ad0f5ed temporary directories. Performance was not measured.

## Deviations from the design

The user retained HIrqNum/VIrqNum rather than the earlier prototype spellings; strong typing is preserved without aliases. Learning predictions and handwritten exercises were waived by the user. Entry bookkeeping was separated after the final responsibility audit exposed lost nested frames. The final two source increments use an isolated project-local worktree because the shared address-type migration is independently incomplete.

## Found along the way

Fixed invalid registration combinations, the Permanent guard bypass, old-ack/new-work completion, manual domain ownership, public raw storage access, unsupported controller declarations, source/id/vector confusion, unused invalid adapters and lost outer trap frames. Their reasons, alternatives and verification are recorded in the append-only change log.

Existing platform ISA override precedence and missing global lockdep class infrastructure remain in docs/repairs/261004/kernel-irq.md. The separate VirtAddr/PhysAddr migration currently prevents a shared-checkout full build; its edits were preserved, and this branch does not claim to verify them.

## Not done

No performance optimization or measurement, new physical controller, MSI, dynamic vector/affinity support, global lockdep implementation, platform firmware arbitration fix, address migration or integration of independent dirty shared work. Those are not implied by the declared generic protocols. The delivered IRQ source and all six acceptance requirements are complete within the accepted scope; integration with independent address work remains a separate operation.

Shared-checkout cleanup: the exact task-owned pending patch was moved to this branch and reversed from the original checkout only after its HEAD/path and reverse-application checks passed. Unchanged task-owned new files were removed there. The external types.hpp SHA-256 was identical before/after (45080379eb6b1d62b9fe775f775316d3eae4aec749caa1a982be9172c66f4f10). Kernel paths in the shared checkout have no remaining task-owned uncommitted edits; user type changes, unrelated vendor deletions and .mimosa remain preserved. The last two source commits and this audit are intentionally delivered on codex/irq-final-review, pending integration with that independent work.
