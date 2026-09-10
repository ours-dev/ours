/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <cassert>
#include <cstddef>

#include <ustl/collections/error.hpp>
#include <ustl/collections/memory_resource.hpp>

auto main() -> int {
    auto *null_resource = ustl::collections::null_memory_resource();
    auto null_allocation = null_resource->try_allocate(8, alignof(int));
    assert(null_allocation.is_err());
    assert(null_allocation.error() == ustl::collections::Error::OutOfMemory);

    auto invalid_alignment = null_resource->try_allocate(8, 3);
    assert(invalid_alignment.is_err());
    assert(invalid_alignment.error() == ustl::collections::Error::InvArgs);

    auto *new_delete = ustl::collections::new_delete_resource();
    auto allocation = new_delete->try_allocate(32, alignof(std::max_align_t));
    assert(allocation.is_ok());
    new_delete->deallocate(allocation.value(), 32, alignof(std::max_align_t));
    assert(new_delete->is_equal(*new_delete));
    assert(!new_delete->is_equal(*null_resource));

    auto *old_default = ustl::collections::set_default_resource(null_resource);
    assert(ustl::collections::get_default_resource() == null_resource);
    assert(ustl::collections::set_default_resource(old_default) == null_resource);
}
