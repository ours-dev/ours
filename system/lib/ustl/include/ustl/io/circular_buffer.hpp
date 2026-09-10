/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_IO_CIRCULAR_BUFFER_HPP
#define USTL_IO_CIRCULAR_BUFFER_HPP 1

#include <type_traits>
#include <utility>

#include <cstddef>

#include <ustl/collections/error.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>

namespace ustl::io {
/// A fixed-capacity FIFO ring: once full, pushing overwrites the oldest
/// element rather than failing or growing. That is what makes it right for
/// a device that must never block its producer -- an IRQ handler feeding
/// scancodes drops the stalest input instead of the newest.
///
/// Default construction allocates nothing, so an instance can live as a
/// static object built before the heap exists; call `resize()` once the
/// allocator is up to give it capacity. Until then the buffer holds
/// nothing and every push is discarded.
///
/// Like ustl::collections::Vec, this follows the kernel allocator contract
/// rather than the standard one: `A::allocate(n)` returns `n` already
/// value-initialized elements, so slots are live objects assigned into,
/// never raw storage placement-newed over. Hence the element type must be
/// default constructible and move assignable.
template <typename T, typename A>
class CircularBuffer final {
    static_assert(std::is_object_v<T>, "CircularBuffer values must be object types.");
    static_assert(std::is_default_constructible_v<T>,
                  "CircularBuffer values must be default constructible; its allocator "
                  "value-initializes.");
    static_assert(std::is_nothrow_move_assignable_v<T>,
                  "CircularBuffer values must be nothrow move assignable.");

  public:
    using value_type = T;
    using allocator_type = A;
    using size_type = std::size_t;
    using reference = value_type &;
    using const_reference = value_type const &;
    using pointer = value_type *;

    constexpr CircularBuffer() noexcept = default;

    CircularBuffer(CircularBuffer const &) = delete;
    auto operator=(CircularBuffer const &) -> CircularBuffer & = delete;

    CircularBuffer(CircularBuffer &&other) noexcept
        : m_data(std::exchange(other.m_data, nullptr))
        , m_capacity(std::exchange(other.m_capacity, 0U))
        , m_head(std::exchange(other.m_head, 0U))
        , m_size(std::exchange(other.m_size, 0U)) {}

    auto operator=(CircularBuffer &&other) noexcept -> CircularBuffer & {
        if (this != std::addressof(other)) {
            release();
            m_data = std::exchange(other.m_data, nullptr);
            m_capacity = std::exchange(other.m_capacity, 0U);
            m_head = std::exchange(other.m_head, 0U);
            m_size = std::exchange(other.m_size, 0U);
        }
        return *this;
    }

    ~CircularBuffer() noexcept {
        release();
    }

    [[nodiscard]]
    auto empty() const noexcept -> bool {
        return m_size == 0U;
    }

    [[nodiscard]]
    auto full() const noexcept -> bool {
        return m_capacity != 0U && m_size == m_capacity;
    }

    [[nodiscard]]
    auto size() const noexcept -> size_type {
        return m_size;
    }

    [[nodiscard]]
    auto capacity() const noexcept -> size_type {
        return m_capacity;
    }

    /// Gives the buffer a new capacity, discarding whatever it held. The
    /// contents are dropped rather than migrated because a ring that has
    /// wrapped has no meaningful order to preserve across a resize, and
    /// the one caller does this during init while the buffer is empty.
    auto resize(size_type capacity) noexcept -> Result<void, collections::Error> {
        if (capacity == 0U) {
            release();
            return ok();
        }

        auto *const buffer = allocator_type::allocate(capacity);
        if (!buffer) {
            return err(collections::Error::OutOfMemory);
        }

        release();
        m_data = buffer;
        m_capacity = capacity;
        return ok();
    }

    /// Appends |value|, dropping the oldest element if the ring is full.
    /// Returns nothing on purpose: overwriting is the defined behaviour of
    /// a full ring, not a failure. A push into a buffer that has no
    /// capacity yet -- see the class comment -- is silently discarded.
    auto push_back(value_type const &value) noexcept -> void {
        if (m_capacity == 0U) {
            return;
        }

        m_data[(m_head + m_size) % m_capacity] = value;
        if (m_size == m_capacity) {
            // Full: the write above landed on the oldest slot, so the head
            // follows it round.
            m_head = (m_head + 1U) % m_capacity;
        } else {
            m_size += 1U;
        }
    }

    [[nodiscard]]
    auto pop_front() noexcept -> Option<value_type> {
        if (empty()) {
            return NONE;
        }

        auto result = Option<value_type>(std::move(m_data[m_head]));
        m_head = (m_head + 1U) % m_capacity;
        m_size -= 1U;
        return result;
    }

    [[nodiscard]]
    auto front() noexcept -> Option<reference> {
        return empty() ? NONE : Option<reference>(m_data[m_head]);
    }

    [[nodiscard]]
    auto back() noexcept -> Option<reference> {
        return empty() ? NONE : Option<reference>(m_data[(m_head + m_size - 1U) % m_capacity]);
    }

    auto clear() noexcept -> void {
        m_head = 0U;
        m_size = 0U;
    }

  private:
    auto release() noexcept -> void {
        if (m_data) {
            allocator_type::deallocate(m_data, m_capacity);
            m_data = nullptr;
        }
        m_capacity = 0U;
        m_head = 0U;
        m_size = 0U;
    }

    pointer m_data = nullptr;
    size_type m_capacity = 0U;
    /// Index of the oldest element; the ring runs forward from here.
    size_type m_head = 0U;
    size_type m_size = 0U;
};

} // namespace ustl::io

#endif // #ifndef USTL_IO_CIRCULAR_BUFFER_HPP
