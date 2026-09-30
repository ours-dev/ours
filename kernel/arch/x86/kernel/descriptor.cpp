#include <ours/arch/x86/descriptor.hpp>
#include <ours/arch/x86/percpu.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vmm.hpp>

#include <array>

#include <arch/x86/descriptor.hpp>
#include <logz4/log.hpp>
#include <ustl/mem/container_of.hpp>

namespace ours {
struct alignas(PAGE_SIZE) PACKED Gdt {
    auto load() const -> void {
        arch::DescPtr desc{.limit = sizeof(*this) - 1, .base = reinterpret_cast<usize>(this)};

        arch::lgdt(desc);
    }

    FORCE_INLINE
    auto tss_selector(CpuNum cpunum) const -> u16 {
        return static_cast<u16>(sizeof(descs) + cpunum * sizeof(tss[0]));
    }

    /// tss_selector() run backwards: the CPU whose TSS descriptor `selector`
    /// names. A selector is the descriptor's byte offset into this table, and
    /// the TSS descriptors follow the fixed ones one per CPU, in CPU order.
    /// Kept beside tss_selector() so the layout is spelled out in one place.
    [[nodiscard]]
    FORCE_INLINE
    auto cpu_of_tss_selector(u16 selector) const -> CpuNum {
        return static_cast<CpuNum>((selector - sizeof(descs)) / sizeof(tss[0]));
    }

    FORCE_INLINE
    auto get_tss_desc(CpuNum cpunum) -> arch::GdtDesc64 & {
        return tss[cpunum];
    }

    std::array<arch::GdtDesc32, X86_GDT_MAX_SELECTORS> descs;
    std::array<arch::GdtDesc64, MAX_CPU> tss;
};
static_assert(X86_GDT_MAX_SELECTORS * X86_SEGMENT_SELECTOR_SIZE == offsetof(Gdt, tss));

extern "C" Gdt GDT;
static Gdt *PGDT = &GDT;

auto x86_get_gdt() -> void * {
    return PGDT;
}

auto x86_load_gdt() -> void {
    PGDT->load();
}

template <typename Descriptor>
auto dump_desc(Descriptor const &desc) -> void {
    log::info("{: <16} | 0x{:0<16X} | 0x{:<16X} | {}", to_string(desc.type()), desc.base(),
              desc.limit(), to_string(desc.dpl()));
}

auto x86_dump_gdt() -> void {
    auto &desc = PGDT->descs;
    log::info("{: <10} | {: <16} | {: <16} | DPL", "Type", "Base", "Limit");
    for (auto i = usize{0}; i < desc.size(); ++i) {
        dump_desc(desc[i]);
    }

    auto &tss = PGDT->tss;
    for (auto i = usize{0}; i < tss.size(); ++i) {
        dump_desc(tss[i]);
    }
}

auto x86_set_tss_sp(VirtAddr sp) -> void {
    auto tss = &mm::CpuLocal::access(X86_PCPU).tss;
    tss->sp0 = sp;
}

/// For the paranoid entry path (x86_handle_paranoid_exception()), which has
/// to know the CPU before it may touch per-CPU data, and so cannot ask
/// Cpu::id(): that reads the answer through %gs, the very register in doubt.
///
/// TR can be trusted where %gs cannot. x86_setup_tss_percpu() loads each CPU's
/// own TSS selector into it, and ltr is ring 0 only, so a user can neither
/// change it nor point it at another CPU. Nothing here touches per-CPU storage:
/// str, and arithmetic on the table's layout -- `GDT` is named only for its
/// member function, no descriptor is read.
///
/// A CPU that has not reached its ltr yet has TR = 0, which names no TSS and
/// comes out as a huge number; the assertion catches that. Such a CPU has no
/// IST stacks either, so an NMI there cannot be survived regardless.
///
/// Linux reads the CPU number back out of the limit of a per-CPU GDT entry
/// (LOAD_CPU_AND_NODE_SEG_LIMIT, used by GET_PERCPU_BASE) for the same reason.
auto x86_cpu_num_from_tr() -> CpuNum {
    auto const cpunum = GDT.cpu_of_tss_selector(arch::str());
    DEBUG_ASSERT(cpunum < MAX_CPU, "TR names no CPU's TSS: {}", cpunum);
    return cpunum;
}

INIT_CODE
auto x86_setup_gdt() -> void {
    auto result = mm::VmAspace::kernel_aspace()->root_vma()->map_phys(
        mm::virt_to_phys(VirtAddr(PGDT)),
        mm::VmPlacement::anywhere(), // Wherever the VMM has room
        PAGE_SIZE,
        mm::MmuFlags::Readable,                              // Read only
        mm::VmMapOption::Populate | mm::VmMapOption::Pinned, // Disable swapping out.
        "k:GDT");
    ASSERT(result, "Failed to create readonly GDT");

    PGDT = reinterpret_cast<decltype(PGDT)>((*result)->base());
    x86_load_gdt();
}

/// Code bottom is about TSS
INIT_CODE
auto x86_setup_tss_percpu(CpuNum cpunum) -> void {
    DEBUG_ASSERT(cpunum < MAX_CPU, "Invalid CPU number: {}", cpunum);
    auto &pcpu = mm::CpuLocal::access(X86_PCPU, cpunum);
    auto tss = &pcpu.tss;
    tss->init();

    // Publish this CPU's dedicated stacks in the IST slots their gates name
    // (x86_init_idt_early()). Until this was done every slot held 0, so an
    // NMI or #MC switched to %rsp = 0 and faulted pushing its own frame at
    // 0xfffffffffffffff8. That #PF then reported the *interrupted* context:
    // an NMI that caught a user thread killed its process for a
    // FatalPageFault on a kernel address, pinning the kernel's fault on the
    // program.
    //
    // The stacks are this CPU's own, taken from the same X86_PCPU as the TSS,
    // and for the same reason init_arch() calls this a second time on the
    // boot CPU: before CpuLocal::init() X86_PCPU resolves to the template,
    // and slots naming the template's stacks would be stale once CPU 0 moves
    // to its own chunk. DEBUG_BREAK_STACK is published although no gate uses
    // it; a slot nothing names costs nothing.
    //
    // Two limits come with IST, both as in Zircon. There is no guard page:
    // overflowing one of these runs into the neighbouring per-CPU data. And
    // every entry starts again from the top, so a handler must not re-enable
    // its own vector before it is done -- an NMI handler that takes a fault
    // and returns from it with iretq unmasks NMIs, and the next NMI lands on
    // the frame of the first. Linux's entry_64.S carries a nested-NMI
    // protocol for exactly that; here the rule is simply that the NMI and #MC
    // paths do not fault.
    //
    // Zircon counterpart: x86_tss_assign_ists(), called from
    // x86_initialize_percpu_tss().
    for (auto i = 0; i < MAX_NUM_STACKS; ++i) {
        auto const which = static_cast<IntrStackOrdering>(i);
        tss->set_ist(ist_slot(which), pcpu.intr_stack[which].top());
    }

    // Do not use PGDT, because it is readonly.
    GDT.get_tss_desc(cpunum)
        .set_base(usize(tss))
        .set_limit(sizeof(X86Tss) - 1)
        .set_type(arch::SegType::Tss64Available)
        .set_present(true)
        .set_dpl(arch::Dpl::Ring0)
        .set_granularity(arch::SegGran::Byte)
        .set_longmode(true);

    // ltr writes too: it marks the descriptor it loads busy, in the table
    // GDTR points at. Once x86_setup_gdt() has run, that is the read-only
    // alias, and the write faults -- a #PF in the middle of bringing the CPU
    // up. It went unnoticed while every page was mapped writable whatever it
    // asked for (arch::paging::X86Pte::make()). So the writable table is
    // loaded for the one instruction and the alias put back after it.
    //
    // Linux does the same in native_load_tr_desc(): if the current GDT is the
    // read-only fixmap copy, load the direct one, ltr, and load the fixmap one
    // again. Zircon avoids the question by mapping the GDT's real pages
    // read-write and only the unused rest of its 64 KiB read-only
    // (gdt_setup()).
    GDT.load();
    arch::ltr(GDT.tss_selector(cpunum));
    x86_load_gdt();
}

} // namespace ours
