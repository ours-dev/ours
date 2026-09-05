#include <ktl/new.hpp>
#include <ours/arch/mem_cfg.hpp>
#include <ours/arch/x86/bootstrap.hpp>
#include <ours/mm/early_mem.hpp>
#include <ours/mm/init.hpp>
#include <ours/mm/pmm.hpp>
#include <ours/object/resource_dispatcher.hpp>
#include <ours/platform/init.hpp>
#include <ustl/util/type_list.hpp>
#include <ustl/util/type_map.hpp>

#include <algorithm>
#include <array>
#include <type_traits>

namespace ours {
template <typename>
struct DisabledZoneFilter;

template <mm::ZoneType Type, usize Pfn, bool Enable>
struct DisabledZoneFilter<MaxZonePfn<Type, Pfn, Enable>>: std::bool_constant<!Enable> {};

/// This template design ensures that changes to the number of zone types do not affect existing implementations.
template <typename... LinearZoneMaxPfns>
INIT_CODE
auto platform_init_pmm_with_config(ustl::TypeList<LinearZoneMaxPfns...>) -> void {
    static_assert(sizeof...(LinearZoneMaxPfns) <= mm::NR_ZONES_PER_NODE, "To many zones per node");

    std::array<mm::Pfn, sizeof...(LinearZoneMaxPfns)> linear_zone_pfns;
    auto index = 0;
    auto handle_item =
        [&]<mm::ZoneType Type, usize Pfn, bool Enable>(MaxZonePfn<Type, Pfn, Enable>) {
            static_assert(Enable, "There is a disabled zone type");
            linear_zone_pfns[index++] = std::min(Pfn, mm::max_pfn());
        };

    (handle_item(LinearZoneMaxPfns()), ...);

    mm::init_pmm(linear_zone_pfns);
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