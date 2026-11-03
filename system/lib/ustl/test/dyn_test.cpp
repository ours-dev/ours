/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
/// Host test for ustl::Dyn, Method and Ref (ustl/dyn.hpp), dynamic dispatch
/// without a base class.
///
/// Covers: calls reaching two unrelated types through one interface and
/// changing the objects themselves; a reference as a result; an optional method
/// answered by the object's own member and by the default; what models an
/// interface and what does not; that a reference is two words and trivially
/// copyable; the empty reference; and rebinding a reference to another object.

#include <ustl/dyn.hpp>

#include <type_traits>

#include <cassert>
#include <cstring>

// A dispatch that no interface in this file lists.
namespace elsewhere {
USTL_DYN_MEMBER(Unused, unused);
} // namespace elsewhere

namespace {
USTL_DYN_MEMBER(Bump, bump);
USTL_DYN_MEMBER(Name, name);
USTL_DYN_MEMBER(Peek, peek);
USTL_DYN_MEMBER_OR(Reset, reset, ustl::dyn::Ignore{});
USTL_DYN_MEMBER_OR(Limit, limit, ustl::dyn::Returns<-1>{});

using Counting =
    ustl::Dyn<ustl::Method<Bump, auto(int)->int>, ustl::Method<Name, auto()->char const *>,
              ustl::Method<Peek, auto()->int &>, ustl::Method<Reset, auto()->void>,
              ustl::Method<Limit, auto()->int>>;

// Two types that know nothing of each other or of the interface.
struct Counter {
    int count = 0;
    int resets = 0;

    auto bump(int by) -> int {
        count += by;
        return count;
    }
    auto name() const -> char const * {
        return "counter";
    }
    auto peek() -> int & {
        return count;
    }
    auto reset() -> void {
        count = 0;
        resets += 1;
    }
    auto limit() const -> int {
        return 10;
    }
};

// Has the required methods and none of the optional ones.
struct Bare {
    int count = 100;

    auto bump(int by) -> int {
        count += by;
        return count;
    }
    auto name() const -> char const * {
        return "bare";
    }
    auto peek() -> int & {
        return count;
    }
};

// Lacks bump and peek.
struct Lacking {
    auto name() const -> char const * {
        return "lacking";
    }
};

template <typename Dispatch>
concept Callable = requires(ustl::Ref<Counting> ref) { ref.template call<Dispatch>(); };

static_assert(Counting::MODELED_BY<Counter>);
static_assert(Counting::MODELED_BY<Bare>); // the optional methods are not required
static_assert(!Counting::MODELED_BY<Lacking>);
static_assert(!Counting::MODELED_BY<Counter const>);

static_assert(std::is_constructible_v<ustl::Ref<Counting>, Counter &>);
static_assert(!std::is_constructible_v<ustl::Ref<Counting>, Lacking &>);
static_assert(!std::is_constructible_v<ustl::Ref<Counting>, Counter const &>);

static_assert(std::is_trivially_copyable_v<ustl::Ref<Counting>>);
static_assert(sizeof(ustl::Ref<Counting>) == 2 * sizeof(void *));

static_assert(Callable<Name>);               // a method of the interface, called with its arguments
static_assert(!Callable<Bump>);              // a method of the interface, called without them
static_assert(!Callable<elsewhere::Unused>); // not a method of the interface at all

auto test_dispatch() -> void {
    Counter counter;
    Bare bare;
    ustl::Ref<Counting> const a = counter;
    ustl::Ref<Counting> const b = bare;

    assert(a.call<Bump>(5) == 5);
    assert(counter.count == 5); // the object itself changed, not a copy
    assert(b.call<Bump>(7) == 107);
    assert(bare.count == 107);
    assert(counter.count == 5); // and each reference reached its own object

    assert(std::strcmp(a.call<Name>(), "counter") == 0);
    assert(std::strcmp(b.call<Name>(), "bare") == 0);
}

auto test_result_is_a_reference() -> void {
    Counter counter;
    ustl::Ref<Counting> const ref = counter;

    static_assert(std::is_same_v<decltype(ref.call<Peek>()), int &>);
    ref.call<Peek>() = 42;
    assert(counter.count == 42);
}

auto test_optional_methods() -> void {
    Counter counter;
    Bare bare;
    ustl::Ref<Counting> const a = counter;
    ustl::Ref<Counting> const b = bare;

    // The object's own member where it has one...
    a.call<Bump>(3);
    a.call<Reset>();
    assert(counter.count == 0);
    assert(counter.resets == 1);
    assert(a.call<Limit>() == 10);

    // ...and the default where it has not, written once with the method.
    b.call<Reset>();
    assert(bare.count == 100);
    assert(b.call<Limit>() == -1);
}

auto test_empty_and_rebinding() -> void {
    Counter counter;
    Bare bare;

    ustl::Ref<Counting> ref;
    assert(!ref);

    ref = counter;
    assert(ref);
    assert(ref.call<Bump>(1) == 1);

    // The same reference, pointed at another type: the table goes with it.
    ref = bare;
    assert(ref.call<Bump>(1) == 101);
    assert(counter.count == 1);

    ustl::Ref<Counting> const copy = ref;
    assert(copy.call<Bump>(1) == 102);
    assert(bare.count == 102);
}
} // namespace

auto main() -> int {
    test_dispatch();
    test_result_is_a_reference();
    test_optional_methods();
    test_empty_and_rebinding();
    return 0;
}
