# riscv-boot outstanding repairs

## 1. Nothing shows that the riscv boot chain runs under an SBI implementation other than OpenSBI

**Severity**
Low: a missing test. The rule that phys and the kernel know the firmware only through the SBI specification and the device tree (`ours-kernel-design`, `references/boot.md`, section Firmware) is held today by reading the code, not by a boot.

**Symptom**
Latent, cannot be triggered today. A dependence on OpenSBI beyond the specification would only show when another implementation boots the image: a legacy extension it leaves out, a firmware region assumed rather than read from `/reserved-memory`, a behaviour the specification does not promise.

**Cause**
`riscv_boot.py` boots only with QEMU's `-bios default`, which is OpenSBI. A claim that code depends only on an interface can be checked only against a second implementation of that interface; one implementation passing shows only that the code works with that one.

**Change**
Fetch a released RustSBI build for QEMU's `virt` machine into `prebuilt/` (official upstream releases only, as for every toolchain here), and give `riscv_boot.py` a firmware choice: OpenSBI by default, RustSBI on request, each with its own banner as the first stage's pattern and its own fatal-trap message as a failure pattern. Check first whether that build implements the legacy console extension, the one call in use that a conforming firmware may leave out (`kernel/arch/riscv/lib/arch/include/arch/riscv/sbi.hpp`); if it does not, that is the finding, and the fix is the debug console extension probed through the base extension. Ruled out: building RustSBI from source, which needs a Rust riscv toolchain the repo does not otherwise have.

**Reference**
Linux `sbi_init()` (`arch/riscv/kernel/sbi.c`) reads the specification version and probes each extension it uses through the base extension, falling back to the legacy calls only when built with `CONFIG_RISCV_SBI_V01`.

**Acceptance**
`riscv_boot.py` with the RustSBI choice reaches the same stage as with OpenSBI, `[fake-kernel] handoff ok` once the integration boot passes; x86 `ours-verify` still passes.
