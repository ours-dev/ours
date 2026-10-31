#include <ours/arch/mem_cfg.hpp>
#include <ours/arch/x86/bootstrap.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/init.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/object/resource_dispatcher.hpp>
#include <ours/platform/init.hpp>

#include <algorithm>
#include <array>
#include <type_traits>
#include <utility>

#include <heap/new.hpp>
#include <ustl/util/type_list.hpp>
#include <ustl/util/type_map.hpp>

namespace ours {
template <typename>
struct DisabledZoneFilter;

template <mm::ZoneType Type, usize Pfn, bool Enable>
struct DisabledZoneFilter<MaxZonePfn<Type, Pfn, Enable>>: std::bool_constant<!Enable> {};

/// Hands init_pmm() each zone type's bound at its type's index: the first pfn
/// past the type, capped at the last pfn there is. A type the platform does not
/// list, or switches off, keeps 0, which init_pmm() reads as no memory of that
/// type.
///
/// By type, not by place in the list. The list holds only the types this
/// platform has, and a switched-off one is filtered out of it, so by place a
/// left-out type would move every later bound onto the type before it (with
/// DMA off, DMA32's 4 GiB would become DMA's bound), and a list shorter than
/// the types was read past its end.
template <typename... LinearZoneMaxPfns>
INIT_CODE
auto platform_init_pmm_with_config(ustl::TypeList<LinearZoneMaxPfns...>) -> void {
    static_assert(sizeof...(LinearZoneMaxPfns) <= mm::NR_ZONES_PER_NODE, "To many zones per node");

    std::array<mm::Pfn, mm::NR_ZONES_PER_NODE> max_zone_pfns{};
    auto handle_item =
        [&]<mm::ZoneType Type, usize Pfn, bool Enable>(MaxZonePfn<Type, Pfn, Enable>) {
            static_assert(Enable, "There is a disabled zone type");
            max_zone_pfns[std::to_underlying(Type)] = std::min(Pfn, mm::max_pfn());
        };

    (handle_item(LinearZoneMaxPfns()), ...);

    mm::init_pmm(max_zone_pfns);
}

INIT_CODE
auto platform_init_pmm() -> void {
    using Config = ustl::TypeAlgos::Filter<ArchPmmZoneConfig, DisabledZoneFilter>;
    platform_init_pmm_with_config(Config());
}

INIT_CODE
static auto platform_init_resource() -> void {}
GKTL_INIT_HOOK(ResourceInit, platform_init_resource, gktl::InitLevel::Vmm);

} // namespace ours