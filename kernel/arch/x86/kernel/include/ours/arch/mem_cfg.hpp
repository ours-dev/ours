/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_MEM_CFG_HPP
#define OURS_ARCH_MEM_CFG_HPP 1

#include <ours/mm/init.hpp>

#include <type_traits>

#include <ustl/util/type_list.hpp>

/// The zone bounds are the address limits of devices: Dma is what an ISA
/// device reaches (24-bit addresses), Dma32 what a device with 32-bit
/// addresses reaches. A lower Dma32 bound only makes the zone a fraction of
/// the memory such a device can use.
#define ARCH_MAX_DMA_PFN (16_mb >> PAGE_SHIFT)
#define ARCH_MAX_DMA32_PFN (4_gb >> PAGE_SHIFT)
#define ARCH_MAX_PFN_LIMIT (64_gb >> PAGE_SHIFT)

namespace ours {
template <mm::ZoneType>
struct Selector;

template <mm::ZoneType Type, usize Pfn, bool Enable = true>
using MaxZonePfn =
    ustl::TypeList<Selector<Type>, std::integral_constant<usize, Pfn>, std::bool_constant<Enable>>;
using ArchPmmZoneConfig =
    ustl::TypeList<MaxZonePfn<mm::ZoneType::Dma, ARCH_MAX_DMA_PFN, OURS_CONFIG_ZONE_DMA>,
                   MaxZonePfn<mm::ZoneType::Dma32, ARCH_MAX_DMA32_PFN, OURS_CONFIG_ZONE_DMA32>,
                   MaxZonePfn<mm::ZoneType::Normal, ARCH_MAX_PFN_LIMIT>>;
} // namespace ours

#endif // #ifndef OURS_ARCH_MEM_CFG_HPP