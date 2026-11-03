/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <bootmem/physical_memory.hpp>

#include <array>
#include <limits>

#include <cstdlib>

namespace {
using bootmem::AllocationDirection;
using bootmem::AllocationRequest;
using bootmem::MemoryQuery;
using bootmem::MemoryRegion;
using bootmem::MemoryType;
using bootmem::MemoryView;
using bootmem::PhysicalMemory;
using bootmem::PhysicalMemoryStorage;
using bootmem::Range;
using ours::NodeId;
using ours::PhysAddr;
using ours::Status;

auto require(bool const condition) -> void {
    if (!condition) {
        std::abort();
    }
}

class Fixture final {
  public:
    Fixture() : memory_({.usable = usable_, .reservations = reservations_}) {}

    [[nodiscard]]
    auto memory() -> PhysicalMemory & {
        return memory_;
    }

  private:
    std::array<MemoryRegion, 8> usable_{};
    std::array<bootmem::Reservation, 8> reservations_{};
    PhysicalMemory memory_;
};

auto range(PhysAddr const base, usize const size) -> Range {
    auto const value = Range::from(base, size);
    require(value.is_ok());
    return value.unwrap();
}

auto request(AllocationDirection const direction) -> AllocationRequest {
    return {
        .size = 0x1000,
        .alignment = 0x1000,
        .lower = PhysAddr{0x1000},
        .upper = PhysAddr{0x9000},
        .node = bootmem::ANY_NODE,
        .direction = direction,
    };
}

auto free_query() -> MemoryQuery {
    return {
        .lower = PhysAddr{0x1000},
        .upper = PhysAddr{0x9000},
        .node = bootmem::ANY_NODE,
        .view = MemoryView::Free,
    };
}

struct CollectedRegions final {
    std::array<MemoryRegion, 8> regions{};
    usize count{};
};

auto collect(PhysicalMemory const &memory, MemoryQuery const query) -> CollectedRegions {
    auto collected = CollectedRegions{};
    auto const status = memory.visit(query, [&collected](MemoryRegion const region) {
        collected.regions[collected.count++] = region;
        return true;
    });
    require(status == Status::Ok);
    return collected;
}

auto allocation_is_exact_and_bottom_up() -> void {
    auto fixture = Fixture{};
    require(fixture.memory().add_usable(range(PhysAddr{0x1000}, 0x8000), NodeId{0}) == Status::Ok);
    require(fixture.memory().reserve(range(PhysAddr{0x4000}, 0x2000), MemoryType::KernelData) ==
            Status::Ok);

    auto const allocated = fixture.memory().allocate(request(AllocationDirection::BottomUp));
    require(allocated.is_ok());
    require(allocated.unwrap().base == PhysAddr{0x1000});
    require(allocated.unwrap().size == 0x1000);

    auto const next = fixture.memory().allocate(request(AllocationDirection::BottomUp));
    require(next.is_ok());
    require(next.unwrap().base == PhysAddr{0x2000});
}

auto allocation_is_exact_and_top_down() -> void {
    auto fixture = Fixture{};
    require(fixture.memory().add_usable(range(PhysAddr{0x1000}, 0x8000), NodeId{0}) == Status::Ok);
    require(fixture.memory().reserve(range(PhysAddr{0x4000}, 0x2000), MemoryType::KernelData) ==
            Status::Ok);

    auto const allocated = fixture.memory().allocate(request(AllocationDirection::TopDown));
    require(allocated.is_ok());
    require(allocated.unwrap().base == PhysAddr{0x8000});
    require(allocated.unwrap().size == 0x1000);
}

auto free_query_reports_the_reservation_tail() -> void {
    auto fixture = Fixture{};
    require(fixture.memory().add_usable(range(PhysAddr{0x1000}, 0x8000), NodeId{0}) == Status::Ok);
    require(fixture.memory().reserve(range(PhysAddr{0x4000}, 0x2000), MemoryType::KernelData) ==
            Status::Ok);

    auto const collected = collect(fixture.memory(), free_query());
    require(collected.count == 2);
    require(collected.regions[0].range.base == PhysAddr{0x1000});
    require(collected.regions[0].range.size == 0x3000);
    require(collected.regions[1].range.base == PhysAddr{0x6000});
    require(collected.regions[1].range.size == 0x3000);
}

auto node_assignment_splits_usable_memory() -> void {
    auto fixture = Fixture{};
    require(fixture.memory().add_usable(range(PhysAddr{0x1000}, 0x8000), NodeId{0}) == Status::Ok);
    require(fixture.memory().assign_node(range(PhysAddr{0x3000}, 0x2000), NodeId{1}) == Status::Ok);

    auto const collected = collect(fixture.memory(), {
                                                         .lower = PhysAddr{0x1000},
                                                         .upper = PhysAddr{0x9000},
                                                         .node = NodeId{1},
                                                         .view = MemoryView::Usable,
                                                     });
    require(collected.count == 1);
    require(collected.regions[0].range.base == PhysAddr{0x3000});
    require(collected.regions[0].range.size == 0x2000);
}

auto overlapping_usable_ranges_are_coalesced_per_node() -> void {
    auto fixture = Fixture{};
    require(fixture.memory().add_usable(range(PhysAddr{0x1000}, 0x4000), NodeId{0}) == Status::Ok);
    require(fixture.memory().add_usable(range(PhysAddr{0x3000}, 0x4000), NodeId{0}) == Status::Ok);

    auto const collected = collect(fixture.memory(), {
                                                         .lower = PhysAddr{0x1000},
                                                         .upper = PhysAddr{0x8000},
                                                         .node = NodeId{0},
                                                         .view = MemoryView::Usable,
                                                     });
    require(collected.count == 1);
    require(collected.regions[0].range.base == PhysAddr{0x1000});
    require(collected.regions[0].range.size == 0x6000);
}

auto free_query_can_stop_early() -> void {
    auto fixture = Fixture{};
    require(fixture.memory().add_usable(range(PhysAddr{0x1000}, 0x2000), NodeId{0}) == Status::Ok);
    require(fixture.memory().add_usable(range(PhysAddr{0x4000}, 0x2000), NodeId{0}) == Status::Ok);

    auto calls = usize{0};
    auto const status = fixture.memory().visit(free_query(), [&calls](MemoryRegion const) {
        ++calls;
        return false;
    });
    require(status == Status::Ok);
    require(calls == 1);
}

auto reservation_capacity_failure_is_transactional() -> void {
    auto usable = std::array<MemoryRegion, 2>{};
    auto reservations = std::array<bootmem::Reservation, 1>{};
    auto memory = PhysicalMemory({.usable = usable, .reservations = reservations});
    require(memory.add_usable(range(PhysAddr{0x1000}, 0x8000), NodeId{0}) == Status::Ok);
    require(memory.reserve(range(PhysAddr{0x4000}, 0x1000), MemoryType::KernelData) == Status::Ok);

    auto const allocated = memory.allocate(request(AllocationDirection::BottomUp));
    require(allocated.is_err());

    auto const collected = collect(memory, free_query());
    require(collected.count == 2);
    require(collected.regions[0].range.base == PhysAddr{0x1000});
    require(collected.regions[0].range.size == 0x3000);
    require(collected.regions[1].range.base == PhysAddr{0x5000});
    require(collected.regions[1].range.size == 0x4000);
}

auto range_rejects_empty_and_overflowing_values() -> void {
    require(Range::from(PhysAddr{0x1000}, 0).is_err());
    require(Range::from(PhysAddr{~0_usize} - 7, 8).is_err());
}

auto release_splits_only_the_requested_reservation() -> void {
    auto fixture = Fixture{};
    require(fixture.memory().add_usable(range(PhysAddr{0x1000}, 0x8000), NodeId{0}) == Status::Ok);
    require(fixture.memory().reserve(range(PhysAddr{0x2000}, 0x4000), MemoryType::KernelData) ==
            Status::Ok);
    require(fixture.memory().release(range(PhysAddr{0x3000}, 0x1000)) == Status::Ok);

    auto const collected = collect(fixture.memory(), free_query());
    require(collected.count == 3);
    require(collected.regions[0].range.base == PhysAddr{0x1000});
    require(collected.regions[0].range.size == 0x1000);
    require(collected.regions[1].range.base == PhysAddr{0x3000});
    require(collected.regions[1].range.size == 0x1000);
    require(collected.regions[2].range.base == PhysAddr{0x6000});
    require(collected.regions[2].range.size == 0x3000);
}

auto release_capacity_failure_is_transactional() -> void {
    auto usable = std::array<MemoryRegion, 2>{};
    auto reservations = std::array<bootmem::Reservation, 1>{};
    auto memory = PhysicalMemory({.usable = usable, .reservations = reservations});
    require(memory.add_usable(range(PhysAddr{0x1000}, 0x8000), NodeId{0}) == Status::Ok);
    require(memory.reserve(range(PhysAddr{0x2000}, 0x4000), MemoryType::KernelData) == Status::Ok);
    require(memory.release(range(PhysAddr{0x3000}, 0x1000)) == Status::NoMemory);

    auto const collected = collect(memory, free_query());
    require(collected.count == 2);
    require(collected.regions[0].range.base == PhysAddr{0x1000});
    require(collected.regions[0].range.size == 0x1000);
    require(collected.regions[1].range.base == PhysAddr{0x6000});
    require(collected.regions[1].range.size == 0x3000);
}

auto trim_discards_partial_pages_and_updates_bounds() -> void {
    auto fixture = Fixture{};
    require(fixture.memory().add_usable(range(PhysAddr{0x1003}, 0x3FFE), NodeId{0}) == Status::Ok);
    require(fixture.memory().trim_usable(0x1000) == Status::Ok);
    require(fixture.memory().min_address() == PhysAddr{0x2000});
    require(fixture.memory().max_address() == PhysAddr{0x5000});

    auto const collected = collect(fixture.memory(), {
                                                         .lower = PhysAddr{0x1000},
                                                         .upper = PhysAddr{0x6000},
                                                         .node = bootmem::ANY_NODE,
                                                         .view = MemoryView::Usable,
                                                     });
    require(collected.count == 1);
    require(collected.regions[0].range.base == PhysAddr{0x2000});
    require(collected.regions[0].range.size == 0x3000);
}
} // namespace

auto main() -> int {
    allocation_is_exact_and_bottom_up();
    allocation_is_exact_and_top_down();
    free_query_reports_the_reservation_tail();
    node_assignment_splits_usable_memory();
    overlapping_usable_ranges_are_coalesced_per_node();
    free_query_can_stop_early();
    reservation_capacity_failure_is_transactional();
    range_rejects_empty_and_overflowing_values();
    release_splits_only_the_requested_reservation();
    release_capacity_failure_is_transactional();
    trim_discards_partial_pages_and_updates_bounds();
    return 0;
}
