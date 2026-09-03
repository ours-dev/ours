/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MM_CFG_HPP
#define OURS_MM_CFG_HPP 1

#include <ours/macro_abi.hpp>

/// The build spells these switches MM_NUMA / MM_NUMA_EMULATION (GN booleans,
/// so they reach source as the keywords `true`/`false` -- see
/// //kernel/products:memory), while everything below is written in terms of the
/// OURS_CONFIG_* names. Derive one from the other here rather than leaving two
/// spellings of one switch to drift: they had drifted, and a build with
/// `mm_numa = true` still got NUMA_MAX_NODE == 1 and a PmFrame with its node
/// field compiled out -- NUMA was selectable but never actually on.
#ifndef OURS_CONFIG_NUMA
#if defined(MM_NUMA) && MM_NUMA
#define OURS_CONFIG_NUMA 1
#else
#define OURS_CONFIG_NUMA 0
#endif
#endif

#ifndef OURS_CONFIG_NUMA_EMULATION
#if defined(MM_NUMA_EMULATION) && MM_NUMA_EMULATION
#define OURS_CONFIG_NUMA_EMULATION 1
#else
#define OURS_CONFIG_NUMA_EMULATION 0
#endif
#endif

#ifndef OURS_CONFIG_MAX_PHYSADDR_BITS
#define OURS_CONFIG_MAX_PHYSADDR_BITS 52
#endif

#ifndef OURS_CONFIG_ZONE_DMA
#define OURS_CONFIG_ZONE_DMA 1
#endif

#ifndef OURS_CONFIG_ZONE_DMA32
#define OURS_CONFIG_ZONE_DMA32 1
#endif

#ifndef OURS_CONFIG_ZONE_MOVABLE
#define OURS_CONFIG_ZONE_MOVABLE 0
#endif

#ifndef OURS_CONFIG_ZONE_DYNAMIC
#define OURS_CONFIG_ZONE_DYNAMIC 0
#endif

#ifndef OURS_CONFIG_NUMA_MAX_NODES
#if OURS_CONFIG_NUMA == 1 || OURS_CONFIG_NUMA_EMULATION == 1
#define NUMA_MAX_NODE 32
#else
#define NUMA_MAX_NODE 1
#endif
#endif
#define NUMA_MAX_NODES_BITS BIT_WIDTH(NUMA_MAX_NODE)
#define NUMA_NO_NODE (NUMA_MAX_NODE + 1)

#define MAX_PHYSADDR_BITS OURS_CONFIG_MAX_PHYSADDR_BITS

/// [Node.MaxZoneNum]
#define ZONES_SHIFT BIT_WIDTH(NR_ZONES_PER_NODE)
#define ZONE_MASK (BIT(ZONES_SHIFT) - 1)
#define NR_ZONES_PER_NODE_BITS BIT_WIDTH(NR_ZONES_PER_NODE)
#define MAX_ZONES (NR_ZONES_PER_NODE * NUMA_MAX_NODE)
#define MAX_ZONES_BITS BIT_WIDTH(MAX_ZONES)

/// [Frame.Shift]
#ifndef OURS_CONFIG_PAGE_SHIFT
#define PAGE_SHIFT 12 // 4K page
#else
#define PAGE_SHIFT OURS_CONFIG_PAGE_SHIFT
#endif
#define PAGE_SIZE BIT(PAGE_SHIFT)

#ifndef OURS_CONFIG_MAX_PAGE_SHIFT
#define MAX_PAGE_SHIFT PAGE_SHIFT
#else
#define MAX_PAGE_SHIFT OURS_CONFIG_MAX_PAGE_SHIFT
#endif
#define MAX_PAGE_SIZE BIT(MAX_PAGE_SHIFT)

/// [Secion.Bits]
#ifndef OURS_CONFIG_SECTION_SIZE_BITS
#define SECTION_SIZE_BITS 27
#else
#define SECTION_SIZE_BITS OURS_CONFIG_SECTION_SIZE_BITS
#endif
#define SECTION_SHIFT SECTION_SIZE_BITS
#define SECTION_MASK (BIT(SECTION_SHIFT) - 1)
#define PFN_SECTION_SHIFT (SECTION_SHIFT - PAGE_SHIFT)
#define PFN_SECTION_MASK (BIT(SECTION_SHIFT - PAGE_SHIFT) - 1)

/// [Frame.NrOrders]
#ifndef OURS_CONFIG_NR_FRAME_ORDERS
#define NR_FRAME_ORDERS 11
#else
#define NR_FRAME_ORDERS OURS_CONFIG_NR_FRAME_ORDERS
#endif
#define MAX_FRAME_ORDER (NR_FRAME_ORDERS - 1)

#ifndef OURS_CONFIG_KASLR
#define OURS_CONFIG_KASLR 0
#endif

#define FRAME_DESC_SIZE OURS_TARGET_BITS
#define FRAME_DESC_SHIFT (OURS_TARGET_BITS == 64 ? 6 : 5)

#endif // #ifndef OURS_MM_CFG_HPP
