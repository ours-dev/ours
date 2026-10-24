# arch changes

## Title: the kernel can tell bare metal, a hypervisor and an emulator apart (2026-10-24)

**Files**
- `kernel/lib/arch/include/arch/machine.hpp` — new: `MachineKind { BareMetal, Virtualized, Emulated }`, `machine_kind()`, `to_string()`, and the include of the architecture's completion
- `kernel/arch/x86/lib/arch/include/arch/x86/machine.hpp` — new: the x86 answer, from CPUID leaf 1 and leaf 0x4000'0000
- `kernel/lib/arch/BUILD.gn`, `kernel/arch/x86/BUILD.gn` — the two headers listed
- `kernel/commands/cpu.cpp` — `cpu status` prints the machine kind first

**Symptom**
None, a new capability. It is the first half of large-pages step 6: the large-page benchmark has metrics (TLB-bound random access) that mean nothing under QEMU's translator, and it needs a way to know it is there so it can skip them with a message instead of printing a number that looks real.

**Cause**
The kernel had no notion of what was running it. That did not matter while nothing measured caches or the TLB. Under QEMU TCG (the only way to run an x86 guest on this arm64 host), every guest instruction is translated by software, and QEMU's own soft TLB stores 4 KiB entries whatever the guest's page size, so a 2 MiB leaf saves no TLB entries at all; a timing taken there is not a smaller version of the real effect, it is a different quantity.

**Change**
Two CPUID reads, in the arch header pattern `arch/random.hpp` already uses (a generic declaration, then an inline x86 completion):

```cpp
// CPUID.1:ECX bit 31: a processor reports it clear, anything presenting a
// virtual one sets it.
if ((ecx & (u32(1) << 31)) == 0) {
    return MachineKind::BareMetal;
}
// Leaf 0x4000'0000: the hypervisor's vendor in EBX, ECX, EDX.
if (__builtin_memcmp(signature, "TCGTCGTCGTCG", sizeof(signature)) == 0) {
    return MachineKind::Emulated;
}
return MachineKind::Virtualized;
```

Three kinds, not two, because KVM and TCG differ in exactly the way a benchmark cares about: under KVM the guest runs on the processor and its TLB is the processor's (with nested paging on top), so TLB-bound numbers there are meaningful; under TCG they are not.

`cpu status` gains one line, `machine: emulated`, so the answer can be seen without writing a benchmark, and so the header has a user (and gets compiled) in the commit that adds it.

Rejected: caching the answer at boot in a global. Nothing asks often, CPUID is cheap next to any benchmark, and a global would need an init step and an ordering argument for nothing.

**Reference**
Zircon `get_hypervisor()` in `kernel/arch/x86/feature.cc` does the same two reads (`X86_FEATURE_HYPERVISOR`, then `X86_CPUID_HYP_VENDOR` compared with "KVMKVMKVM\0\0\0"), but answers NONE, KVM or UNKNOWN: it wants to know about KVM's paravirtual clock and EOI, not about emulation. Linux `detect_hypervisor_vendor()` in `arch/x86/kernel/cpu/hypervisor.c` walks a table of hypervisor drivers keyed by the same signature, gated on `X86_FEATURE_HYPERVISOR`. Neither has an "emulated" answer; this project needs one because its only x86 test machine is an emulator.

**Verification**
- Build, clang-format, clang-tidy: clean.
- `python3 scripts/kshell_run.py --out out/bringup.x86 "cpu status"`: prints `machine: emulated` (QEMU TCG, leaf 1 ECX = 0x80982201, leaf 0x4000'0000 signature "TCGTCGTCGTCG", read earlier with a throwaway probe).
- `boot_check.py`: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), host tests 146 passed.
- Virtualized and BareMetal: not measured, no such machine at hand.

**Deliberately not done**
- No ARM or RISC-V completion: no such kernel builds today, and `arch/random.hpp` leaves the same gap.
- No detection of an emulator that leaves the hypervisor bit clear (Bochs by default): CPUID cannot see one, and it is reported as bare metal. The comment says so.
- No other hypervisor names (Hyper-V, VMware, Xen): all of them run guest code on the processor, so they are all `Virtualized` as far as a measurement goes.

**How it was found**
None.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why is KVM a different answer from TCG for a TLB measurement, though both are "QEMU"?
- What does `machine_kind()` say about an emulator that does not set CPUID.1:ECX bit 31, and why can it not do better?
- Why does a 2 MiB leaf save no TLB entries under TCG?
