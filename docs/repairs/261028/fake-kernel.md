# fake-kernel outstanding repairs

## 1. The fake kernel's `memory ranges` line runs into its value

**Severity**
Low: a report line is harder to read; no check depends on it.

**Symptom**
Every fake-image boot, x86 and riscv, prints `[fake-kernel] memory ranges0x0000000000000012` (riscv: `...0x000000000000000f`), where every other line has a space between label and value, as in `[fake-kernel] kernel at    0x000000008ff82000`.

**Cause**
`fake_kernel_main()` (`kernel/tests/fake-kernel/fake_kernel.cpp`) pads each label by hand to the same width so the values line up: `"[fake-kernel] kernel at    "` is the label, three spaces, and the space before the value. `"memory ranges"` is the longest label and exactly that width, so it got no separating space, and `write_line()` appends the value directly.

**Change**
Widen every label by one space so the longest one keeps a separator (`"[fake-kernel] memory ranges "`, the others one space longer), or let `write_line()` write the space itself and drop the trailing padding. `riscv_boot.py`, the only script that reads these lines, matches only `entered`, `FAIL:` and `handoff ok`, so it does not change.

**Reference**
None, this project's own design: a test kernel's report.

**Acceptance**
A fake-image boot prints `[fake-kernel] memory ranges 0x...`, its values aligned with the other lines; the riscv check reaches `handoff ok` and x86 `ours-verify` passes.
