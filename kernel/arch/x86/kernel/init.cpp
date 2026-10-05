#include <ours/arch/apic.hpp>
#include <ours/arch/aspace_layout.hpp>
#include <ours/arch/x86/descriptor.hpp>
#include <ours/arch/x86/entry.hpp>
#include <ours/arch/x86/feature.hpp>
#include <ours/arch/x86/idt.hpp>
#include <ours/arch/x86/init.hpp>
#include <ours/cpu.hpp>
#include <ours/init.hpp>
#include <ours/mm/cfg.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/start.hpp>

#include <arch/halt.hpp>
#include <arch/x86/msr.hpp>
#include <arch/x86/system.hpp>

namespace ours {
INIT_CODE
static auto x86_enable_syscall() -> void {
    using namespace arch;

    // Enable system call. It is required by Intel.
    X86Efer::read().set<X86Efer::Sce>(true).write();

    /// On syscall,
    ///     CS = IA32_STAR_MSR[47:32]
    ///     SS = IA32_STAR_MSR[47:32] + 8
    /// On sysexit,
    ///     CS = IA32_STAR_MSR[63:48] + 16
    ///     SS = IA32_STAR_MSR[63:48] + 8
    constexpr auto const CS_AND_SS((usize(X86_GDT_KERNEL_CODE64) << 32) |
                                   (usize(X86_GDT_USER_CODE32) << 48));
    MsrIo::write(MsrRegAddr::IA32Star, CS_AND_SS);

    // Set system call entry point to x86_syscall
    MsrIo::write(MsrRegAddr::IA32Lstar, reinterpret_cast<usize>(&x86_syscall));

    // SYSCALL also saves RFLAGS into R11 and then masks RFLAGS using the
    // IA32_FMASK MSR (MSR address C0000084H); specifically, the processor
    // clears in RFLAGS every bit corresponding to a bit that is set in the
    // IA32_FMASK MSR.
    //
    // 1. Disable nested interrupt.
    // 2. Escalates IO privilege level(U->K).
    // 3. Clear those unused status bits.
    // 4. Clear the three flags whose user value would be a kernel bug. Every
    //    one of them is a flag user code may set with a plain popf, and
    //    SYSCALL carries RFLAGS into ring 0 untouched except for this mask:
    //    - TF: a single-step trap would fire after the first kernel
    //      instruction, before the stub has left the user stack. #DB does not
    //      switch stacks from CPL 0, so the CPU would push its frame through a
    //      stack pointer userspace chose.
    //    - DF: the SysV ABI promises DF=0 at every function entry, and the
    //      compiler's `rep movs` for memcpy trusts it. A user DF=1 runs every
    //      kernel copy backwards from its start.
    //    - AC: on a CPU with SMAP, AC=1 is exactly the permission for ring 0
    //      to touch user pages. Carried in from user space it switches SMAP
    //      off for the whole syscall.
    constexpr usize RFLAGS_STATUS_MASK = (usize{1} << 0) | (usize{1} << 2) | (usize{1} << 4) |
                                         (usize{1} << 6) | (usize{1} << 7) | (usize{1} << 11);
    constexpr usize RFLAGS_TRAP_FLAG = usize{1} << 8;
    constexpr usize RFLAGS_INTERRUPT_FLAG = usize{1} << 9;
    constexpr usize RFLAGS_DIRECTION_FLAG = usize{1} << 10;
    constexpr usize RFLAGS_IOPL_MASK = usize{3} << 12;
    constexpr usize RFLAGS_NESTED_TASK = usize{1} << 14;
    constexpr usize RFLAGS_ALIGNMENT_CHECK = usize{1} << 18;
    constexpr usize DISABLED_FLAG_BIT_MASK =
        RFLAGS_STATUS_MASK | RFLAGS_TRAP_FLAG | RFLAGS_INTERRUPT_FLAG | RFLAGS_DIRECTION_FLAG |
        RFLAGS_IOPL_MASK | RFLAGS_NESTED_TASK | RFLAGS_ALIGNMENT_CHECK;
    MsrIo::write(MsrRegAddr::IA32Fmask, DISABLED_FLAG_BIT_MASK);
}

INIT_CODE
auto x86_init_percpu(CpuNum cpunum) -> void {
    using arch::Cr4;

    x86_init_feature_percpu(cpunum);

    // GDT
    x86_load_gdt();

    // TSS
    x86_setup_tss_percpu(cpunum);

    // IDT
    x86_load_idt();

    Cr4::read()
        .set<Cr4::FsGsBase>(x86_has_feature(CpuFeatureType::FsGsBase)) // Enable FS/GS_BASE
        .set<Cr4::Umip>(x86_has_feature(CpuFeatureType::Umip)) // User mode instruction preventation
        .write();

    x86_enable_syscall();

    x86_init_mmu_percpu();
}

extern "C" INIT_CODE
auto init_arch_early() -> void {
    x86_init_idt_early();

    x86_init_percpu(0);

    x86_init_mmu_early();

    Cpu::set_state(Cpu::State::Online);
}

extern "C" INIT_CODE
auto init_arch() -> void {
    x86_setup_idt();

    x86_setup_gdt();

    // The boot CPU's TSS descriptor has to be rebuilt now that CpuLocal has
    // moved the per-CPU area.
    //
    // x86_init_percpu(0) already ran x86_setup_tss_percpu() from
    // init_arch_early(), long before mm::CpuLocal::init(): the descriptor it
    // wrote -- and the `ltr` that loaded it -- named the *template* copy of
    // X86_PCPU, in the physmap. CpuLocal then gave CPU 0 a chunk of its own,
    // so x86_set_tss_sp(), which resolves X86_PCPU through the running CPU,
    // has been writing sp0 into an object the CPU never consults. The two
    // addresses were visibly different: the GDT said 0xffffff801fd400e0 while
    // the live one was 0xffffff8800000e0.
    //
    // It cost nothing while the kernel stayed in ring 0, because sp0 is only
    // read on a privilege change. The first `iretq` into ring 3 made it
    // fatal: the very next interrupt loaded RSP0 = 0 from the stale TSS,
    // pushed the interrupt frame at address 0, and the #PF that raised needed
    // RSP0 again -- a double fault, then a triple, before any handler ran.
    //
    // Only the boot CPU is affected. A secondary runs x86_init_percpu() from
    // its own bring-up path, which happens well after CpuLocal::init().
    x86_setup_tss_percpu(Cpu::id());

    x86_setup_mmu();
}

} // namespace ours
