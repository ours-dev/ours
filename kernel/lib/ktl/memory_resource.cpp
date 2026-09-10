/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <heap/kmalloc.hpp>
#include <ktl/memory_resource.hpp>

namespace ktl {

auto KernelMemoryResource::do_try_allocate(std::size_t bytes, std::size_t alignment) noexcept
    -> ustl::Result<void *, ustl::collections::Error> {
    if (alignment > MAX_ALIGNMENT) {
        // Refused rather than silently under-aligned: kmalloc takes no
        // alignment argument, so there is no way to honour this here.
        return ustl::err(ustl::collections::Error::InvArgs);
    }
    if (bytes == 0) {
        bytes = 1;
    }

    auto *pointer = heap::kmalloc(bytes, m_gaf);
    if (pointer == nullptr) {
        return ustl::err(ustl::collections::Error::OutOfMemory);
    }
    return ustl::ok(pointer);
}

auto KernelMemoryResource::do_deallocate(void *pointer, std::size_t, std::size_t) noexcept -> void {
    heap::kfree(pointer);
}

auto kernel_memory_resource() noexcept -> KernelMemoryResource & {
    static KernelMemoryResource resource{ours::mm::GAF_KERNEL};
    return resource;
}

auto init_memory_resource() -> void {
    static_cast<void>(ustl::collections::set_default_resource(&kernel_memory_resource()));
}

} // namespace ktl
