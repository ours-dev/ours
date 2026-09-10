/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_ALLOCATOR_HPP
#define USTL_COLLECTIONS_ALLOCATOR_HPP 1

#include <limits>
#include <type_traits>

#include <cstddef>

#include <ustl/collections/error.hpp>
#include <ustl/collections/memory_resource.hpp>
#include <ustl/result.hpp>

namespace ustl::collections {
namespace detail {
    template <typename T>
    [[nodiscard]]
    constexpr auto max_allocation_count() noexcept -> std::size_t {
        return std::numeric_limits<std::size_t>::max() / sizeof(T);
    }

    template <typename T>
    [[nodiscard]]
    auto try_allocate(MemoryResource &resource, std::size_t count) noexcept -> Result<T *, Error> {
        if (count > max_allocation_count<T>()) {
            return err(Error::CapacityOverflow);
        }

        auto allocation = resource.try_allocate(count * sizeof(T), alignof(T));
        if (allocation.is_err()) {
            return err(allocation.error());
        }
        return ok(static_cast<T *>(allocation.value()));
    }
} // namespace detail

template <typename T>
class [[nodiscard]] Allocator final {
    static_assert(std::is_object_v<T>, "Allocator value type must be an object type.");

  public:
    using Value = T;
    template <typename U>
    using Rebind = Allocator<U>;

    constexpr Allocator() noexcept : m_resource(get_default_resource()) {}

    constexpr explicit Allocator(MemoryResource &resource) noexcept
        : m_resource(std::addressof(resource)) {}

    [[nodiscard]]
    constexpr auto resource() const noexcept -> MemoryResource * {
        return m_resource;
    }

    [[nodiscard]]
    static constexpr auto max_size() noexcept -> std::size_t {
        return detail::max_allocation_count<T>();
    }

    [[nodiscard]]
    auto try_allocate(std::size_t count) noexcept -> Result<T *, Error> {
        return detail::try_allocate<T>(*m_resource, count);
    }

    auto deallocate(T *pointer, std::size_t count) noexcept -> void {
        if (pointer != nullptr) {
            m_resource->deallocate(pointer, count * sizeof(T), alignof(T));
        }
    }

  private:
    MemoryResource *m_resource;
};

template <typename T>
class [[nodiscard]] PolymorphicAllocator final {
    static_assert(std::is_object_v<T>, "PolymorphicAllocator value type must be an object type.");

  public:
    using Value = T;
    template <typename U>
    using Rebind = PolymorphicAllocator<U>;

    constexpr PolymorphicAllocator() noexcept : m_resource(get_default_resource()) {}

    constexpr explicit PolymorphicAllocator(MemoryResource &resource) noexcept
        : m_resource(std::addressof(resource)) {}

    [[nodiscard]]
    constexpr auto resource() const noexcept -> MemoryResource * {
        return m_resource;
    }

    [[nodiscard]]
    static constexpr auto max_size() noexcept -> std::size_t {
        return detail::max_allocation_count<T>();
    }

    [[nodiscard]]
    auto try_allocate(std::size_t count) noexcept -> Result<T *, Error> {
        return detail::try_allocate<T>(*m_resource, count);
    }

    auto deallocate(T *pointer, std::size_t count) noexcept -> void {
        if (pointer != nullptr) {
            m_resource->deallocate(pointer, count * sizeof(T), alignof(T));
        }
    }

  private:
    MemoryResource *m_resource;
};
} // namespace ustl::collections

#endif // USTL_COLLECTIONS_ALLOCATOR_HPP
