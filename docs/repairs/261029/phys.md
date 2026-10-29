# phys outstanding repairs

Found on 2026-10-29 while building a QEMU machine with more proximity domains than the kernel can hold. Nothing to do with NUMA: the machine simply needed more memory than the four-node one, and no boot with more than 1 GiB of RAM survives physload.

## 1. The boot shim links only the first GiB of the identity map it builds, so physload triple-faults on a machine with more memory

**Severity**
High. Every machine with more than 1 GiB of RAM fails to boot before the kernel gets control, which is every real machine. It is invisible today only because every image this repo boots is given 512M or 1024M.

**Symptom**
`MEM=2048 python3 .agents/skills/ours-verify/scripts/boot_check.py --out out/<dir> --only dbg` reports `QEMU exited (rc 0) before the shell finished -- reset or triple fault? ... reached: phys`. The log's last line is physload's `crashlog: reserved [0x4000000, 0x4004000)`; the next line a healthy boot prints is `Allocate page table`. It is independent of NUMA -- `NUMA=0 MEM=2048`, `NUMA=1 MEM=2048` and `NUMA=33 MEM=1056` all fail, while `MEM=1024` and below pass, on both images.

**Cause**
`kernel/arch/x86/phys/start64.S` builds four pages of 2 MiB leaf entries and links one of them. The leaf loop runs `X86_MAX_PTES << 2` = 2048 iterations into a `PD` object of `.skip (PAGE_SIZE << 2)`, which is 4 GiB of mappings, and then:

```text
    // PDP[0] = PD | X86_KERNEL_PD_FLAGS
    lea rva(PD)(%ebx), %eax
    or $X86_KERNEL_PD_FLAGS, %eax
    mov %eax, rva(PDP)(%ebx)
```

One PDP entry spans 1 GiB, so entries 512..2047 of `PD` -- the mappings for 1 GiB..4 GiB -- are written and then orphaned: nothing points at the pages holding them. `PDP[1]`, `PDP[2]` and `PDP[3]` are never written. The comment in `kernel/arch/x86/phys/BUILD.gn` ("an identity-mapped 4 GiB of 2 MiB pages") describes the leaf entries, which exist, and not the mapping, which does not.

It faults where it does because physload allocates its own page tables top-down from the highest usable address and zeroes each one immediately, before the log line that would name it (`kernel/phys/aspace.cpp`, `Aspace::alloc_page_table()`):

```cpp
    if (phys) {
        std::memset(reinterpret_cast<void *>(phys), 0, size);
    }

    log::debug("Allocate page table (p:0x{:X}, s:0x{:X})", phys, size);
```

The first allocation tracks the top of RAM: `0x1FEBB000` on a 512M machine, `0x20EBB000` on a 528M one, `0x3FEBB000` on a 1024M one -- that last inside the final 2 MiB page the shim actually linked, with 1.3 MiB to spare. One more megabyte of RAM puts it past `0x40000000`, where there is no mapping, and the `memset` faults with no handler. That the fault boundary sits exactly between 1024M and 1056M is the evidence for this chain; the faulting address itself was not read under a debugger.

**Change**
Two separate things, and the first is the bug:

- Link the three remaining PD pages in `start64.S`, so the shim maps the 4 GiB it already builds entries for. A loop of the same shape as the leaf loop, writing `PDP[i] = PD + 0x1000 * i | X86_KERNEL_PD_FLAGS` for `i` in 0..3.
- Independently, bound physload's page-table allocations to the window it can touch rather than to `max_address()`. `Aspace` already has `m_allocation_upper_bound` for this; a machine with more RAM than the shim maps should then fail an allocation and say so, instead of writing to an unmapped address. Without this, a machine with more than 4 GiB fails the same silent way after the first part is done.

**Reference**
Linux's `arch/x86/kernel/head_64.S` maps its early region with `level3_kernel_pgt` holding **two** PDP entries, and `startup_64` fixes up the page tables for the actual load address; the early mapping is deliberately small and the kernel switches to a full map in `init_mem_mapping()` before touching high memory, rather than relying on the early map's size. The shape to copy is not "map more" but "never allocate outside what is mapped".

**Acceptance**
`MEM=2048` and `MEM=4096` boot both images to the end marker, and a machine with more RAM than the shim maps fails with a diagnosable message rather than a reset. `MEM=1024` and the NUMA machines keep passing.
