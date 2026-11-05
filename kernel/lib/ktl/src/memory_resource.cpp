/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ktl/memory_resource.hpp>

#include <heap/new.hpp>
#include <ours/mm/priv/alloc_failure.hpp>

namespace ktl {

auto KernelMemoryResource::do_try_allocate(std::size_t bytes, std::size_t alignment) noexcept
    -> ustl::Result<void *, ustl::collections::Error> {
    if (alignment > MAX_ALIGNMENT) {
        // Refused rather than silently under-aligned: neither heap takes an
        // alignment argument, so there is no way to honour this here.
        return ustl::err(ustl::collections::Error::InvArgs);
    }
    if (bytes == 0) {
        bytes = 1;
    }
#if MM_TEST_ALLOC_FAILURE
    using ours::mm::test::AllocationFailure;
    using ours::mm::test::AllocationSite;
    if (AllocationFailure::hit(AllocationSite::Collection)) {
        return ustl::err(ustl::collections::Error::OutOfMemory);
    }
#endif

    // Through the kernel's `new`, not straight into kmalloc. That operator is
    // the one place the kmalloc/vmalloc choice is made (heap/new.hpp), and
    // calling kmalloc here skipped it: every request above
    // heap::KMALLOC_MAX_SIZE came back OutOfMemory even with memory to spare.
    // gktl's command tree -- one ~27 KiB array -- was the first container to
    // ask for that much, and every kshell command went missing with it.
    auto *pointer = ::operator new(bytes, m_alloc_flags);
    if (pointer == nullptr) {
        return ustl::err(ustl::collections::Error::OutOfMemory);
    }
    return ustl::ok(pointer);
}

/// The kernel's `operator delete` routes on the address, so a block that
/// came from vmalloc goes back to vfree without this having to remember which
/// heap served it. heap::kfree() here would read a vmalloc address through the
/// physmap as if it were a slab page.
auto KernelMemoryResource::do_deallocate(void *pointer, std::size_t, std::size_t) noexcept -> void {
    ::operator delete(pointer);
}

auto kernel_memory_resource() noexcept -> KernelMemoryResource & {
    static KernelMemoryResource RESOURCE{ours::mm::ALLOC_KERNEL};
    return RESOURCE;
}

auto init_memory_resource() -> void {
    auto _ = ustl::collections::set_default_resource(&kernel_memory_resource());
}

} // namespace ktl
