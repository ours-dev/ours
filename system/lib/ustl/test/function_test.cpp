/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
/// Host test for ustl::function::Function (ustl/function/function.hpp), the
/// non-allocating type-erased callable with a 24-byte inline target.
///
/// Covers: the size and alignment limit (a target one byte too large, or too
/// strictly aligned, does not construct), calling through function pointers
/// and capturing lambdas, return-type conversion, mutable targets called
/// through a const Function, move-only semantics, and that every target
/// constructed is destroyed exactly once across moves, reassignment and reset.

#include <ustl/function/function.hpp>

#include <array>
#include <type_traits>
#include <utility>

#include <cassert>
#include <cstddef>

namespace {
using ustl::function::Function;

// A lambda's closure is its captures, laid out like a struct; these stand in
// for closures of an exact size.
template <std::size_t N>
struct Bytes {
    std::array<std::byte, N> bytes{};
    auto operator()() const -> int {
        return static_cast<int>(N);
    }
};

struct alignas(16) OverAligned {
    auto operator()() const -> int {
        return 0;
    }
};

struct MoveOnly {
    MoveOnly() = default;
    MoveOnly(MoveOnly &&) noexcept = default;
    MoveOnly(MoveOnly const &) = delete;
    auto operator()() const -> int {
        return 1;
    }
};

// Pointers are 8 bytes on every host this test runs on, so the limit really is
// three of them.
static_assert(Function<void()>::INLINE_SIZE == 24);
static_assert(sizeof(Function<void()>) == 32);

// The limit is exact: 24 bytes constructs, 25 does not, and neither does a
// target asking for more alignment than a pointer's.
static_assert(std::is_constructible_v<Function<int()>, Bytes<24>>);
static_assert(!std::is_constructible_v<Function<int()>, Bytes<25>>);
static_assert(!std::is_constructible_v<Function<int()>, OverAligned>);

// A lambda capturing three pointers fits; one capturing four does not.
constexpr auto THREE = [p = nullptr, q = nullptr, r = nullptr] { return p == q && q == r; };
constexpr auto FOUR = [p = nullptr, q = nullptr, r = nullptr, s = nullptr] {
    return p == q && q == r && r == s;
};
static_assert(std::is_constructible_v<Function<bool()>, decltype(THREE)>);
static_assert(!std::is_constructible_v<Function<bool()>, decltype(FOUR)>);

// Only callables matching the signature construct.
static_assert(!std::is_constructible_v<Function<int()>, int>);
static_assert(!std::is_constructible_v<Function<int(int)>, Bytes<8>>);

// Move-only, whatever it holds; and a move-only target is accepted.
static_assert(!std::is_copy_constructible_v<Function<int()>>);
static_assert(!std::is_copy_assignable_v<Function<int()>>);
static_assert(std::is_nothrow_move_constructible_v<Function<int()>>);
static_assert(std::is_nothrow_move_assignable_v<Function<int()>>);
static_assert(std::is_constructible_v<Function<int()>, MoveOnly>);

// Counts the live copies of a target, to show that moves relocate rather than
// leak or double-destroy.
struct Tracked {
    int *live;
    explicit Tracked(int *live) : live(live) {
        ++*live;
    }
    Tracked(Tracked &&other) noexcept : live(other.live) {
        ++*live;
    }
    Tracked(Tracked const &) = delete;
    auto operator=(Tracked &&) -> Tracked & = delete;
    auto operator=(Tracked const &) -> Tracked & = delete;
    ~Tracked() {
        --*live;
    }
    auto operator()() const -> int {
        return *live;
    }
};

auto add(int const lhs, int const rhs) -> int {
    return lhs + rhs;
}

auto test_empty() -> void {
    Function<void()> const empty;
    assert(!empty);
    assert(empty == nullptr);

    Function<void()> const null{nullptr};
    assert(!null);

    // A null function pointer gives an empty Function, not one that calls 0.
    auto (*const no_function)(int, int)->int = nullptr;
    Function<int(int, int)> const from_null{no_function};
    assert(!from_null);
}

auto test_call() -> void {
    Function<int(int, int)> const from_pointer{&add};
    assert(from_pointer);
    assert(from_pointer(2, 3) == 5);

    int base = 10;
    int *const at = &base;
    Function<long(int)> const capturing{
        [at, scale = 3L](int const x) { return (*at + x) * scale; }};
    assert(capturing(1) == 33);
    base = 20;
    assert(capturing(1) == 63);

    // The result converts to R, and a void R discards it.
    Function<long()> const widened{[] { return 7; }};
    assert(widened() == 7L);
    Function<void()> const discarded{[] { return 7; }};
    discarded();

    // Arguments are forwarded, so a move-only argument passes through.
    Function<int(MoveOnly)> const sink{[](MoveOnly const &value) { return value(); }};
    assert(sink(MoveOnly{}) == 1);
}

auto test_mutable_through_const() -> void {
    Function<int()> counter{[n = 0]() mutable { return ++n; }};
    Function<int()> const &view = counter;
    assert(view() == 1);
    assert(view() == 2);
    assert(counter() == 3);
}

auto test_move() -> void {
    Function<int()> source{[n = 41]() { return n + 1; }};
    Function<int()> moved{std::move(source)};
    assert(!source); // NOLINT(bugprone-use-after-move): moved-from is specified empty.
    assert(moved);
    assert(moved() == 42);

    Function<int()> assigned;
    assigned = std::move(moved);
    assert(!moved); // NOLINT(bugprone-use-after-move): moved-from is specified empty.
    assert(assigned() == 42);

    // Self-move leaves the target where it was.
    auto &alias = assigned;
    assigned = std::move(alias);
    assert(assigned() == 42);
}

auto test_lifetime() -> void {
    int live = 0;
    {
        Function<int()> first{Tracked{&live}};
        assert(live == 1);
        assert(first() == 1);

        Function<int()> second{std::move(first)};
        assert(live == 1);
        assert(second() == 1);

        // Reassigning destroys the old target before taking the new one.
        Function<int()> third{Tracked{&live}};
        assert(live == 2);
        third = std::move(second);
        assert(live == 1);

        third = nullptr;
        assert(live == 0);
        assert(!third);

        third = Tracked{&live};
        assert(live == 1);
    }
    assert(live == 0);
}
} // namespace

auto main() -> int {
    test_empty();
    test_call();
    test_mutable_through_const();
    test_move();
    test_lifetime();
    return 0;
}
