/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_COLLECTIONS_VEC_HPP
#define USTL_COLLECTIONS_VEC_HPP 1

#include <iterator>
#include <type_traits>
#include <utility>

#include <cstddef>

#include <ustl/collections/error.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>

namespace ustl::collections {
/// A growable array over an allocator, the heap-backed counterpart to
/// StaticVec.
///
/// The allocator contract here is the kernel's, not the standard library's:
/// `A::allocate(n)` hands back `n` *already value-initialized* elements
/// (ktl::Allocator does `new (gaf) T[n]()`), and `A::deallocate(p, n)`
/// destroys them. Vec therefore never placement-news into raw storage the
/// way StaticVec does -- every slot in the buffer is a live object, and
/// growing means assigning into slots that already exist. That is why the
/// element type must be default constructible and move assignable, a
/// stronger requirement than std::vector's.
///
/// The pairing is deliberate: it suits two-phase types such as
/// arch::IoApic, where `emplace_back()` yields a default-constructed slot
/// the caller then finishes with its own `init()`.
template <typename T, typename A>
class Vec final {
    static_assert(std::is_object_v<T>, "Vec values must be object types.");
    static_assert(std::is_default_constructible_v<T>,
                  "Vec values must be default constructible; its allocator value-initializes.");
    static_assert(std::is_nothrow_move_assignable_v<T>,
                  "Vec values must be nothrow move assignable.");

  public:
    using value_type = T;
    using allocator_type = A;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = value_type &;
    using const_reference = value_type const &;
    using pointer = value_type *;
    using const_pointer = value_type const *;
    using iterator = pointer;
    using const_iterator = const_pointer;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    constexpr Vec() noexcept = default;

    Vec(Vec const &) = delete;
    auto operator=(Vec const &) -> Vec & = delete;

    Vec(Vec &&other) noexcept
        : m_data(std::exchange(other.m_data, nullptr))
        , m_size(std::exchange(other.m_size, 0U))
        , m_capacity(std::exchange(other.m_capacity, 0U)) {}

    auto operator=(Vec &&other) noexcept -> Vec & {
        if (this != std::addressof(other)) {
            release();
            m_data = std::exchange(other.m_data, nullptr);
            m_size = std::exchange(other.m_size, 0U);
            m_capacity = std::exchange(other.m_capacity, 0U);
        }
        return *this;
    }

    ~Vec() noexcept {
        release();
    }

    [[nodiscard]]
    auto empty() const noexcept -> bool {
        return m_size == 0U;
    }

    [[nodiscard]]
    auto size() const noexcept -> size_type {
        return m_size;
    }

    [[nodiscard]]
    auto capacity() const noexcept -> size_type {
        return m_capacity;
    }

    [[nodiscard]]
    auto data() noexcept -> pointer {
        return m_data;
    }

    [[nodiscard]]
    auto data() const noexcept -> const_pointer {
        return m_data;
    }

    auto begin() noexcept -> iterator {
        return m_data;
    }
    auto end() noexcept -> iterator {
        return m_data + m_size;
    }
    auto begin() const noexcept -> const_iterator {
        return m_data;
    }
    auto end() const noexcept -> const_iterator {
        return m_data + m_size;
    }

    auto operator[](size_type index) noexcept -> reference {
        return m_data[index];
    }

    auto operator[](size_type index) const noexcept -> const_reference {
        return m_data[index];
    }

    [[nodiscard]]
    auto front() noexcept -> Option<reference> {
        return empty() ? NONE : Option<reference>(m_data[0]);
    }

    [[nodiscard]]
    auto back() noexcept -> Option<reference> {
        return empty() ? NONE : Option<reference>(m_data[m_size - 1U]);
    }

    /// Makes room for at least |n| elements. Existing elements are moved
    /// into the new buffer; on failure the vector is left untouched.
    auto reserve(size_type n) noexcept -> Result<void, Error> {
        if (n <= m_capacity) {
            return ok();
        }

        auto *const buffer = allocator_type::allocate(n);
        if (!buffer) {
            return err(Error::OutOfMemory);
        }

        for (auto i = size_type(0); i < m_size; ++i) {
            buffer[i] = std::move(m_data[i]);
        }

        if (m_data) {
            allocator_type::deallocate(m_data, m_capacity);
        }
        m_data = buffer;
        m_capacity = n;
        return ok();
    }

    /// Appends a default-constructed element and hands back a pointer to
    /// it, for callers that finish initialization in place. A pointer
    /// rather than a reference because ustl::Result rejects reference
    /// value types.
    auto emplace_back() noexcept -> Result<pointer, Error> {
        if (auto grown = grow_if_full(); grown.is_err()) {
            return err(grown.unwrap_err());
        }

        auto *const slot = m_data + m_size;
        m_size += 1U;
        return ok(slot);
    }

    template <typename... Args>
        requires std::is_constructible_v<value_type, Args &&...>
    auto emplace_back(Args &&...args) noexcept -> Result<pointer, Error> {
        if (auto grown = grow_if_full(); grown.is_err()) {
            return err(grown.unwrap_err());
        }

        auto *const slot = m_data + m_size;
        *slot = value_type(std::forward<Args>(args)...);
        m_size += 1U;
        return ok(slot);
    }

    auto push_back(value_type const &value) noexcept -> Result<pointer, Error>
        requires std::is_copy_constructible_v<value_type>
    {
        return emplace_back(value);
    }

    auto push_back(value_type &&value) noexcept -> Result<pointer, Error> {
        return emplace_back(std::move(value));
    }

    /// Drops the last element. Its slot stays constructed -- see the class
    /// comment -- so it is only logically removed.
    auto pop_back() noexcept -> void {
        if (!empty()) {
            m_size -= 1U;
        }
    }

    auto clear() noexcept -> void {
        m_size = 0U;
    }

  private:
    auto grow_if_full() noexcept -> Result<void, Error> {
        if (m_size < m_capacity) {
            return ok();
        }
        return reserve(m_capacity == 0U ? INITIAL_CAPACITY : m_capacity * 2U);
    }

    auto release() noexcept -> void {
        if (m_data) {
            allocator_type::deallocate(m_data, m_capacity);
            m_data = nullptr;
        }
        m_size = 0U;
        m_capacity = 0U;
    }

    constexpr static size_type INITIAL_CAPACITY = 4U;

    pointer m_data = nullptr;
    size_type m_size = 0U;
    size_type m_capacity = 0U;
};

} // namespace ustl::collections

#endif // #ifndef USTL_COLLECTIONS_VEC_HPP
