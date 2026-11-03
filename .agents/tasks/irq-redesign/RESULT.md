# irq-redesign: review and proposed design delivered

**Date**: 2026-11-03

## Delivered

Documentation-only review of the IRQ line module and a Proposed first design cut. REVIEW.md records source-backed findings and retained guarantees. The user accepted DESIGN.md D1 (b) on 2026-11-03 by answering "B": Line, Registration, Domain and ControllerOps, with Domain owning mapping, storage and retirement. Concrete Interfaces remain Proposed. PLAN.md records this round's scope. No implementation commits or C++ changes were made.

The invalid-registration policy defect is fixed and its repair record removed; the change and validation are in `docs/changes/261103/kernel-irq.md`. The deferred-completion protocol is implemented by owner-state revalidation; deterministic tests demonstrate the old synthetic failure and verify the fix. This is not reported as a reproduced real-device failure.

## Verification

Used isolated `out/irq-design-review` to avoid another session's output. All tools came from the repository's prebuilt tree.

- LLVM 23 `clang-format --dry-run -Werror`: all six implementation files and nine headers pass, with no source edits.
- LLVM 23 clang-tidy: all 15 scoped IRQ files exit 0 against the fresh build's compile database. IRQ header diagnostics were included using `--header-filter=.*ours/irq/.*`; no IRQ-local warnings or errors were emitted. Existing transitive-header diagnostics were suppressed by that filter. Logs and the JSON summary are in `/tmp/ours-irq-design-review/`.
- The first tidy attempt used an older compile database and found a missing generated vDSO header. Re-running against the newly built output resolved that environment issue. It is not an IRQ finding.
- The first sandboxed build could not write the configured ccache temporary file. The authorized rerun with cache access succeeded; no cache configuration or source change was needed.

`python3 .agents/skills/verify/scripts/boot_check.py --out out/irq-design-review`:

```text
build      ok (out/irq-design-review, 8.8s)
dbg        PASS  shell finished at 5.7s · 46 passed, 5 skipped, 0 failed · 0 error line(s) · reached: end
rel        PASS  shell finished at 5.0s · 44 passed, 8 skipped, 0 failed · 7/7 required · 0 error line(s) · reached: end
host-tests PASS  161 passed, 0 failed · 12.4s
```

`python3 scripts/kshell_run.py --out out/irq-design-review irq.test irqchip.test irqrevoke.test irqregistry.test interrupt.test interruptline.test` exited 0:

```text
irq.test: ok -- 72 checks
irqchip.test: ok -- 18 checks
irqrevoke.test: ok -- 37 checks
irqregistry.test: ok -- 66 checks
interrupt.test: ok -- 67 checks
interruptline.test: ok -- 78 checks
```

Boot logs: `/var/folders/n_/20ryw2px3052ggtjf0xpsc8h0000gn/T/ours-verify-irq-design-review-96943ff4fb/`. Kernel-suite log: `/var/folders/n_/20ryw2px3052ggtjf0xpsc8h0000gn/T/ours-kshell-irq-design-review-96943ff4fb/dbg.log`.

D1 acceptance recheck on 2026-11-03, using the same isolated output:

```text
build      ok (out/irq-design-review, 2.5s)
dbg        PASS  shell finished at 5.8s · 46 passed, 5 skipped, 0 failed · 0 error line(s) · reached: end
rel        PASS  shell finished at 4.9s · 44 passed, 8 skipped, 0 failed · 7/7 required · 0 error line(s) · reached: end
host-tests PASS  161 passed, 0 failed · 1.0s
```

Performance: not measured. Real-controller delivery during the old-ack/new-dispatch window: not verified. Non-x86 controller protocols: not verified.

## Deviations from the design

None. D1 was explicitly accepted by the user. The overall design remains Proposed while D2 and D3 are unresolved; no concurrency mechanism was silently selected.

## Found along the way

- Invalid Permanent plus Shared on an empty line is fixed by immutable policy validation before attachment; both forms and unknown bits are covered by irq.test.
- The old thread-context preemption repair describes behavior superseded by the current IrqRcuReadGuard in the public test-dispatch facade. It was not deleted during this review.
- Owning Span fields, repeated public controls, weak number types, and controller-protocol assumptions are design findings in REVIEW.md. They were not opportunistically changed.

## Not done

The user has suspended learning-oriented arguments and authorized development toward the full active goal. Naming, registration validation, uniform guards, owning Domain storage and completion revalidation are verified. Remaining endpoint/facade, registry ownership, numeric identity and controller capability work is tracked in STATUS.md. No performance improvement is claimed.
