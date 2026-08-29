/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef BOOTMEM_TYPES_HPP
#define BOOTMEM_TYPES_HPP 1

#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

#include <limits>

namespace bootmem {
using ours::NnId;
using ours::PhysAddr;
using ours::Status;

inline constexpr auto ANY_NODE = std::numeric_limits<NnId>::max();

struct Range final {
    PhysAddr base{};
    usize size{};

    [[nodiscard]]
    static constexpr auto from(PhysAddr const base, usize const size)
        -> ustl::Result<Range, Status> {
        if (size == 0) {
            return ustl::err(Status::InvalidArgument);
        }
        if (base > std::numeric_limits<PhysAddr>::max() - size) {
            return ustl::err(Status::OutOfRange);
        }
        return ustl::ok(Range{.base = base, .size = size});
    }

    [[nodiscard]]
    constexpr auto end() const -> PhysAddr {
        return base + size;
    }
};

struct MemoryRegion final {
    Range range{};
    NnId node{};
};

/// One range plus whether it is currently free, for handing the whole
/// memory map to a consumer that should read it rather than share this
/// allocator's live state -- see PhysicalMemory::snapshot() and
/// ours::phys::MemoryHandoff.
struct MemRange final {
    Range range{};
    bool used{};
};

enum class AllocationDirection : u8 {
    BottomUp,
    TopDown,
};

struct AllocationRequest final {
    usize size{};
    usize alignment{};
    PhysAddr lower{};
    PhysAddr upper{};
    NnId node{ANY_NODE};
    AllocationDirection direction{AllocationDirection::BottomUp};
};

enum class MemoryView : u8 {
    Usable,
    Free,
};

struct MemoryQuery final {
    PhysAddr lower{};
    PhysAddr upper{};
    NnId node{ANY_NODE};
    MemoryView view{MemoryView::Usable};
};

struct PhysicalMemoryStorage final {
    ustl::views::Span<MemoryRegion> usable{};
    ustl::views::Span<Range> reservations{};
};

using MemoryVisitor = auto (*)(void *, MemoryRegion) -> bool;

} // namespace bootmem

namespace bootmm = bootmem;

#endif // #ifndef BOOTMEM_TYPES_HPP
