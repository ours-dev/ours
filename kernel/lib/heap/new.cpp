/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <heap/new.hpp>
#include <heap/vmalloc.hpp>

namespace {
/// Where a request of |size| bytes comes from.
///
/// Linux counterpart: kvmalloc(). It tries the slab first and falls back to
/// vmalloc; this decides up front instead, because kmalloc here answers
/// nullptr above its ladder rather than failing for a reason worth retrying,
/// so "try and fall back" and "choose by size" are the same thing minus a
/// wasted call.
///
/// The node hint reaches kmalloc only. See the note in new.hpp.
[[nodiscard]]
auto allocate(usize const size, ours::mm::Gaf const gaf) noexcept -> void * {
    if (size > heap::KMALLOC_MAX_SIZE) {
        return heap::vmalloc(size, gaf);
    }
    return heap::kmalloc(size, gaf);
}

[[nodiscard]]
auto allocate(usize const size, ours::mm::Gaf const gaf, ours::NnId const nid) noexcept -> void * {
    if (size > heap::KMALLOC_MAX_SIZE) {
        return heap::vmalloc(size, gaf);
    }
    return heap::kmalloc(size, gaf, nid);
}

/// The counterpart of allocate(), and the reason the vmalloc window is a
/// fixed range: this runs on every deallocation the kernel performs.
///
/// Order matters. heap::kfree() resolves its pointer to an owning cache
/// through the physmap, and in a KERNEL_DEBUG_LEVEL=0 image a non-physmap
/// pointer does not trip an assert there -- PhysMap::virt_to_phys() answers
/// zero, phys_to_frame(0) is frame 0, and its body is read as a MemCache
/// pointer. So the window test has to come first, always.
///
/// Linux counterpart: kvfree(), which routes on is_vmalloc_addr() for the
/// same reason.
auto deallocate(void *const pointer) noexcept -> void {
    if (pointer == nullptr) {
        return;
    }
    if (heap::is_vmalloc_addr(pointer)) {
        return heap::vfree(pointer);
    }
    return heap::kfree(pointer);
}
} // namespace

/// The property the whole nullptr-on-failure convention rests on.
///
/// [expr.new] only lets a new-expression skip the constructor and evaluate to
/// null when the allocation function it selected has a non-throwing exception
/// specification. Drop the noexcept and every `if (!p)` in the kernel becomes
/// unreachable code guarded by a constructor that already ran on address zero.
/// It cannot be observed from the boot self-check without arranging an
/// allocation failure for a type with a constructor, so it is pinned here.
static_assert(noexcept(::operator new(usize{}, ours::mm::Gaf{})),
              "the kernel's placement new must be non-throwing");
static_assert(noexcept(::operator new[](usize{}, ours::mm::Gaf{})),
              "the kernel's placement new[] must be non-throwing");

auto operator new(usize const size, ours::mm::Gaf const gaf) noexcept -> void * {
    return allocate(size, gaf);
}

auto operator new[](usize const size, ours::mm::Gaf const gaf) noexcept -> void * {
    return allocate(size, gaf);
}

auto operator new(usize const size, ours::mm::Gaf const gaf, ours::NnId const nid) noexcept
    -> void * {
    return allocate(size, gaf, nid);
}

auto operator new[](usize const size, ours::mm::Gaf const gaf, ours::NnId const nid) noexcept
    -> void * {
    return allocate(size, gaf, nid);
}

auto operator delete(void *const ptr) noexcept -> void {
    return deallocate(ptr);
}

auto operator delete[](void *const ptr) noexcept -> void {
    return deallocate(ptr);
}

/// The sized forms, which exist because the compiler emits calls to them.
///
/// Clang predefines __cpp_sized_deallocation at -std=c++2c and this build does
/// not pass -fno-sized-deallocation, so every deleting destructor in the image
/// references operator delete(void*, size_t). Before these definitions those
/// calls bound to the weak no-op in //kernel/lib/libc/cpp_dependencies.cpp,
/// which is to say they leaked in silence. Nothing executed one yet --
/// ustl::RefCounted and ustl::Box both reclaim through the unsized overload --
/// but "nothing yet" is not a property anyone maintains.
///
/// The size is ignored rather than used to route: it describes the object, not
/// the allocation, and neither allocator here needs it back. Forwarding keeps
/// one routing decision rather than two that can disagree.
auto operator delete(void *const ptr, usize) noexcept -> void {
    return deallocate(ptr);
}

auto operator delete[](void *const ptr, usize) noexcept -> void {
    return deallocate(ptr);
}
