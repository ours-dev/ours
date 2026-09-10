/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_MEM_CFG_HPP
#define OURS_ARCH_MEM_CFG_HPP 1

#include <ours/mm/init.hpp>

#include <type_traits>

#include <ustl/util/type_list.hpp>

#define ARCH_MAX_DMA_PFN (MB(16) >> PAGE_SHIFT)
#define ARCH_MAX_DMA32_PFN (MB(512) >> PAGE_SHIFT)
#define ARHC_MAX_PFN_LIMIT (GB(64) >> PAGE_SHIFT)

namespace ours {
template <mm::ZoneType>
struct Selector;

template <mm::ZoneType Type, usize Pfn, bool Enable = true>
using MaxZonePfn =
    ustl::TypeList<Selector<Type>, std::integral_constant<usize, Pfn>, std::bool_constant<Enable>>;
using ArchPmmZoneConfig =
    ustl::TypeList<MaxZonePfn<mm::ZoneType::Dma, ARCH_MAX_DMA_PFN, OURS_CONFIG_ZONE_DMA>,
                   MaxZonePfn<mm::ZoneType::Dma32, ARCH_MAX_DMA32_PFN, OURS_CONFIG_ZONE_DMA32>,
                   MaxZonePfn<mm::ZoneType::Normal, ARHC_MAX_PFN_LIMIT>>;
} // namespace ours

#endif // #ifndef OURS_ARCH_MEM_CFG_HPP