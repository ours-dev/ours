/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_IO_DIRECT_MEMORY_HPP
#define USTL_IO_DIRECT_MEMORY_HPP 1

#include <ours/types.hpp>

#include <limits>
#include <type_traits>

#include <cstring>

#include <ustl/views/span.hpp>

namespace ustl::io {
struct DirectMemory final {
    DirectMemory() noexcept = default;

    explicit DirectMemory(ustl::views::Span<u8> data) noexcept : m_buffer(data) {}

    explicit DirectMemory(void *data, usize size) noexcept
        : m_buffer(static_cast<u8 *>(data), size) {}

    template <typename T>
    [[nodiscard]]
    auto read_at(usize offset = 0) noexcept -> T * {
        static_assert(std::is_trivially_copyable_v<T>);
        if (!contains(offset, sizeof(T))) {
            return nullptr;
        }
        if ((reinterpret_cast<usize>(m_buffer.data() + offset) % alignof(T)) != 0) {
            return nullptr;
        }
        return reinterpret_cast<T *>(m_buffer.data() + offset);
    }

    template <typename T>
    [[nodiscard]]
    auto read_at(usize offset = 0) const noexcept -> T const * {
        static_assert(std::is_trivially_copyable_v<T>);
        if (!contains(offset, sizeof(T))) {
            return nullptr;
        }
        if ((reinterpret_cast<usize>(m_buffer.data() + offset) % alignof(T)) != 0) {
            return nullptr;
        }
        return reinterpret_cast<T const *>(m_buffer.data() + offset);
    }

    template <typename T>
    [[nodiscard]]
    auto read_at_n(usize offset, usize count) noexcept -> ustl::views::Span<T> {
        static_assert(std::is_trivially_copyable_v<T>);
        if (count > std::numeric_limits<usize>::max() / sizeof(T)) {
            return {};
        }
        auto const size = count * sizeof(T);
        if (!contains(offset, size)) {
            return {};
        }
        if (count == 0) {
            return {};
        }
        if ((reinterpret_cast<usize>(m_buffer.data() + offset) % alignof(T)) != 0) {
            return {};
        }
        return {reinterpret_cast<T *>(m_buffer.data() + offset), count};
    }

    template <typename T>
    [[nodiscard]]
    auto read_at_n(usize offset, usize count) const noexcept -> ustl::views::Span<T const> {
        static_assert(std::is_trivially_copyable_v<T>);
        if (count > std::numeric_limits<usize>::max() / sizeof(T)) {
            return {};
        }
        auto const size = count * sizeof(T);
        if (!contains(offset, size)) {
            return {};
        }
        if (count == 0) {
            return {};
        }
        if ((reinterpret_cast<usize>(m_buffer.data() + offset) % alignof(T)) != 0) {
            return {};
        }
        return {reinterpret_cast<T const *>(m_buffer.data() + offset), count};
    }

    template <typename T>
    [[nodiscard]]
    auto read_and_cast(usize offset, usize size) noexcept -> ustl::views::Span<T> {
        if (size % sizeof(T) != 0) {
            return {};
        }
        return read_at_n<T>(offset, size / sizeof(T));
    }

    template <typename T>
    [[nodiscard]]
    auto read_and_cast(usize offset, usize size) const noexcept -> ustl::views::Span<T const> {
        if (size % sizeof(T) != 0) {
            return {};
        }
        return read_at_n<T>(offset, size / sizeof(T));
    }

    template <typename T>
    [[nodiscard]]
    auto write(usize offset, T const &value) noexcept -> bool {
        static_assert(std::is_trivially_copyable_v<T>);
        auto *const destination = read_at<T>(offset);
        if (destination == nullptr || (reinterpret_cast<usize>(destination) % alignof(T)) != 0) {
            return false;
        }
        std::memcpy(destination, &value, sizeof(T));
        return true;
    }

    auto reset(void const *data, usize size) noexcept -> void {
        m_buffer = {const_cast<u8 *>(static_cast<u8 const *>(data)), size};
    }

    [[nodiscard]]
    constexpr auto empty() const noexcept -> bool {
        return m_buffer.empty();
    }
    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return m_buffer.size();
    }
    [[nodiscard]]
    constexpr auto address() noexcept -> u8 * {
        return m_buffer.data();
    }
    [[nodiscard]]
    constexpr auto address() const noexcept -> u8 const * {
        return m_buffer.data();
    }

  private:
    [[nodiscard]]
    constexpr auto contains(usize offset, usize size) const noexcept -> bool {
        return offset <= m_buffer.size() && size <= m_buffer.size() - offset;
    }

    ustl::views::Span<u8> m_buffer{};
};
} // namespace ustl::io

#endif // USTL_IO_DIRECT_MEMORY_HPP
