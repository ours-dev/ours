#include <arch/x86/descriptor.hpp>
#include <logz4/log.hpp>
#include <ours/arch/x86/descriptor.hpp>
#include <ours/arch/x86/percpu.hpp>
#include <ours/mm/cpu_local.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vmm.hpp>
#include <ustl/mem/container_of.hpp>

#include <array>

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

    FORCE_INLINE
    auto get_tss_desc(CpuNum cpunum) -> arch::GdtDesc64 & {
        return tss[cpunum];
    }

    std::array<arch::GdtDesc32, X86_GDT_MAX_SELECTORS> descs;
    std::array<arch::GdtDesc64, MAX_CPU> tss;
};
static_assert(X86_GDT_MAX_SELECTORS * X86_SEGMENT_SELECTOR_SIZE == offsetof(Gdt, tss));

NO_MANGLE Gdt GDT;
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

INIT_CODE
auto x86_setup_gdt() -> void {
    auto result = mm::VmAspace::kernel_aspace()->root_vma()->map_phys(
        mm::virt_to_phys(VirtAddr(PGDT)),
        mm::VmPlacement::anywhere(), // Wherever the VMM has room
        PAGE_SIZE,
        mm::MmuFlags::Readable,                            // Read only
        mm::VmMapOption::Commit | mm::VmMapOption::Pinned, // Disable swapping out.
        "k:GDT");
    ASSERT(result, "Failed to create readonly GDT");

    PGDT = reinterpret_cast<decltype(PGDT)>((*result)->base());
    x86_load_gdt();
}

/// Code bottom is about TSS
INIT_CODE
auto x86_setup_tss_percpu(CpuNum cpunum) -> void {
    DEBUG_ASSERT(cpunum < MAX_CPU, "Invalid CPU number: {}", cpunum);
    auto tss = &mm::CpuLocal::access(X86_PCPU, cpunum).tss;
    tss->init();

    // Do not use PGDT, because it is readonly.
    GDT.get_tss_desc(cpunum)
        .set_base(usize(tss))
        .set_limit(sizeof(X86Tss) - 1)
        .set_type(arch::SegType::Tss64Available)
        .set_present(true)
        .set_dpl(arch::Dpl::Ring0)
        .set_granularity(arch::SegGran::Byte)
        .set_longmode(true);

    arch::ltr(GDT.tss_selector(cpunum));
}

} // namespace ours
