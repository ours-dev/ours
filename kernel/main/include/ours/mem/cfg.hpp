/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_CFG_HPP
#define OURS_MEM_CFG_HPP 1

#include <ours/macro_abi.hpp>
#include <arch/macro/irqframe.hpp>

#ifndef OURS_CONFIG_NUMA_MAX_NODES
#   if OURS_CONFIG_NUMA == 1 || OURS_CONFIG_NUMA_EMULATION == 1
#       define NUMA_MAX_NODE 32
#   else
#       define NUMA_MAX_NODE 1
#   endif
#endif
#define NUMA_MAX_NODES_BITS  BIT_WIDTH(NUMA_MAX_NODE)
#define NUMA_NO_NODE (NUMA_MAX_NODE + 1)

/// [Node.MaxZoneNum]
#define ZONES_SHIFT BIT_WIDTH(NR_ZONES_PER_NODE)
#define ZONE_MASK (BIT(ZONES_SHIFT) - 1) 
#define NR_ZONES_PER_NODE_BITS BIT_WIDTH(NR_ZONES_PER_NODE)
#define MAX_ZONES (NR_ZONES_PER_NODE * NUMA_MAX_NODE)
#define MAX_ZONES_BITS BIT_WIDTH(MAX_ZONES)

/// [Frame.Shift]
#ifndef OURS_CONFIG_PAGE_SHIFT
#   define PAGE_SHIFT   12 // 4K page
#else
#   define PAGE_SHIFT OURS_CONFIG_PAGE_SHIFT
#endif
#define PAGE_SIZE BIT(PAGE_SHIFT)

#ifndef OURS_CONFIG_MAX_PAGE_SHIFT
#   define MAX_PAGE_SHIFT  PAGE_SHIFT
#else
#   define MAX_PAGE_SHIFT  OURS_CONFIG_MAX_PAGE_SHIFT
#endif
#define MAX_PAGE_SIZE BIT(MAX_PAGE_SHIFT)

/// [Secion.Bits]
#ifndef OURS_CONFIG_SECTION_SIZE_BITS	
#   define SECTION_SIZE_BITS    27
#else
#   define SECTION_SIZE_BITS OURS_CONFIG_SECTION_SIZE_BITS
#endif
#define SECTION_SHIFT SECTION_SIZE_BITS
#define SECTION_MASK  (BIT(SECTION_SHIFT) - 1)
#define PFN_SECTION_SHIFT   (SECTION_SHIFT - PAGE_SHIFT)
#define PFN_SECTION_MASK    (BIT(SECTION_SHIFT - PAGE_SHIFT) - 1)

/// [Frame.NrOrders]
#ifndef OURS_CONFIG_NR_FRAME_ORDERS
#   define NR_FRAME_ORDERS  11
#else
#   define NR_FRAME_ORDERS  OURS_CONFIG_NR_FRAME_ORDERS
#endif
#define MAX_FRAME_ORDER (NR_FRAME_ORDERS - 1) 

#ifndef OURS_CONFIG_KASLR
#   define OURS_CONFIG_KASLR  0 
#endif

#define FRAME_DESC_SIZE     OURS_TARGET_BITS
#define FRAME_DESC_SHIFT   (OURS_TARGET_BITS == 64 ? 6 : 5)

#define OM_API  [[nodiscard("OM-API: Ignoring the return value will lead to memory leaks.")]]

#endif // #ifndef OURS_MEM_CFG_HPP