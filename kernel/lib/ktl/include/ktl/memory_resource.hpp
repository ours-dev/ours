/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef KTL_MEMORY_RESOURCE_HPP
#define KTL_MEMORY_RESOURCE_HPP 1

#include <ours/mm/alloc_flags.hpp>
#include <ustl/collections/memory_resource.hpp>

namespace ktl {

/// A `ustl::collections::MemoryResource` backed by the kernel heap, so the
/// allocator-aware containers in `ustl::collections` (BTree and friends) can
/// be used from kernel code at all: their default resource is the hosted
/// `new`/`delete` one, which does not exist here.
///
/// It allocates through the kernel's `operator new(size, alloc_flags)` and frees
/// through `operator delete`, the same kmalloc-or-vmalloc routing every other
/// kernel allocation gets. A request above heap::KMALLOC_MAX_SIZE therefore
/// inherits vmalloc's contract: it may block, so a container that can grow
/// that large must not do so from an interrupt handler or with a spin lock
/// held.
///
/// Alignment is the one place this is not a drop-in. Neither heap takes an
/// alignment argument and both only promise their natural alignment, so a
/// request for more than that is refused rather than quietly satisfied with
/// a under-aligned block.
class KernelMemoryResource final: public ustl::collections::MemoryResource {
  public:
    /// What `kmalloc` guarantees without being asked; anything stricter has
    /// to go through a different allocator.
    static constexpr std::size_t MAX_ALIGNMENT = alignof(std::max_align_t);

    explicit KernelMemoryResource(ours::mm::AllocFlags alloc_flags) noexcept
        : m_alloc_flags(alloc_flags) {}

  protected:
    [[nodiscard]]
    auto do_try_allocate(std::size_t bytes, std::size_t alignment) noexcept
        -> ustl::Result<void *, ustl::collections::Error> override;

    auto do_deallocate(void *pointer, std::size_t bytes, std::size_t alignment) noexcept
        -> void override;

  private:
    ours::mm::AllocFlags m_alloc_flags;
};

/// The heap-backed resource every kernel container defaults to. Installed as
/// `ustl::collections::set_default_resource` during early init, so a plainly
/// constructed `ustl::collections::Allocator<T>` allocates from the kernel
/// heap without every container having to be handed one.
[[nodiscard]]
auto kernel_memory_resource() noexcept -> KernelMemoryResource &;

/// Installs the above as the process-wide default. Idempotent.
auto init_memory_resource() -> void;

} // namespace ktl

#endif // KTL_MEMORY_RESOURCE_HPP
