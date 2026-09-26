/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef BOOTMEM_PHYSICAL_MEMORY_HPP
#define BOOTMEM_PHYSICAL_MEMORY_HPP 1

#include <bootmem/types.hpp>

#include <limits>

namespace bootmem {
class PhysicalMemory final {
  public:
    constexpr explicit PhysicalMemory(PhysicalMemoryStorage const storage) : storage_(storage) {}

    [[nodiscard]]
    auto add_usable(Range const range, NnId const node) -> Status {
        if (auto const status = range_status(range); status != Status::Ok) {
            return status;
        }

        auto merged = range;
        auto changed = true;
        while (changed) {
            changed = false;
            for (auto const region: storage_.usable.subspan(0, usable_count_)) {
                if (region.node != node ||
                    (!overlaps(region.range, merged) && region.range.end() != merged.base &&
                     region.range.base != merged.end())) {
                    continue;
                }

                auto const expanded = make_range(min_value(merged.base, region.range.base),
                                                 max_value(merged.end(), region.range.end()));
                if (expanded.base != merged.base || expanded.size != merged.size) {
                    merged = expanded;
                    changed = true;
                }
            }
        }

        auto erase_begin = usable_count_;
        auto erase_end = usize{0};
        for (auto current = usize{0}; current < usable_count_; ++current) {
            auto const region = storage_.usable[current];
            if (region.node != node) {
                if (overlaps(region.range, merged)) {
                    return Status::AlreadyExists;
                }
                continue;
            }
            if (overlaps(region.range, merged)) {
                if (erase_begin == usable_count_) {
                    erase_begin = current;
                }
                erase_end = current + 1;
            }
        }

        if (erase_begin == usable_count_) {
            erase_begin = 0;
            while (erase_begin < usable_count_ &&
                   storage_.usable[erase_begin].range.end() <= merged.base) {
                ++erase_begin;
            }
            erase_end = erase_begin;
        }

        auto const erase_count = erase_end - erase_begin;
        if (usable_count_ - erase_count + 1 > storage_.usable.size()) {
            return Status::NoMemory;
        }

        replace_usable(erase_begin, erase_count, {.range = merged, .node = node});
        return Status::Ok;
    }

    /// Claims |range| for |type|.
    ///
    /// Only reservations of the same type are merged. A merge is a claim that
    /// two ranges are interchangeable, and two reservations of different types
    /// are not: coalescing a page table with the phys image beside it would
    /// erase the very fact the type exists to carry. The cost is that the list
    /// can hold more entries than it used to for the same memory -- the bound is
    /// one entry per maximal same-type run.
    ///
    /// A *different* type already overlapping any of it is refused rather than
    /// silently retyped: it means two subsystems each believe they own the
    /// range, which is the caller's bug, and quietly picking a winner would
    /// destroy the one fact this is here to record.
    [[nodiscard]]
    auto reserve(Range const range, MemoryType const type) -> Status {
        if (auto const status = range_status(range); status != Status::Ok) {
            return status;
        }
        if (type == MemoryType::Free) {
            // "Reserve it as free" has no meaning; release() is how a range
            // goes back.
            return Status::InvArgs;
        }

        // Grow `merged` over every same-type reservation it touches, repeating
        // until it stops growing: swallowing one entry can bring the next within
        // reach, so a single pass is not enough. add_usable() coalesces with the
        // same shape for the same reason.
        auto merged = range;
        auto changed = true;
        while (changed) {
            changed = false;
            for (auto index = usize{0}; index < reservation_count_; ++index) {
                auto const other = storage_.reservations[index];
                if (other.type != type) {
                    continue;
                }
                if (!overlaps(other.range, merged) && other.range.end() != merged.base &&
                    other.range.base != merged.end()) {
                    continue;
                }
                auto const expanded = make_range(min_value(merged.base, other.range.base),
                                                 max_value(merged.end(), other.range.end()));
                if (expanded.base != merged.base || expanded.size != merged.size) {
                    merged = expanded;
                    changed = true;
                }
            }
        }

        for (auto index = usize{0}; index < reservation_count_; ++index) {
            auto const other = storage_.reservations[index];
            if (other.type != type && overlaps(other.range, merged)) {
                return Status::AlreadyExists;
            }
        }

        // Drop the same-type entries `merged` has absorbed. Each one is now
        // contained in it by construction, so containment is the test.
        for (auto index = usize{0}; index < reservation_count_;) {
            auto const other = storage_.reservations[index];
            if (other.type == type && merged.base <= other.range.base &&
                other.range.end() <= merged.end()) {
                erase_reservation(index);
                continue;
            }
            ++index;
        }

        if (reservation_count_ + 1 > storage_.reservations.size()) {
            return Status::NoMemory;
        }

        // Insert where the list stays sorted by base. Everything downstream --
        // find_free(), snapshot(), visit() -- walks it in address order and
        // stops at the first entry past the range it cares about, so an
        // out-of-order entry is not a slow list, it is a wrong answer. The
        // insertion point has to be computed from `merged`, not from `range`:
        // the merge can have moved the base downwards.
        auto index = usize{0};
        while (index < reservation_count_ &&
               storage_.reservations[index].range.base < merged.base) {
            ++index;
        }
        for (auto destination = reservation_count_; destination > index; --destination) {
            storage_.reservations[destination] = storage_.reservations[destination - 1];
        }
        storage_.reservations[index] = {.range = merged, .type = type};
        ++reservation_count_;
        return Status::Ok;
    }

    [[nodiscard]]
    auto allocate(AllocationRequest const request) -> ustl::Result<Range, Status> {
        if (auto const status = request_status(request); status != Status::Ok) {
            return ustl::err(status);
        }

        auto const allocation = find_free(request);
        if (allocation.is_err()) {
            return ustl::err(allocation.unwrap_err());
        }
        if (auto const status = reserve(allocation.unwrap(), request.type);
            status != Status::Ok) {
            return ustl::err(status);
        }
        return allocation;
    }

    [[nodiscard]]
    auto assign_node(Range const range, NnId const node) -> Status {
        if (auto const status = range_status(range); status != Status::Ok) {
            return status;
        }

        auto additional = usize{0};
        for (auto index = usize{0}; index < usable_count_; ++index) {
            auto const region = storage_.usable[index];
            if (!overlaps(region.range, range) || region.node == node) {
                continue;
            }

            auto pieces = usize{1};
            if (region.range.base < range.base) {
                ++pieces;
            }
            if (range.end() < region.range.end()) {
                ++pieces;
            }
            additional += pieces - 1;
        }
        if (additional > storage_.usable.size() - usable_count_) {
            return Status::NoMemory;
        }

        for (auto index = usize{0}; index < usable_count_;) {
            auto const region = storage_.usable[index];
            if (!overlaps(region.range, range) || region.node == node) {
                ++index;
                continue;
            }

            auto const start = max_value(region.range.base, range.base);
            auto const end = min_value(region.range.end(), range.end());
            MemoryRegion pieces[3]{};
            auto piece_count = usize{0};
            if (region.range.base < start) {
                pieces[piece_count++] = {
                    .range = make_range(region.range.base, start),
                    .node = region.node,
                };
            }
            pieces[piece_count++] = {
                .range = make_range(start, end),
                .node = node,
            };
            if (end < region.range.end()) {
                pieces[piece_count++] = {
                    .range = make_range(end, region.range.end()),
                    .node = region.node,
                };
            }
            replace_usable_region(index, pieces, piece_count);
            index += piece_count;
        }
        coalesce_usable();
        return Status::Ok;
    }

    [[nodiscard]]
    auto release(Range const range) -> Status {
        if (auto const status = range_status(range); status != Status::Ok) {
            return status;
        }

        auto additional = usize{0};
        for (auto index = usize{0}; index < reservation_count_; ++index) {
            auto const reserved = storage_.reservations[index].range;
            if (overlaps(reserved, range) && reserved.base < range.base &&
                range.end() < reserved.end()) {
                ++additional;
            }
        }
        if (additional > storage_.reservations.size() - reservation_count_) {
            return Status::NoMemory;
        }

        for (auto index = usize{0}; index < reservation_count_;) {
            // Whatever is left of a reservation after the hole is punched keeps
            // its type: releasing part of a range says nothing about what the
            // rest of it is.
            auto const type = storage_.reservations[index].type;
            auto const reserved = storage_.reservations[index].range;
            if (!overlaps(reserved, range)) {
                ++index;
                continue;
            }

            auto const start = max_value(reserved.base, range.base);
            auto const end = min_value(reserved.end(), range.end());
            auto const has_left = reserved.base < start;
            auto const has_right = end < reserved.end();
            if (has_left && has_right) {
                for (auto source = reservation_count_; source > index + 1; --source) {
                    storage_.reservations[source] = storage_.reservations[source - 1];
                }
                storage_.reservations[index] = {make_range(reserved.base, start), type};
                storage_.reservations[index + 1] = {make_range(end, reserved.end()), type};
                ++reservation_count_;
                index += 2;
            } else if (has_left) {
                storage_.reservations[index] = {make_range(reserved.base, start), type};
                ++index;
            } else if (has_right) {
                storage_.reservations[index] = {make_range(end, reserved.end()), type};
                ++index;
            } else {
                erase_reservation(index);
            }
        }
        return Status::Ok;
    }

    [[nodiscard]]
    auto trim_usable(usize const alignment) -> Status {
        if (!is_power_of_two(alignment)) {
            return Status::InvArgs;
        }

        for (auto index = usize{0}; index < usable_count_;) {
            auto &region = storage_.usable[index];
            auto start = PhysAddr{};
            auto const end = align_down(region.range.end(), alignment);
            if (!align_up(region.range.base, alignment, &start) || start >= end) {
                erase_usable(index);
                continue;
            }
            region.range = make_range(start, end);
            ++index;
        }
        coalesce_usable();
        return Status::Ok;
    }

    /// The most entries snapshot() can ever write: each of the usable_count_
    /// regions, split by at most every reservation it overlaps (each
    /// reservation contributes at most one extra split point beyond the
    /// region it falls within).  `extra_reservations` accounts for
    /// reservations the caller is about to add (e.g. for a buffer to hold
    /// the snapshot itself) but hasn't yet -- each one costs the same two
    /// extra entries a reservation already on the books would.
    [[nodiscard]]
    constexpr auto max_snapshot_entries(usize const extra_reservations = 0) const -> usize {
        return usable_count_ + 2 * (reservation_count_ + extra_reservations);
    }

    /// The final, read-only memory map: one MemRange per maximal run of usable
    /// RAM that is all one MemoryType, in address order. This is what crosses a
    /// handoff boundary to a consumer that should read the map rather than share
    /// this live, mutable allocator -- see ours::phys::MemoryHandoff. Returns
    /// NoMemory if `out` is smaller than max_snapshot_entries().
    ///
    /// The type is the whole point of the snapshot now: the consumer on the far
    /// side has no other way to learn what a reserved range holds, and once the
    /// jump is made there is nothing left to ask.
    [[nodiscard]]
    auto snapshot(ustl::views::Span<MemRange> const out) const
        -> ustl::Result<ustl::views::Span<MemRange const>, Status> {
        auto count = usize{0};
        auto const emit = [&](Range const range, MemoryType const type) -> bool {
            if (count >= out.size()) {
                return false;
            }
            out[count++] = MemRange{.range = range, .type = type};
            return true;
        };

        for (auto usable_index = usize{0}; usable_index < usable_count_; ++usable_index) {
            auto const usable = storage_.usable[usable_index];
            auto const end = usable.range.end();
            auto cursor = usable.range.base;

            for (auto reservation_index = usize{0}; reservation_index < reservation_count_;
                 ++reservation_index) {
                auto const reserved = storage_.reservations[reservation_index].range;
                auto const type = storage_.reservations[reservation_index].type;
                if (reserved.end() <= cursor) {
                    continue;
                }
                if (reserved.base >= end) {
                    break;
                }
                if (cursor < reserved.base) {
                    if (!emit(make_range(cursor, reserved.base), MemoryType::Free)) {
                        return ustl::err(Status::NoMemory);
                    }
                    cursor = reserved.base;
                }
                auto const used_end = min_value(reserved.end(), end);
                if (cursor < used_end) {
                    if (!emit(make_range(cursor, used_end), type)) {
                        return ustl::err(Status::NoMemory);
                    }
                    cursor = used_end;
                }
                if (cursor >= end) {
                    break;
                }
            }
            if (cursor < end && !emit(make_range(cursor, end), MemoryType::Free)) {
                return ustl::err(Status::NoMemory);
            }
        }
        return ustl::ok(ustl::views::Span<MemRange const>(out.data(), count));
    }

    /// Every reservation, in address order, with what it is for.
    ///
    /// Separate from visit() rather than a third MemoryView: the usable and free
    /// views answer "which addresses", and their visitor sees a MemoryRegion,
    /// which has a node but no type. A caller that wants the type wants the
    /// reservation itself, so that is what it gets. Returning false stops the
    /// walk, as everywhere else here.
    ///
    /// Mutating this allocator from the visitor is not allowed: reserve() and
    /// release() both reorder the list under it.
    template <typename Visitor>
    auto visit_reservations(Visitor &&visitor) const -> void {
        for (auto index = usize{0}; index < reservation_count_; ++index) {
            if (!visitor(storage_.reservations[index])) {
                return;
            }
        }
    }

    [[nodiscard]]
    constexpr auto min_address() const -> PhysAddr {
        return usable_count_ == 0 ? PhysAddr{} : storage_.usable[0].range.base;
    }

    [[nodiscard]]
    constexpr auto max_address() const -> PhysAddr {
        return usable_count_ == 0 ? PhysAddr{} : storage_.usable[usable_count_ - 1].range.end();
    }

    [[nodiscard]]
    auto visit(MemoryQuery const query, MemoryVisitor const visitor,
               void *const visitor_context) const -> Status {
        if (auto const status = query_status(query); status != Status::Ok) {
            return status;
        }
        if (visitor == nullptr) {
            return Status::InvArgs;
        }

        for (auto usable_index = usize{0}; usable_index < usable_count_; ++usable_index) {
            auto const usable = storage_.usable[usable_index];
            if (query.node != ANY_NODE && usable.node != query.node) {
                continue;
            }

            auto const start = max_value(usable.range.base, query.lower);
            auto const end = min_value(usable.range.end(), query.upper);
            if (start >= end) {
                continue;
            }

            if (query.view == MemoryView::Usable) {
                if (!visitor(visitor_context,
                             {.range = make_range(start, end), .node = usable.node})) {
                    return Status::Ok;
                }
                continue;
            }

            auto free_start = start;
            for (auto reservation_index = usize{0}; reservation_index < reservation_count_;
                 ++reservation_index) {
                auto const reserved = storage_.reservations[reservation_index].range;
                if (reserved.end() <= free_start) {
                    continue;
                }
                if (reserved.base >= end) {
                    break;
                }
                if (free_start < reserved.base &&
                    !visitor(visitor_context,
                             {.range = make_range(free_start, min_value(reserved.base, end)),
                              .node = usable.node})) {
                    return Status::Ok;
                }
                free_start = max_value(free_start, min_value(reserved.end(), end));
                if (free_start >= end) {
                    break;
                }
            }
            if (free_start < end && !visitor(visitor_context, {.range = make_range(free_start, end),
                                                               .node = usable.node})) {
                return Status::Ok;
            }
        }
        return Status::Ok;
    }

    template <typename Visitor>
    [[nodiscard]]
    auto visit(MemoryQuery const query, Visitor &&visitor) const -> Status {
        struct VisitorContext final {
            Visitor &visitor;
        };

        auto context = VisitorContext{.visitor = visitor};
        auto const callback = [](void *const visitor_context, MemoryRegion const region) -> bool {
            auto &context = *static_cast<VisitorContext *>(visitor_context);
            return context.visitor(region);
        };
        return visit(query, callback, &context);
    }

  private:
    [[nodiscard]]
    static constexpr auto range_status(Range const range) -> Status {
        if (range.size == 0) {
            return Status::InvArgs;
        }
        if (range.base > std::numeric_limits<PhysAddr>::max() - range.size) {
            return Status::OutOfRange;
        }
        return Status::Ok;
    }

    [[nodiscard]]
    static constexpr auto is_power_of_two(usize const value) -> bool {
        return value != 0 && (value & (value - 1)) == 0;
    }

    [[nodiscard]]
    static constexpr auto overlaps(Range const left, Range const right) -> bool {
        return left.base < right.end() && right.base < left.end();
    }

    [[nodiscard]]
    static constexpr auto max_value(PhysAddr const left, PhysAddr const right) -> PhysAddr {
        return left < right ? right : left;
    }

    [[nodiscard]]
    static constexpr auto min_value(PhysAddr const left, PhysAddr const right) -> PhysAddr {
        return left < right ? left : right;
    }

    [[nodiscard]]
    static constexpr auto make_range(PhysAddr const base, PhysAddr const end) -> Range {
        return {.base = base, .size = end - base};
    }

    [[nodiscard]]
    static constexpr auto align_up(PhysAddr const value, usize const alignment,
                                   PhysAddr *const result) -> bool {
        auto const adjustment = alignment - 1;
        if (value > std::numeric_limits<PhysAddr>::max() - adjustment) {
            return false;
        }
        *result = (value + adjustment) & ~static_cast<PhysAddr>(adjustment);
        return true;
    }

    [[nodiscard]]
    static constexpr auto align_down(PhysAddr const value, usize const alignment) -> PhysAddr {
        return value & ~static_cast<PhysAddr>(alignment - 1);
    }

    [[nodiscard]]
    static constexpr auto request_status(AllocationRequest const request) -> Status {
        if (request.size == 0 || !is_power_of_two(request.alignment) ||
            request.lower >= request.upper) {
            return Status::InvArgs;
        }
        if (request.size > request.upper - request.lower) {
            return Status::NoMemory;
        }
        return Status::Ok;
    }

    [[nodiscard]]
    static constexpr auto query_status(MemoryQuery const query) -> Status {
        return query.lower < query.upper ? Status::Ok : Status::InvArgs;
    }

    auto replace_usable(usize const index, usize const erase_count, MemoryRegion const region)
        -> void {
        for (auto destination = index; destination + erase_count < usable_count_; ++destination) {
            storage_.usable[destination] = storage_.usable[destination + erase_count];
        }
        usable_count_ -= erase_count;
        for (auto destination = usable_count_; destination > index; --destination) {
            storage_.usable[destination] = storage_.usable[destination - 1];
        }
        storage_.usable[index] = region;
        ++usable_count_;
    }

    auto erase_usable(usize const index) -> void {
        for (auto destination = index; destination + 1 < usable_count_; ++destination) {
            storage_.usable[destination] = storage_.usable[destination + 1];
        }
        --usable_count_;
    }

    auto erase_reservation(usize const index) -> void {
        for (auto destination = index; destination + 1 < reservation_count_; ++destination) {
            storage_.reservations[destination] = storage_.reservations[destination + 1];
        }
        --reservation_count_;
    }

    auto replace_usable_region(usize const index, MemoryRegion const *const regions,
                               usize const region_count) -> void {
        auto const additional = region_count - 1;
        for (auto source = usable_count_; source > index + 1; --source) {
            storage_.usable[source - 1 + additional] = storage_.usable[source - 1];
        }
        usable_count_ += additional;
        for (auto offset = usize{0}; offset < region_count; ++offset) {
            storage_.usable[index + offset] = regions[offset];
        }
    }

    auto coalesce_usable() -> void {
        auto destination = usize{0};
        for (auto source = usize{0}; source < usable_count_; ++source) {
            auto const region = storage_.usable[source];
            if (destination != 0 && storage_.usable[destination - 1].node == region.node &&
                storage_.usable[destination - 1].range.end() == region.range.base) {
                storage_.usable[destination - 1].range.size += region.range.size;
                continue;
            }
            storage_.usable[destination++] = region;
        }
        usable_count_ = destination;
    }

    [[nodiscard]]
    auto find_free(AllocationRequest const request) const -> ustl::Result<Range, Status> {
        auto found = false;
        auto candidate = Range{};

        auto const consider = [&](PhysAddr const start, PhysAddr const end) -> bool {
            if (end - start < request.size) {
                return true;
            }

            if (request.direction == AllocationDirection::BottomUp) {
                auto aligned = PhysAddr{};
                if (!align_up(start, request.alignment, &aligned) || aligned > end ||
                    request.size > end - aligned) {
                    return true;
                }
                candidate = make_range(aligned, aligned + request.size);
                found = true;
                return false;
            }

            auto const aligned = align_down(end - request.size, request.alignment);
            if (aligned < start) {
                return true;
            }
            candidate = make_range(aligned, aligned + request.size);
            found = true;
            return true;
        };

        for (auto usable_index = usize{0}; usable_index < usable_count_; ++usable_index) {
            auto const usable = storage_.usable[usable_index];
            if (request.node != ANY_NODE && usable.node != request.node) {
                continue;
            }

            auto const start = max_value(usable.range.base, request.lower);
            auto const end = min_value(usable.range.end(), request.upper);
            if (start >= end) {
                continue;
            }

            auto free_start = start;
            for (auto reservation_index = usize{0}; reservation_index < reservation_count_;
                 ++reservation_index) {
                auto const reserved = storage_.reservations[reservation_index].range;
                if (reserved.end() <= free_start) {
                    continue;
                }
                if (reserved.base >= end) {
                    break;
                }
                if (free_start < reserved.base &&
                    !consider(free_start, min_value(reserved.base, end))) {
                    return ustl::ok(candidate);
                }
                free_start = max_value(free_start, min_value(reserved.end(), end));
                if (free_start >= end) {
                    break;
                }
            }

            if (free_start < end && !consider(free_start, end)) {
                return ustl::ok(candidate);
            }
        }

        if (!found) {
            return ustl::err(Status::NoMemory);
        }
        return ustl::ok(candidate);
    }

    PhysicalMemoryStorage storage_;
    usize usable_count_{};
    usize reservation_count_{};
};
} // namespace bootmem

#endif // BOOTMEM_PHYSICAL_MEMORY_HPP
