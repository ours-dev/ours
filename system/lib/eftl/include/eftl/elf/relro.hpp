// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_RELRO_HPP
#define EFTL_ELF_RELRO_HPP 1

#include <eftl/elf/details/segment.hpp>
#include <eftl/elf/table.hpp>
#include <eftl/elf/traits.hpp>

#include <limits>

#include <ours/types.hpp>
#include <ustl/option.hpp>

namespace eftl::elf {
/// A copied PT_GNU_RELRO virtual range with a checked exclusive end.
template <typename Traits>
class RelroRegion final {
  public:
    using Segment = typename Traits::Segment;

    [[nodiscard]]
    static auto from(Segment const &segment) noexcept -> ustl::Option<RelroRegion> {
        auto const virtual_start = static_cast<u64>(segment.p_vaddr);
        auto const memory_size = static_cast<u64>(segment.p_memsz);
        if (segment.type() != SegType::GnuRelro ||
            memory_size > std::numeric_limits<u64>::max() - virtual_start) {
            return ustl::NONE;
        }
        return ustl::some(RelroRegion(virtual_start, virtual_start + memory_size));
    }

    [[nodiscard]]
    constexpr auto virtual_start() const noexcept -> u64 {
        return m_virtual_start;
    }

    [[nodiscard]]
    constexpr auto virtual_end() const noexcept -> u64 {
        return m_virtual_end;
    }

  private:
    constexpr RelroRegion(u64 virtual_start, u64 virtual_end) noexcept
        : m_virtual_start(virtual_start), m_virtual_end(virtual_end) {}

    u64 m_virtual_start{0};
    u64 m_virtual_end{0};
};

/// A bounded, allocation-free collection of every PT_GNU_RELRO program header.
template <typename Traits>
class RelroRegions final {
  public:
    using Segment = typename Traits::Segment;
    using Segments = TableView<Segment>;

    constexpr RelroRegions() noexcept = default;

    [[nodiscard]]
    static auto from(Segments segments) noexcept -> ustl::Option<RelroRegions> {
        usize count = 0;
        for (auto const segment: segments) {
            if (segment.type() != SegType::GnuRelro) {
                continue;
            }
            if (RelroRegion<Traits>::from(segment).is_none() ||
                count == std::numeric_limits<usize>::max()) {
                return ustl::NONE;
            }
            ++count;
        }
        return ustl::some(RelroRegions(segments, count));
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return m_count;
    }

    [[nodiscard]]
    auto region_at(usize index) const noexcept -> ustl::Option<RelroRegion<Traits>> {
        if (index >= m_count) {
            return ustl::NONE;
        }

        usize current = 0;
        for (auto const segment: m_segments) {
            if (segment.type() != SegType::GnuRelro) {
                continue;
            }
            auto const region = RelroRegion<Traits>::from(segment);
            if (region.is_none()) {
                return ustl::NONE;
            }
            if (current == index) {
                return region;
            }
            ++current;
        }
        return ustl::NONE;
    }

  private:
    constexpr RelroRegions(Segments segments, usize count) noexcept
        : m_segments(segments), m_count(count) {}

    Segments m_segments{};
    usize m_count{0};
};
} // namespace eftl::elf

#endif // EFTL_ELF_RELRO_HPP
