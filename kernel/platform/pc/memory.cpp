#include <ktl/new.hpp>
#include <ours/arch/mem-cfg.hpp>
#include <ours/arch/x86/bootstrap.hpp>
#include <ours/mem/early-mem.hpp>
#include <ours/mem/init.hpp>
#include <ours/mem/pmm.hpp>
#include <ours/object/resource-dispatcher.hpp>
#include <ours/platform/init.hpp>
#include <ustl/algorithms/minmax.hpp>
#include <ustl/array.hpp>
#include <ustl/util/type-list.hpp>

namespace ours {
template <typename>
struct DisabledZoneFilter;

template <mem::ZoneType Type, usize Pfn, bool Enable>
struct DisabledZoneFilter<MaxZonePfn<Type, Pfn, Enable>>: ustl::traits::BoolConstant<!Enable> {};

/// This template design ensures that changes to the number of zone types do not affect existing implementations.
template <typename... LinearZoneMaxPfns>
INIT_CODE auto platform_init_pmm_with_config(ustl::TypeList<LinearZoneMaxPfns...>) -> void {
    static_assert(sizeof...(LinearZoneMaxPfns) <= mem::NR_ZONES_PER_NODE, "To many zones per node");

    ustl::Array<mem::Pfn, sizeof...(LinearZoneMaxPfns)> linear_zone_pfns;
    auto index = 0;
    auto handle_item =
        [&]<mem::ZoneType Type, usize Pfn, bool Enable>(MaxZonePfn<Type, Pfn, Enable>) {
            static_assert(Enable, "There is a disabled zone type");
            linear_zone_pfns[index++] = ustl::algorithms::min(Pfn, mem::max_pfn());
        };

    (handle_item(LinearZoneMaxPfns()), ...);

    mem::init_pmm(linear_zone_pfns);
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