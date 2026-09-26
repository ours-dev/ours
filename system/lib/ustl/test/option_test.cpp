/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ustl/option.hpp>

#include <concepts>
#include <type_traits>
#include <utility>

#include <cassert>

// `some` is owning regardless of the argument's value category; only
// `some_ref` aliases. Passing a named variable must not silently hand back
// a reference into it — that turned a natural `return some(local);` into a
// dangling Option whenever the return type was deduced.
static_assert(std::same_as<decltype(ustl::some(std::declval<int &>())), ustl::Option<int>>);
static_assert(std::same_as<decltype(ustl::some(1)), ustl::Option<int>>);
static_assert(std::same_as<decltype(ustl::some_ref(std::declval<int &>())), ustl::Option<int &>>);
static_assert(
    std::same_as<decltype(ustl::some_ref(std::declval<int const &>())), ustl::Option<int const &>>);
static_assert(std::is_nothrow_destructible_v<ustl::Option<int>>);
static_assert(std::is_nothrow_destructible_v<ustl::Option<int &>>);

// `Option<T &>` converts exactly where the `T *` inside it does: const may
// be added implicitly and never taken away, a derived referent may be seen
// as its base and never the reverse, and an unrelated type never converts.
namespace {
struct Base {};
struct Derived final: Base {};
} // namespace

static_assert(std::is_nothrow_convertible_v<ustl::Option<int &>, ustl::Option<int const &>>);
static_assert(!std::is_constructible_v<ustl::Option<int &>, ustl::Option<int const &>>);
static_assert(std::is_nothrow_convertible_v<ustl::Option<Derived &>, ustl::Option<Base &>>);
static_assert(!std::is_constructible_v<ustl::Option<Derived &>, ustl::Option<Base &>>);
static_assert(!std::is_constructible_v<ustl::Option<long const &>, ustl::Option<int &>>);

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

    // Owning: writing through the option must NOT reach back into `number`.
    int number = 9;
    auto copied = ustl::some(number);
    copied.value() = 11;
    assert(number == 9);
    assert(*copied == 11);

    // Aliasing is opt-in, and does write through.
    auto reference = ustl::some_ref(number);
    reference.value() = 11;
    assert(number == 11);
    assert(*reference == 11);
    assert(reference.map([](int input) noexcept -> int { return input + 1; }).value() == 12);

    // Widening copies the view, not the object: the result still aliases
    // `number`, assignment goes through the same conversion, and an empty
    // option stays empty.
    ustl::Option<int const &> const widened = reference;
    assert(&widened.value() == &number);
    ustl::Option<int const &> assigned = ustl::NONE;
    assigned = reference;
    assert(&assigned.value() == &number);
    ustl::Option<int const &> const widened_none = ustl::Option<int &>(ustl::NONE);
    assert(widened_none.is_none());

    assert(reference.take().value() == 11);
    assert(reference.is_none());
}
