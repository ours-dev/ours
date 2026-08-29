/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/option.hpp>

#include <concepts>
#include <type_traits>
#include <utility>

#include <cassert>

static_assert(std::same_as<decltype(ustl::some(std::declval<int &>())), ustl::Option<int &>>);
static_assert(std::same_as<decltype(ustl::some(1)), ustl::Option<int>>);
static_assert(std::is_nothrow_destructible_v<ustl::Option<int>>);
static_assert(std::is_nothrow_destructible_v<ustl::Option<int &>>);

static auto implicit_option() -> ustl::Option<int> {
    return 7;
}

auto main() -> int {
    auto empty = ustl::Option<int>(ustl::NONE);
    assert(empty.is_none());

    auto value = ustl::some(7);
    assert(value.is_some());
    assert(value.value() == 7);
    assert(value.take().value() == 7);
    assert(value.is_none());
    assert(*implicit_option() == 7);

    auto mapped = ustl::some(6).map([](int input) noexcept -> int { return input * 2; });
    assert(mapped.value() == 12);
    assert(ustl::Option<int>(ustl::NONE)
               .map([](int input) noexcept -> int { return input * 2; })
               .is_none());

    int number = 9;
    auto reference = ustl::some(number);
    reference.value() = 11;
    assert(number == 11);
    assert(*reference == 11);
    assert(reference.map([](int input) noexcept -> int { return input + 1; }).value() == 12);
    assert(reference.take().value() == 11);
    assert(reference.is_none());
}
