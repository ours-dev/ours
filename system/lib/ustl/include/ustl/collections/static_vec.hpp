/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef USTL_COLLECTIONS_STATIC_VEC_HPP
#define USTL_COLLECTIONS_STATIC_VEC_HPP 1

#include <ustl/collections/error.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>

#include <array>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>

#include <cstddef>

namespace ustl::collections {
template <typename T, std::size_t Capacity>
class StaticVec final {
    static_assert(std::is_object_v<T>, "StaticVec values must be object types.");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "StaticVec values must be nothrow destructible.");
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "StaticVec values must be nothrow move constructible.");

    struct Slot final {
        alignas(T) std::byte m_storage[sizeof(T)];
    };

  public:
    using value_type = T;
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

    constexpr StaticVec() noexcept = default;

    StaticVec(StaticVec const &) = delete;
    auto operator=(StaticVec const &) -> StaticVec & = delete;

    StaticVec(StaticVec &&other) noexcept {
        move_from(std::move(other));
    }

    auto operator=(StaticVec &&other) noexcept -> StaticVec & {
        if (this != std::addressof(other)) {
            clear();
            move_from(std::move(other));
        }
        return *this;
    }

    ~StaticVec() noexcept {
        clear();
    }

    [[nodiscard]]
    constexpr auto empty() const noexcept -> bool {
        return m_size == 0U;
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> size_type {
        return m_size;
    }

    [[nodiscard]]
    static constexpr auto capacity() noexcept -> size_type {
        return Capacity;
    }

    [[nodiscard]]
    static constexpr auto max_size() noexcept -> size_type {
        return Capacity;
    }

    [[nodiscard]]
    auto data() noexcept -> pointer {
        return element_at(0U);
    }

    [[nodiscard]]
    auto data() const noexcept -> const_pointer {
        return element_at(0U);
    }

    [[nodiscard]]
    auto begin() noexcept -> iterator {
        return data();
    }

    [[nodiscard]]
    auto begin() const noexcept -> const_iterator {
        return data();
    }

    [[nodiscard]]
    auto cbegin() const noexcept -> const_iterator {
        return begin();
    }

    [[nodiscard]]
    auto end() noexcept -> iterator {
        return data() + m_size;
    }

    [[nodiscard]]
    auto end() const noexcept -> const_iterator {
        return data() + m_size;
    }

    [[nodiscard]]
    auto cend() const noexcept -> const_iterator {
        return end();
    }

    [[nodiscard]]
    auto rbegin() noexcept -> reverse_iterator {
        return reverse_iterator(end());
    }

    [[nodiscard]]
    auto rbegin() const noexcept -> const_reverse_iterator {
        return const_reverse_iterator(end());
    }

    [[nodiscard]]
    auto rend() noexcept -> reverse_iterator {
        return reverse_iterator(begin());
    }

    [[nodiscard]]
    auto rend() const noexcept -> const_reverse_iterator {
        return const_reverse_iterator(begin());
    }

    [[nodiscard]]
    auto at(size_type index) noexcept -> Option<reference> {
        return index < m_size ? Option<reference>(*element_at(index)) : NONE;
    }

    [[nodiscard]]
    auto at(size_type index) const noexcept -> Option<const_reference> {
        return index < m_size ? Option<const_reference>(*element_at(index)) : NONE;
    }

    [[nodiscard]]
    auto operator[](size_type index) noexcept -> reference {
        return *element_at(index);
    }

    [[nodiscard]]
    auto operator[](size_type index) const noexcept -> const_reference {
        return *element_at(index);
    }

    [[nodiscard]]
    auto front() noexcept -> Option<reference> {
        return empty() ? NONE : Option<reference>(*element_at(0U));
    }

    [[nodiscard]]
    auto front() const noexcept -> Option<const_reference> {
        return empty() ? NONE : Option<const_reference>(*element_at(0U));
    }

    [[nodiscard]]
    auto back() noexcept -> Option<reference> {
        return empty() ? NONE : Option<reference>(*element_at(m_size - 1U));
    }

    [[nodiscard]]
    auto back() const noexcept -> Option<const_reference> {
        return empty() ? NONE : Option<const_reference>(*element_at(m_size - 1U));
    }

    auto push_back(value_type const &value) noexcept -> Result<void, Error>
        requires std::is_nothrow_copy_constructible_v<value_type>
    {
        return emplace_back(value);
    }

    auto push_back(value_type const &) noexcept -> Result<void, Error>
        requires(!std::is_nothrow_copy_constructible_v<value_type>)
    = delete;

    auto push_back(value_type &&value) noexcept -> Result<void, Error> {
        return emplace_back(std::move(value));
    }

    template <typename... Args>
        requires std::is_nothrow_constructible_v<value_type, Args &&...>
    auto emplace_back(Args &&...args) noexcept -> Result<void, Error> {
        if (m_size == Capacity) {
            return err(Error::CapacityOverflow);
        }
        std::construct_at(element_at(m_size), std::forward<Args>(args)...);
        ++m_size;
        return ok();
    }

    [[nodiscard]]
    auto pop_back() noexcept -> Option<value_type> {
        if (empty()) {
            return NONE;
        }

        auto *const value = element_at(m_size - 1U);
        auto result = Option<value_type>(std::move(*value));
        std::destroy_at(value);
        --m_size;
        return result;
    }

    auto clear() noexcept -> void {
        while (!empty()) {
            --m_size;
            std::destroy_at(element_at(m_size));
        }
    }

  private:
    [[nodiscard]]
    auto element_at(size_type index) noexcept -> pointer {
        return std::launder(reinterpret_cast<pointer>(m_slots.data() + index));
    }

    [[nodiscard]]
    auto element_at(size_type index) const noexcept -> const_pointer {
        return std::launder(reinterpret_cast<const_pointer>(m_slots.data() + index));
    }

    auto move_from(StaticVec &&other) noexcept -> void {
        for (auto &value: other) {
            std::construct_at(element_at(m_size), std::move(value));
            ++m_size;
        }
        other.clear();
    }

    std::array<Slot, Capacity> m_slots{};
    size_type m_size{};
};
} // namespace ustl::collections

#endif // USTL_COLLECTIONS_STATIC_VEC_HPP
