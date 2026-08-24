/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <cassert>
#include <cstddef>
#include <utility>

#include <ustl/collections/allocator.hpp>
#include <ustl/collections/btree.hpp>
#include <ustl/collections/error.hpp>
#include <ustl/collections/memory_resource.hpp>

namespace {
    class FailingResource final : public ustl::collections::MemoryResource {
      public:
        explicit FailingResource(std::size_t allocations_before_failure) noexcept
            : m_allocations_before_failure(allocations_before_failure) {}

        auto fail_after(std::size_t allocations_before_failure) noexcept -> void {
            m_allocations_before_failure = allocations_before_failure;
        }

      protected:
        auto do_try_allocate(std::size_t bytes, std::size_t alignment) noexcept
            -> ustl::Result<void *, ustl::collections::Error> override {
            if (m_allocations_before_failure == 0U) {
                return ustl::err(ustl::collections::Error::OutOfMemory);
            }
            --m_allocations_before_failure;
            return ustl::collections::new_delete_resource()->try_allocate(bytes, alignment);
        }

        auto do_deallocate(void *pointer, std::size_t bytes, std::size_t alignment) noexcept -> void override {
            ustl::collections::new_delete_resource()->deallocate(pointer, bytes, alignment);
        }

      private:
        std::size_t m_allocations_before_failure;
    };
} // namespace

auto main() -> int {
    FailingResource resource {0U};
    ustl::collections::BTreeSet<int> empty_tree {ustl::collections::Allocator<int> {resource}};

    const auto empty_insert = empty_tree.insert(1);
    assert(empty_insert.is_err());
    assert(empty_insert.error() == ustl::collections::Error::OutOfMemory);
    assert(empty_tree.empty());
    assert(empty_tree.find(1).is_none());

    resource.fail_after(1U);
    ustl::collections::BTreeSet<int> tree {ustl::collections::Allocator<int> {resource}};
    for (auto value = 1; value <= 15; ++value) {
        const auto inserted = tree.insert(value);
        assert(inserted.is_ok());
    }

    resource.fail_after(1U);
    const auto split_insert = tree.insert(16);
    assert(split_insert.is_err());
    assert(split_insert.error() == ustl::collections::Error::OutOfMemory);
    assert(tree.size() == 15U);
    assert(tree.find(16).is_none());

    auto expected = 1;
    for (const auto value : tree) {
        assert(value == expected);
        ++expected;
    }
    assert(expected == 16);
}
