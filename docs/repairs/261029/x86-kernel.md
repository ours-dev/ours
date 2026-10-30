# x86-kernel outstanding repairs

Found on 2026-10-29, the first time a machine with RAM above 4 GiB got through phys: before then the boot shim mapped 1 GiB and physload took its page tables from outside it, so no such machine ever reached the kernel. See `docs/changes/261029/phys.md`.

## 1. Preparing the AP trampoline fails an ASSERT on any machine with RAM above 4 GiB

**Severity**
High. No machine with RAM above 4 GiB boots -- on QEMU's q35 that is anything over 2816M -- and both images stop on a hard `ASSERT`, not a debug one.

**Symptom**
`MEM=3072`, `MEM=4096` and `MEM=8192` all fail on both images with `error-level line (panic or failed ASSERT) ... reached: kernel`; `MEM=2560` and below pass. The kernel gets as far as the timers, then:

```text
[trace] Bootstrap PGD at 0x7FEDB000
[trace] Bootstrap16 data at v:0xFFFFFF800000E000, p:0xE000

OURS KERNEL PANIC
[cpu0 error] Assertion failed on Status::Ok == status
```

No backtrace is printed.

**Cause**
Not diagnosed. The two lines above are the last things `make_bootstrap_area()` in `kernel/arch/x86/kernel/bootstrap.cpp` logs before two consecutive identity maps into the kernel aspace, both with `ArchMapOptions::ErrorIfExisting` and both followed by `ASSERT(Status::Ok == status)`: the AP trampoline buffer at `BOOTSTRAP16_BUFFER_BASE`, below 64 KiB, and the page holding the bootstrap GDT. One of them fails. Which one, and with what status, is the first thing to find -- the message does not say, and nothing else in the log does.

What is known: the boundary sits exactly where q35 starts putting RAM above the 4 GiB hole (2560M passes, 3072M fails), so the trigger is RAM above 4 GiB rather than the amount of RAM. And `Bootstrap PGD` is the kernel aspace's root (`fill_bootstrap_tables()` reads it from `pgd_phys()`), and its value, `0x7FEDB000`, is the address at which physload allocated its own root on the same machine: the kernel aspace still uses physload's root table, and so whatever physload's identity map left beneath it. Plausible but unverified: physload's identity map of usable RAM lays out the low tables differently once there is RAM above 4 GiB, and a mapping the trampoline expects to add already exists, or exists as a large page that cannot be split.

**Change**
None proposed until the failing call and its status are known. Break on the panic under the lldb MCP server, or log `status` before each `ASSERT`, on `MEM=3072`, and compare the kernel aspace's lower-half entries with `MEM=2560`.

**Reference**
None yet.

**Acceptance**
`MEM=3072`, `MEM=4096` and `MEM=8192` boot both images to the end marker, with all CPUs up; `MEM=2560` and below, and the NUMA machines, keep passing.
