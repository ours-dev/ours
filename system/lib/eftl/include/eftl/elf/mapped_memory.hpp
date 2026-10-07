/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_MAPPED_MEMORY_HPP
#define EFTL_ELF_MAPPED_MEMORY_HPP 1

#include <eftl/elf/load_policy.hpp>
#include <eftl/elf/traits.hpp>

#include <limits>
#include <type_traits>

#include <ours/types.hpp>
#include <ustl/io/direct_memory.hpp>
#include <ustl/views/span.hpp>

namespace eftl::elf {
// Maps ELF virtual addresses through the loader's explicit ELF load intervals.
template <typename Traits>
class MappedMemory final {
  public:
    using Addr = typename Traits::Addr;

    MappedMemory(void *data, usize size, ustl::views::Span<LoadRegion const> regions) noexcept
        : m_memory(data, effective_size(data, size))
        , m_regions(regions)
        , m_backing_is_valid(data != nullptr || size == 0) {}

    template <typename T>
    [[nodiscard]]
    auto read_at(Addr address) noexcept -> T * {
        static_assert(std::is_trivially_copyable_v<T>);
        auto const offset = translate(address, sizeof(T), false);
        if (offset == INVALID_OFFSET) {
            return nullptr;
        }
        return m_memory.template read_at<T>(offset);
    }

    template <typename T>
    [[nodiscard]]
    auto read_at_n(Addr address, usize count) noexcept -> ustl::views::Span<T> {
        static_assert(std::is_trivially_copyable_v<T>);
        if (count > std::numeric_limits<usize>::max() / sizeof(T)) {
            return {};
        }
        auto const offset = translate(address, count * sizeof(T), false);
        if (offset == INVALID_OFFSET) {
            return {};
        }
        return m_memory.template read_at_n<T>(offset, count);
    }

    template <typename T>
    [[nodiscard]]
    auto write(Addr address, T const &value) noexcept -> bool {
        static_assert(std::is_trivially_copyable_v<T>);
        auto const offset = translate(address, sizeof(T), true);
        return offset != INVALID_OFFSET && m_memory.template write<T>(offset, value);
    }

  private:
    static constexpr usize INVALID_OFFSET = std::numeric_limits<usize>::max();

    [[nodiscard]]
    static constexpr auto effective_size(void *data, usize size) noexcept -> usize {
        if (data == nullptr) {
            return 0;
        }
        return size;
    }

    [[nodiscard]]
    auto translate(Addr address, usize request_size, bool requires_write) const noexcept -> usize {
        if (!m_backing_is_valid) {
            return INVALID_OFFSET;
        }
        if constexpr (sizeof(usize) > sizeof(u64)) {
            if (request_size > static_cast<usize>(std::numeric_limits<u64>::max())) {
                return INVALID_OFFSET;
            }
        }

        auto const virtual_address = static_cast<u64>(address);
        auto const virtual_size = static_cast<u64>(request_size);
        if (virtual_address > std::numeric_limits<u64>::max() - virtual_size) {
            return INVALID_OFFSET;
        }
        auto const virtual_end = virtual_address + virtual_size;

        for (auto const &region: m_regions) {
            if (region.virtual_end < region.virtual_start) {
                return INVALID_OFFSET;
            }
        }

        auto mapped_offset = INVALID_OFFSET;
        bool found_mapping = false;
        for (auto const &region: m_regions) {
            if (virtual_address < region.virtual_start || virtual_end > region.virtual_end) {
                continue;
            }
            if (found_mapping) {
                return INVALID_OFFSET;
            }
            if (requires_write &&
                static_cast<unsigned>(region.permissions & SegPerms::Writable) == 0U) {
                return INVALID_OFFSET;
            }

            auto const virtual_offset = virtual_address - region.virtual_start;
            if constexpr (sizeof(u64) > sizeof(usize)) {
                if (virtual_offset > static_cast<u64>(std::numeric_limits<usize>::max())) {
                    return INVALID_OFFSET;
                }
            }
            auto const native_virtual_offset = static_cast<usize>(virtual_offset);
            if (region.image_offset > std::numeric_limits<usize>::max() - native_virtual_offset) {
                return INVALID_OFFSET;
            }
            auto const image_offset = region.image_offset + native_virtual_offset;
            if (image_offset <= m_memory.size() && request_size <= m_memory.size() - image_offset) {
                mapped_offset = image_offset;
                found_mapping = true;
                continue;
            }
            return INVALID_OFFSET;
        }
        return mapped_offset;
    }

    ustl::io::DirectMemory m_memory;
    ustl::views::Span<LoadRegion const> m_regions;
    /// Whether the caller's buffer was usable: not null, or empty. The one
    /// fact m_memory cannot carry, since a null buffer is shrunk to size 0
    /// above and then looks like an honest empty one; and the object's only
    /// yes/no fact, so it has no word to share.
    bool const m_backing_is_valid;
};
} // namespace eftl::elf

#endif // EFTL_ELF_MAPPED_MEMORY_HPP
