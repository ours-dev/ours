/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <cassert>
#include <cstddef>

#include <ustl/collections/allocator.hpp>
#include <ustl/collections/error.hpp>
#include <ustl/collections/memory_resource.hpp>
#include <ustl/option.hpp>

namespace {
    class FailingResource final : public ustl::collections::MemoryResource {
      private:
        auto do_try_allocate(std::size_t, std::size_t) noexcept
            -> ustl::Result<void *, ustl::collections::Error> override {
            return ustl::err(ustl::collections::Error::OutOfMemory);
        }

        auto do_deallocate(void *, std::size_t, std::size_t) noexcept -> void override {}
    };
} // namespace

auto main() -> int {
    auto empty = ustl::Option<int>(ustl::NONE);
    assert(empty.is_none());

    auto value = ustl::some(7);
    assert(value.is_some());
    assert(value.value() == 7);
    assert(value.take().value() == 7);
    assert(value.is_none());

    int number = 9;
    auto reference = ustl::some(number);
    reference.value() = 11;
    assert(number == 11);

    FailingResource failing_resource;
    ustl::collections::Allocator<int> failing_allocator(failing_resource);
    auto failed = failing_allocator.try_allocate(1);
    assert(failed.is_err());
    assert(failed.error() == ustl::collections::Error::OutOfMemory);

    ustl::collections::PolymorphicAllocator<int> polymorphic_allocator(failing_resource);
    auto polymorphic_failed = polymorphic_allocator.try_allocate(1);
    assert(polymorphic_failed.is_err());
    assert(polymorphic_failed.error() == ustl::collections::Error::OutOfMemory);

    auto default_allocator = ustl::collections::Allocator<int>();
    auto allocated = default_allocator.try_allocate(2);
    assert(allocated.is_ok());
    default_allocator.deallocate(allocated.value(), 2);

    auto overflow = default_allocator.try_allocate(static_cast<std::size_t>(-1));
    assert(overflow.is_err());
    assert(overflow.error() == ustl::collections::Error::CapacityOverflow);
}
