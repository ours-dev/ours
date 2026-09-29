/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef EFTL_ELF_LOAD_HPP
#define EFTL_ELF_LOAD_HPP 1

#include <eftl/elf/config.hpp>
#include <eftl/elf/segment.hpp>

#include <algorithm>
#include <limits>
#include <memory>

#include <ours/types.hpp>
#include <ustl/views/span.hpp>

namespace eftl::elf {
/// This helper class is used to calculate the size of the image in memory.
template <typename Traits>
struct VirtAddrRegionObserver
    : public SegmentObserver<VirtAddrRegionObserver<Traits>, SegType::Load> {
    using Addr = typename Traits::Addr;
    using Segment = typename Traits::Segment;

    VirtAddrRegionObserver(Addr &min, Addr &max)
        : m_va_maximal(std::addressof(max)), m_va_minimal(std::addressof(min)) {}

    ~VirtAddrRegionObserver() = default;

    template <typename Memory>
    FORCE_INLINE [[nodiscard]]
    constexpr auto observe(Memory &&memory, Segment const &segment, SegTypeMatcher<SegType::Load>)
        -> bool {
        (void)memory;
        *m_va_minimal = std::min(*m_va_minimal, static_cast<Addr>(segment.va_begin()));
        *m_va_maximal = std::max(*m_va_maximal, static_cast<Addr>(segment.va_limit()));
        return true;
    }

    FORCE_INLINE [[nodiscard]]
    constexpr auto finish() -> bool {
        return true;
    }

    Addr *m_va_maximal;
    Addr *m_va_minimal;
};

template <typename Traits, template <typename> typename Collection>
struct LoadInfoObserver;

template <typename Traits, template <typename> typename Collection>
class LoadInfo final {
  public:
    using Addr = typename Traits::Addr;

    // These names describe the public load plan and intentionally read as record fields.
    // NOLINTBEGIN(readability-identifier-naming)
    struct LoadableSegment {
        Addr va_start;
        Addr va_size;
        Addr file_size;
        Addr alignment;
        SegPerms perms;
        ustl::views::Span<u8 const> content;
    };
    // NOLINTEND(readability-identifier-naming)

    template <typename Visitor>
    [[nodiscard]]
    auto visit_segments(Visitor &&visitor) const -> bool {
        for (auto const &segment: m_segments) {
            if (!visitor(segment)) {
                return false;
            }
        }

        return true;
    }

    [[nodiscard]]
    auto make_observer() -> LoadInfoObserver<Traits, Collection> {
        return {*this};
    }

    [[nodiscard]]
    auto virtual_base() const noexcept -> Addr {
        return m_va_minimal;
    }

    [[nodiscard]]
    auto virtual_end() const noexcept -> Addr {
        return m_va_maximal;
    }

    [[nodiscard]]
    auto virtual_size() const noexcept -> usize {
        if (m_va_minimal == std::numeric_limits<Addr>::max() || m_va_maximal < m_va_minimal) {
            return 0;
        }
        return static_cast<usize>(m_va_maximal - m_va_minimal);
    }

  private:
    friend struct LoadInfoObserver<Traits, Collection>;

    Addr m_va_minimal{std::numeric_limits<Addr>::max()};
    Addr m_va_maximal{};
    Collection<LoadableSegment> m_segments;
};

template <typename Traits, template <typename> typename Collection>
struct LoadInfoObserver
    : public SegmentObserver<LoadInfoObserver<Traits, Collection>, SegType::Load> {
    using Segment = typename Traits::Segment;
    using LoadInfo = LoadInfo<Traits, Collection>;

    FORCE_INLINE
    constexpr LoadInfoObserver(LoadInfo &loadinfo)
        : m_loadinfo(std::addressof(loadinfo)) {}

    ~LoadInfoObserver() = default;

    template <typename Memory>
    FORCE_INLINE [[nodiscard]]
    constexpr auto observe(Memory &&memory, Segment const &segment, SegTypeMatcher<SegType::Load>)
        -> bool {
        (void)memory;
        if (segment.va_size() == 0) {
            return true;
        }

        auto const file_size = static_cast<typename LoadInfo::Addr>(segment.file_size());
        auto const memory_size = static_cast<typename LoadInfo::Addr>(segment.va_size());
        auto const file_offset = static_cast<usize>(segment.file_offset());
        auto const alignment = static_cast<typename LoadInfo::Addr>(segment.alignment());
        if (file_size > memory_size ||
            static_cast<typename LoadInfo::Addr>(segment.va_begin()) >
                std::numeric_limits<typename LoadInfo::Addr>::max() - memory_size ||
            (alignment != 0 && (alignment & (alignment - 1)) != 0) ||
            (alignment > 1 &&
             (file_offset % alignment) != (static_cast<usize>(segment.va_begin()) % alignment))) {
            return false;
        }

        auto const content =
            memory.template read_at_n<u8>(file_offset, static_cast<usize>(file_size));
        if (content.size() != static_cast<usize>(file_size)) {
            return false;
        }

        m_loadinfo->m_va_minimal = std::min(
            m_loadinfo->m_va_minimal, static_cast<typename LoadInfo::Addr>(segment.va_begin()));
        m_loadinfo->m_va_maximal = std::max(
            m_loadinfo->m_va_maximal, static_cast<typename LoadInfo::Addr>(segment.va_limit()));
        auto const old_size = m_loadinfo->m_segments.size();
        (void)m_loadinfo->m_segments.emplace_back(segment.va_begin(), segment.va_size(), file_size,
                                                  segment.alignment(), segment.permissions(),
                                                  content);

        return m_loadinfo->m_segments.size() == old_size + 1;
    }

    FORCE_INLINE [[nodiscard]]
    constexpr auto finish() -> bool {
        return true;
    }

    LoadInfo *m_loadinfo;
};

} // namespace eftl::elf

#endif // #ifndef EFTL_ELF_LOAD_HPP
