/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/collections/allocator.hpp>
#include <ustl/collections/error.hpp>
#include <ustl/collections/memory_resource.hpp>
#include <ustl/option.hpp>

#include <concepts>
#include <type_traits>

static_assert(std::is_nothrow_destructible_v<ustl::Option<int>>);
static_assert(std::is_nothrow_destructible_v<ustl::Option<int &>>);
static_assert(std::is_nothrow_destructible_v<ustl::collections::Allocator<int>>);
static_assert(std::same_as<ustl::collections::Allocator<int>::Rebind<long>,
                           ustl::collections::Allocator<long>>);
static_assert(std::same_as<ustl::collections::PolymorphicAllocator<int>::Rebind<long>,
                           ustl::collections::PolymorphicAllocator<long>>);

auto main() -> int {
    return 0;
}
