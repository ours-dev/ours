/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_LOAD_POLICY_HPP
#define EFTL_ELF_LOAD_POLICY_HPP 1

#include <eftl/binary_stream.hpp>
#include <eftl/elf/binary.hpp>
#include <eftl/elf/details/segment.hpp>
#include <eftl/elf/enums.hpp>
#include <eftl/elf/relro.hpp>

#include <limits>
#include <type_traits>

#include <ours/types.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

namespace eftl::elf {
// An ELF-native image interval materialized by the loader policy.
// Keep this public aggregate in ELF vocabulary for designated initializers.
// NOLINTBEGIN(readability-identifier-naming)
struct LoadRegion final {
    u64 virtual_start{0};
    u64 virtual_end{0};
    usize image_offset{0};
    SegPerms permissions{SegPerms::None};
};

// One PT_GNU_RELRO range: the part of a load segment that stops being
// writable once the loader is done relocating.  Reported exactly as the
// program header spells it -- rounding it to whole pages needs the enclosing
// segment's bounds, which only the code doing the mapping has.
struct RelroInterval final {
    u64 virtual_start{0};
    u64 virtual_end{0};
};
// NOLINTEND(readability-identifier-naming)

enum class LoadError {
    UnsupportedType,
    UnsupportedMachine,
    NoLoadSegments,
    UnmappedEntrypoint,
    InvalidSegment,
    InvalidFileRange,
    InvalidAlignment,
    AddressOverflow,
    ImageTooLarge,
    OverlappingSegments,
    OverlappingPageMappings,
    CapacityExceeded,
};

[[nodiscard]]
constexpr auto to_string(LoadError error) noexcept -> char const * {
    switch (error) {
        case LoadError::UnsupportedType:
            return "Unsupported ELF type";
        case LoadError::UnsupportedMachine:
            return "Unsupported ELF machine";
        case LoadError::NoLoadSegments:
            return "No loadable segments";
        case LoadError::UnmappedEntrypoint:
            return "Entrypoint is not mapped";
        case LoadError::InvalidSegment:
            return "Invalid load segment";
        case LoadError::InvalidFileRange:
            return "Invalid load segment file range";
        case LoadError::InvalidAlignment:
            return "Invalid load segment alignment";
        case LoadError::AddressOverflow:
            return "Load segment address overflow";
        case LoadError::ImageTooLarge:
            return "Load image is too large";
        case LoadError::OverlappingSegments:
            return "Overlapping load segments";
        case LoadError::OverlappingPageMappings:
            return "Overlapping load mapping pages";
        case LoadError::CapacityExceeded:
            return "Load segment capacity exceeded";
    }
    return "Unknown ELF load error";
}

template <typename Traits, template <typename> typename Collection>
class LoadPlan final {
  public:
    using Segment = typename Traits::Segment;

    // These names reflect ELF program-header fields and loader operations.
    // NOLINTBEGIN(readability-identifier-naming)
    struct LoadSegment final {
        LoadRegion region{};
        ByteRange content{};
        u64 file_size{0};
        u64 memory_size{0};
        u64 alignment{0};
    };
    // NOLINTEND(readability-identifier-naming)

    [[nodiscard]]
    auto segments() const noexcept -> Collection<LoadSegment> const & {
        return m_segments;
    }

    [[nodiscard]]
    auto regions() const noexcept -> ustl::views::Span<LoadRegion const> {
        return {m_regions.data(), m_regions.size()};
    }

    [[nodiscard]]
    constexpr auto image_base() const noexcept -> u64 {
        return m_image_base;
    }

    [[nodiscard]]
    constexpr auto image_size() const noexcept -> usize {
        return m_image_size;
    }

    [[nodiscard]]
    constexpr auto entrypoint() const noexcept -> u64 {
        return m_entrypoint;
    }

    /// Every PT_GNU_RELRO range the image carries, in its own link-time
    /// addresses.  There can legitimately be more than one: a linker script
    /// that scatters the RELRO sections -- as this repo's kernel.ld does,
    /// with `.dynamic` at the image head and `.init_array` at its tail --
    /// makes LLD emit one program header per contiguous run.
    [[nodiscard]]
    auto relro_intervals() const noexcept -> ustl::views::Span<RelroInterval const> {
        return {m_relro.data(), m_relro.size()};
    }

    [[nodiscard]]
    auto entry_offset() const noexcept -> ustl::Option<usize> {
        if (m_entrypoint < m_image_base) {
            return ustl::NONE;
        }
        auto const offset = m_entrypoint - m_image_base;
        if (offset > static_cast<u64>(std::numeric_limits<usize>::max())) {
            return ustl::NONE;
        }
        return ustl::some(static_cast<usize>(offset));
    }

  private:
    template <typename, template <typename> typename, Arch, usize>
    friend class KernelElfLoadPolicy;

    Collection<LoadSegment> m_segments;
    Collection<LoadRegion> m_regions;
    Collection<RelroInterval> m_relro;
    u64 m_image_base{0};
    usize m_image_size{0};
    u64 m_entrypoint{0};
};

template <typename Traits, template <typename> typename Collection, Arch ExpectedMachine,
          usize MappingPageSize = 1>
class KernelElfLoadPolicy final {
  public:
    using File = FileImpl<Traits>;
    using Image = BasicImage<Traits>;
    using Segment = typename Traits::Segment;
    using Plan = LoadPlan<Traits, Collection>;
    using LoadSegment = typename Plan::LoadSegment;

    static_assert(ExpectedMachine != Arch::None,
                  "A kernel load policy needs a concrete ELF machine");
    static_assert(MappingPageSize != 0 && (MappingPageSize & (MappingPageSize - 1)) == 0,
                  "The mapping page size must be a nonzero power of two");

    [[nodiscard]]
    static auto make_plan(Image const &image) -> ustl::Result<Plan, LoadError> {
        return make_plan(image.elf_file());
    }

  private:
    [[nodiscard]]
    static auto make_plan(File const &binary) -> ustl::Result<Plan, LoadError> {
        auto const &header = binary.elf_header();
        if (header.e_type != EXECUTABLE && header.e_type != SHARED_OBJECT) {
            return ustl::err(LoadError::UnsupportedType);
        }
        if (static_cast<Arch>(header.e_machine) != ExpectedMachine) {
            return ustl::err(LoadError::UnsupportedMachine);
        }

        Plan plan{};
        bool has_load_segment = false;
        auto image_base = std::numeric_limits<u64>::max();
        auto image_end = u64{0};

        for (auto const segment: binary.segments()) {
            if (segment.type() != SegType::Load) {
                continue;
            }

            auto const load_segment = make_segment(binary, segment);
            if (load_segment.is_err()) {
                return ustl::err(load_segment.unwrap_err());
            }
            auto const &candidate = load_segment.unwrap();
            for (auto const &existing: plan.m_segments) {
                if (overlaps(candidate.region, existing.region)) {
                    return ustl::err(LoadError::OverlappingSegments);
                }
            }

            auto const appended_segment = plan.m_segments.emplace_back(candidate);
            if (appended_segment.is_err()) {
                return ustl::err(LoadError::CapacityExceeded);
            }
            auto const appended_region = plan.m_regions.emplace_back(candidate.region);
            if (appended_region.is_err()) {
                return ustl::err(LoadError::CapacityExceeded);
            }

            has_load_segment = true;
            if (candidate.region.virtual_start < image_base) {
                image_base = candidate.region.virtual_start;
            }
            if (candidate.region.virtual_end > image_end) {
                image_end = candidate.region.virtual_end;
            }
        }

        if (!has_load_segment) {
            return ustl::err(LoadError::NoLoadSegments);
        }
        if (image_end < image_base) {
            return ustl::err(LoadError::AddressOverflow);
        }

        auto const image_size = to_usize(image_end - image_base);
        if (image_size.is_none()) {
            return ustl::err(LoadError::ImageTooLarge);
        }

        auto const relro_error = collect_relro(binary, plan, image_base, image_end);
        if (relro_error.is_some()) {
            return ustl::err(relro_error.value());
        }

        plan.m_image_base = image_base;
        plan.m_image_size = image_size.value();
        plan.m_entrypoint = static_cast<u64>(header.e_entry);

        bool entry_mapped = false;
        for (usize index = 0; index < plan.m_segments.size(); ++index) {
            auto &load_segment = plan.m_segments[index];
            auto &region = plan.m_regions[index];
            auto const image_offset = to_usize(load_segment.region.virtual_start - image_base);
            if (image_offset.is_none()) {
                return ustl::err(LoadError::ImageTooLarge);
            }
            load_segment.region.image_offset = image_offset.value();
            region.image_offset = image_offset.value();
            entry_mapped |= plan.m_entrypoint >= load_segment.region.virtual_start &&
                            plan.m_entrypoint < load_segment.region.virtual_end;
        }

        if (!has_nonoverlapping_mapping_pages(plan)) {
            return ustl::err(LoadError::OverlappingPageMappings);
        }

        if (!entry_mapped) {
            return ustl::err(LoadError::UnmappedEntrypoint);
        }
        return ustl::ok(std::move(plan));
    }

    static constexpr u16 EXECUTABLE = 2;
    static constexpr u16 SHARED_OBJECT = 3;

    struct MappingPageRange final {
        usize m_first_page{0};
        usize m_end_page{0};
    };

    template <typename Value>
    [[nodiscard]]
    static constexpr auto to_usize(Value value) noexcept -> ustl::Option<usize> {
        static_assert(std::is_integral_v<Value> && std::is_unsigned_v<Value>);
        if constexpr (sizeof(Value) > sizeof(usize)) {
            if (value > static_cast<Value>(std::numeric_limits<usize>::max())) {
                return ustl::NONE;
            }
        }
        return ustl::some(static_cast<usize>(value));
    }

    [[nodiscard]]
    static constexpr auto overlaps(LoadRegion const &lhs, LoadRegion const &rhs) noexcept -> bool {
        return lhs.virtual_start < rhs.virtual_end && rhs.virtual_start < lhs.virtual_end;
    }

    [[nodiscard]]
    static auto mapping_page_range(LoadSegment const &segment) noexcept
        -> ustl::Option<MappingPageRange> {
        auto const native_size = to_usize(segment.memory_size);
        if (native_size.is_none() || native_size.value() == 0) {
            return ustl::NONE;
        }
        auto const page_offset = segment.region.image_offset % MappingPageSize;
        if (native_size.value() > std::numeric_limits<usize>::max() - page_offset) {
            return ustl::NONE;
        }

        auto const byte_count = native_size.value() + page_offset;
        auto page_count = byte_count / MappingPageSize;
        if (byte_count % MappingPageSize != 0) {
            ++page_count;
        }
        auto const first_page = segment.region.image_offset / MappingPageSize;
        if (page_count == 0 || first_page > std::numeric_limits<usize>::max() - page_count) {
            return ustl::NONE;
        }
        return ustl::some(MappingPageRange{
            .m_first_page = first_page,
            .m_end_page = first_page + page_count,
        });
    }

    [[nodiscard]]
    static auto has_nonoverlapping_mapping_pages(Plan const &plan) noexcept -> bool {
        for (usize index = 0; index < plan.m_segments.size(); ++index) {
            auto const lhs = mapping_page_range(plan.m_segments[index]);
            if (lhs.is_none()) {
                return false;
            }
            for (usize other = index + 1; other < plan.m_segments.size(); ++other) {
                auto const rhs = mapping_page_range(plan.m_segments[other]);
                if (rhs.is_none() ||
                    (lhs->m_first_page < rhs->m_end_page && rhs->m_first_page < lhs->m_end_page)) {
                    return false;
                }
            }
        }
        return true;
    }

    /// Copies every PT_GNU_RELRO range into the plan, validated against the
    /// image bounds.  Empty ranges are dropped rather than stored, so a
    /// stored interval always describes real bytes.
    [[nodiscard]]
    static auto collect_relro(File const &binary, Plan &plan, u64 image_base, u64 image_end)
        -> ustl::Option<LoadError> {
        auto const regions = binary.relro_regions();
        for (usize index = 0; index < regions.size(); ++index) {
            auto const region = regions.region_at(index);
            if (region.is_none()) {
                return ustl::some(LoadError::InvalidSegment);
            }

            auto const start = region->virtual_start();
            auto const end = region->virtual_end();
            if (start < image_base || end > image_end) {
                return ustl::some(LoadError::InvalidSegment);
            }
            if (start >= end) {
                continue;
            }

            auto const appended = plan.m_relro.emplace_back(RelroInterval{
                .virtual_start = start,
                .virtual_end = end,
            });
            if (appended.is_err()) {
                return ustl::some(LoadError::CapacityExceeded);
            }
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    static auto make_segment(File const &binary, Segment const &segment)
        -> ustl::Result<LoadSegment, LoadError> {
        auto const file_size = static_cast<u64>(segment.p_filesz);
        auto const memory_size = static_cast<u64>(segment.p_memsz);
        auto const virtual_start = static_cast<u64>(segment.p_vaddr);
        auto const alignment = static_cast<u64>(segment.p_align);
        if (memory_size == 0 || file_size > memory_size) {
            return ustl::err(LoadError::InvalidSegment);
        }
        if (virtual_start > std::numeric_limits<u64>::max() - memory_size) {
            return ustl::err(LoadError::AddressOverflow);
        }
        if (alignment != 0 && (alignment & (alignment - 1)) != 0) {
            return ustl::err(LoadError::InvalidAlignment);
        }
        if (alignment > 1 &&
            static_cast<u64>(segment.p_offset) % alignment != virtual_start % alignment) {
            return ustl::err(LoadError::InvalidAlignment);
        }

        auto const file_offset = to_usize(segment.p_offset);
        auto const native_file_size = to_usize(file_size);
        if (file_offset.is_none() || native_file_size.is_none()) {
            return ustl::err(LoadError::InvalidFileRange);
        }
        auto const content = binary.file_bytes(file_offset.value(), native_file_size.value());
        if (content.is_none()) {
            return ustl::err(LoadError::InvalidFileRange);
        }

        return ustl::ok(LoadSegment{
            .region =
                {
                    .virtual_start = virtual_start,
                    .virtual_end = virtual_start + memory_size,
                    .image_offset = 0,
                    .permissions = segment.permissions(),
                },
            .content = content.value(),
            .file_size = file_size,
            .memory_size = memory_size,
            .alignment = alignment,
        });
    }
};
} // namespace eftl::elf

#endif // EFTL_ELF_LOAD_POLICY_HPP
