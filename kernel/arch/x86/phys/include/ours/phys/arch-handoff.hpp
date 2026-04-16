/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PHYS_ARCH_HANDOFF_HPP
#define OURS_PHYS_ARCH_HANDOFF_HPP 1

#include <ours/types.hpp>
#include <ustl/views/span.hpp>

namespace ours::phys {
    struct X86CpuTopologyNode {
        enum Type {
            Cluster,
            // Identical with Socket
            Package,
            Die,
            DieGrp,
            Tile,
            Module,
            Core,
            LogicalProcessor,
        };

        u32 flags;
        u32 apic_id;
        ustl::views::Span<X86CpuTopologyNode> s;
    };

    struct CpuTopologyNode {};

    struct ArchHandOff {};

} // namespace ours::phys

#endif // #ifndef OURS_PHYS_ARCH_HANDOFF_HPP