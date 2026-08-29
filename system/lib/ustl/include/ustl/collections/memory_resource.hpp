/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_COLLECTIONS_MEMORY_RESOURCE_HPP
#define USTL_COLLECTIONS_MEMORY_RESOURCE_HPP 1

#include <ustl/collections/error.hpp>
#include <ustl/result.hpp>

#include <atomic>
#include <bit>
#include <new>

#include <cstddef>

namespace ustl::collections {
class MemoryResource {
  public:
    virtual ~MemoryResource() noexcept = default;

    [[nodiscard]]
    auto try_allocate(std::size_t bytes, std::size_t alignment) noexcept -> Result<void *, Error> {
        if (!std::has_single_bit(alignment)) {
            return err(Error::InvalidArgument);
        }
        return do_try_allocate(bytes, alignment);
    }

    auto deallocate(void *pointer, std::size_t bytes, std::size_t alignment) noexcept -> void {
        do_deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]]
    auto is_equal(MemoryResource const &other) const noexcept -> bool {
        return do_is_equal(other);
    }

  protected:
    [[nodiscard]]
    virtual auto do_try_allocate(std::size_t bytes, std::size_t alignment) noexcept
        -> Result<void *, Error> = 0;

    virtual auto do_deallocate(void *pointer, std::size_t bytes, std::size_t alignment) noexcept
        -> void = 0;

    [[nodiscard]]
    virtual auto do_is_equal(MemoryResource const &other) const noexcept -> bool {
        return this == std::addressof(other);
    }
};

class NullMemoryResource final: public MemoryResource {
  protected:
    [[nodiscard]]
    auto do_try_allocate(std::size_t, std::size_t) noexcept -> Result<void *, Error> override {
        return err(Error::OutOfMemory);
    }

    auto do_deallocate(void *, std::size_t, std::size_t) noexcept -> void override {}
};

class NewDeleteResource final: public MemoryResource {
  protected:
    [[nodiscard]]
    auto do_try_allocate(std::size_t bytes, std::size_t alignment) noexcept
        -> Result<void *, Error> override {
        auto *const pointer =
            alignment > alignof(std::max_align_t)
                ? ::operator new(bytes, static_cast<std::align_val_t>(alignment), std::nothrow)
                : ::operator new(bytes, std::nothrow);
        if (pointer == nullptr) {
            return err(Error::OutOfMemory);
        }
        return ok(pointer);
    }

    auto do_deallocate(void *pointer, std::size_t, std::size_t alignment) noexcept
        -> void override {
        if (alignment > alignof(std::max_align_t)) {
            ::operator delete(pointer, static_cast<std::align_val_t>(alignment));
        } else {
            ::operator delete(pointer);
        }
    }
};

[[nodiscard]]
inline auto null_memory_resource() noexcept -> MemoryResource * {
    static NullMemoryResource resource;
    return std::addressof(resource);
}

[[nodiscard]]
inline auto new_delete_resource() noexcept -> MemoryResource * {
    static NewDeleteResource resource;
    return std::addressof(resource);
}

namespace detail {
    inline auto default_resource() noexcept -> std::atomic<MemoryResource *> & {
        static std::atomic<MemoryResource *> resource{new_delete_resource()};
        return resource;
    }
} // namespace detail

[[nodiscard]]
inline auto get_default_resource() noexcept -> MemoryResource * {
    return detail::default_resource().load();
}

inline auto set_default_resource(MemoryResource *resource) noexcept -> MemoryResource * {
    if (resource == nullptr) {
        resource = new_delete_resource();
    }
    return detail::default_resource().exchange(resource);
}
} // namespace ustl::collections

#endif // USTL_COLLECTIONS_MEMORY_RESOURCE_HPP
