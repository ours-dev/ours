/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_BINARY_STREAM_HPP
#define EFTL_BINARY_STREAM_HPP 1

#include <type_traits>

#include <climits>

#include <ours/types.hpp>
#include <ustl/result.hpp>

namespace eftl {
enum class StreamError {
    NullData,
    OutOfRange,
};

class ByteRange final {
  public:
    constexpr ByteRange() noexcept = default;

    constexpr ByteRange(u8 const *data, usize size) noexcept : m_data(data), m_size(size) {}

    [[nodiscard]]
    constexpr auto data() const noexcept -> u8 const * {
        return m_data;
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return m_size;
    }

    [[nodiscard]]
    constexpr auto empty() const noexcept -> bool {
        return m_size == 0;
    }

  private:
    u8 const *m_data{nullptr};
    usize m_size{0};
};

class BinaryStream final {
  public:
    constexpr BinaryStream() noexcept = default;

    [[nodiscard]]
    static auto from(void const *data, usize size) -> ustl::Result<BinaryStream, StreamError> {
        if (size != 0 && data == nullptr) {
            return ustl::err(StreamError::NullData);
        }
        return ustl::ok(BinaryStream(static_cast<u8 const *>(data), size));
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return m_size;
    }

    [[nodiscard]]
    auto bytes(usize offset, usize count) const -> ustl::Result<ByteRange, StreamError> {
        if (!contains(offset, count)) {
            return ustl::err(StreamError::OutOfRange);
        }
        u8 const *range_data{nullptr};
        if (count == 0) {
            range_data = m_data;
        } else {
            range_data = m_data + offset;
        }
        return ustl::ok(ByteRange(range_data, count));
    }

    template <typename T>
    [[nodiscard]]
    auto read_le(usize offset) const -> ustl::Result<T, StreamError> {
        static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>,
                      "Binary stream scalars must be unsigned integral types");
        auto const range = bytes(offset, sizeof(T));
        if (range.is_err()) {
            return ustl::err(range.unwrap_err());
        }

        T value{};
        for (usize index = sizeof(T); index != 0; --index) {
            value = static_cast<T>((value << CHAR_BIT) | range->data()[index - 1]);
        }
        return ustl::ok(value);
    }

    template <typename T>
    [[nodiscard]]
    auto read_be(usize offset) const -> ustl::Result<T, StreamError> {
        static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>,
                      "Binary stream scalars must be unsigned integral types");
        auto const range = bytes(offset, sizeof(T));
        if (range.is_err()) {
            return ustl::err(range.unwrap_err());
        }

        T value{};
        for (usize index = 0; index < sizeof(T); ++index) {
            value = static_cast<T>((value << CHAR_BIT) | range->data()[index]);
        }
        return ustl::ok(value);
    }

  private:
    constexpr BinaryStream(u8 const *data, usize size) noexcept : m_data(data), m_size(size) {}

    [[nodiscard]]
    constexpr auto contains(usize offset, usize count) const noexcept -> bool {
        return offset <= m_size && count <= m_size - offset;
    }

    u8 const *m_data{nullptr};
    usize m_size{0};
};
} // namespace eftl

#endif // EFTL_BINARY_STREAM_HPP
