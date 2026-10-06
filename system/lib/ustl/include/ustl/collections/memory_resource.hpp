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
            return err(Error::InvArgs);
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

/// Freestanding targets (the kernel and phys images compile with
/// -ffreestanding, i.e. `__STDC_HOSTED__ == 0`) have no hosted
/// `::operator new`/`::operator delete` to back this class, so it exists
/// only in hosted builds. A freestanding environment installs its own
/// resource instead (the kernel does so in `ktl::init_memory_resource()`);
/// until then the default is the null resource below, so an allocation
/// attempted too early fails cleanly with OutOfMemory rather than reaching
/// for a global new/delete that does not exist.
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
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
#endif // hosted

[[nodiscard]]
inline auto null_memory_resource() noexcept -> MemoryResource * {
    static NullMemoryResource RESOURCE;
    return std::addressof(RESOURCE);
}

#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
[[nodiscard]]
inline auto new_delete_resource() noexcept -> MemoryResource * {
    static NewDeleteResource RESOURCE;
    return std::addressof(RESOURCE);
}
#endif

namespace detail {
    inline auto default_resource() noexcept -> std::atomic<MemoryResource *> & {
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
        static std::atomic<MemoryResource *> RESOURCE{new_delete_resource()};
#else
        static std::atomic<MemoryResource *> RESOURCE{null_memory_resource()};
#endif
        return RESOURCE;
    }
} // namespace detail

[[nodiscard]]
inline auto get_default_resource() noexcept -> MemoryResource * {
    return detail::default_resource().load();
}

inline auto set_default_resource(MemoryResource *resource) noexcept -> MemoryResource * {
    if (resource == nullptr) {
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
        resource = new_delete_resource();
#else
        resource = null_memory_resource();
#endif
    }
    return detail::default_resource().exchange(resource);
}
} // namespace ustl::collections

#endif // USTL_COLLECTIONS_MEMORY_RESOURCE_HPP
