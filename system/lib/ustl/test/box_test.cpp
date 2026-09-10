/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
/// Host smoke test for ustl::Box (ustl/box.hpp), the Rust-Box-style
/// unique-ownership smart pointer.
///
/// Covers: unique ownership & move-only semantics, in-place make(), raw
/// handoff (into_raw/from_raw), leak(), deep clone, swap, polymorphic
/// destruction, Disposer-backed storage reclaim, nullability and const
/// access.

#include <type_traits>
#include <utility>

#include <cassert>
#include <cstdio>

#include <ustl/box.hpp>

namespace {

int DESTROYED = 0;

struct Item {
    int value;

    explicit Item(int v) : value(v) {}
    ~Item() {
        ++DESTROYED;
    }
};

static_assert(!std::is_copy_constructible_v<ustl::Box<Item>>);
static_assert(!std::is_copy_assignable_v<ustl::Box<Item>>);
static_assert(std::is_nothrow_move_constructible_v<ustl::Box<Item>>);
static_assert(std::is_nothrow_move_assignable_v<ustl::Box<Item>>);
static_assert(std::is_default_constructible_v<ustl::Box<Item>>);
static_assert(!std::is_copy_constructible_v<ustl::Box<int>>);    // int is copyable, its Box is not
static_assert(std::is_constructible_v<ustl::Box<Item>, Item *>); // explicit adopting ctor
static_assert(!std::is_convertible_v<Item *, ustl::Box<Item>>);  // ...but never implicit

auto test_basic() -> void {
    DESTROYED = 0;
    {
        auto a = ustl::Box<Item>::make(1);
        assert(a->value == 1);         // operator->
        assert((*a).value == 1);       // operator*
        assert(a.as_ref().value == 1); // as_ref
        assert(a);                     // non-empty

        auto b = std::move(a); // move: ownership transfers
        assert(!a);            // NOLINT(bugprone-use-after-move) moved-from box is specified empty
        assert(b && b->value == 1);

        auto c = ustl::Box<Item>::make(2);
        c = std::move(b); // move assignment: old payload (2) destroyed now
        assert(DESTROYED == 1);
        assert(!b && c->value == 1); // NOLINT(bugprone-use-after-move) same contract

        c.reset(); // explicit release
        assert(!c);
        assert(DESTROYED == 2);
    } // ~Box on empty boxes destroys nothing
    assert(DESTROYED == 2);
}

auto test_reset_replaces() -> void {
    DESTROYED = 0;
    auto b = ustl::Box<Item>::make(7);
    b.reset(new Item(8)); // adopt a fresh one, old destroyed
    assert(DESTROYED == 1);
    assert(b->value == 8);
    b.reset(b.as_ptr_mut()); // self-adopt is a no-op
    assert(DESTROYED == 1);
    assert(b && b->value == 8);
}

auto test_nullable() -> void {
    DESTROYED = 0;
    ustl::Box<Item> empty;
    assert(!empty);
    assert(empty == nullptr);
    assert(empty.as_ptr() == nullptr);

    auto b = ustl::Box<Item>::make(1);
    assert(b != nullptr);
    assert(b != empty);

    b = nullptr; // releases ownership
    assert(!b && DESTROYED == 1);

    auto from_null = ustl::Box<Item>::from_raw(nullptr);
    assert(!from_null);
}

// Polymorphic hierarchy: Box<Base> must destroy the most-derived object.
struct Animal {
    virtual ~Animal() = default;
};
struct Dog: Animal {
    ~Dog() override {
        ++DESTROYED;
    }
};

auto test_polymorphic() -> void {
    DESTROYED = 0;
    {
        ustl::Box<Animal> a = ustl::Box<Dog>::make(); // converting move (upcast)
        assert(a);
        ustl::Box<Animal> b = ustl::Box<Animal>::from_raw(new Dog());
        b = std::move(a); // converting move assignment: old payload released now
        assert(DESTROYED == 1);
    }
    assert(DESTROYED == 2); // both ~Dog ran through Animal*
}

auto test_raw_handoff() -> void {
    DESTROYED = 0;
    auto b = ustl::Box<Item>::make(3);
    auto *raw = b.into_raw();
    assert(!b);
    assert(raw->value == 3);
    assert(DESTROYED == 0); // transferred ownership keeps the object alive

    auto adopted = ustl::Box<Item>::from_raw(raw); // re-wrap, sole owner again
    assert(adopted->value == 3);
    assert(DESTROYED == 0);
} // adopted dies here

auto test_adopt() -> void {
    DESTROYED = 0;
    {
        auto b = ustl::Box<Item>::adopt(new Item(11)); // Rc::adopt's counterpart, count-free
        assert(b && b->value == 11);
    }
    assert(DESTROYED == 1); // the box owned and reclaimed the adopted storage
}

auto test_leak() -> void {
    DESTROYED = 0;
    {
        auto b = ustl::Box<Item>::make(9);
        Item &immortal = b.leak();
        assert(!b);
        assert(immortal.value == 9);
    }
    assert(DESTROYED == 0); // leaked on purpose: leak() relinquishes destruction
}

auto test_clone() -> void {
    DESTROYED = 0;
    auto b = ustl::Box<Item>::make(5);
    auto copy = b.clone(); // deep copy
    assert(b && copy && copy->value == 5);
    assert(copy.as_ptr() != b.as_ptr()); // distinct storage
    copy->value = 6;
    assert(b->value == 5); // original untouched

    auto empty = ustl::Box<Item>();
    assert(!empty.clone()); // empty clones to empty
}

auto test_swap() -> void {
    auto a = ustl::Box<Item>::make(1);
    auto b = ustl::Box<Item>::make(2);
    swap(a, b);
    assert(a->value == 2 && b->value == 1);
    a.swap(b);
    assert(a->value == 1 && b->value == 2);
}

// Disposer customization: storage reclaim (object-cache pattern). Destruction
// order must be payload destructor first, Disposer second.
struct Cached {
    static int FREED;
    static int SEQ;
    static int DESTROYED_AT;
    static int FREED_AT;

    struct Disposer {
        auto operator()(Cached *) const noexcept -> void {
            ++Cached::FREED;
            Cached::FREED_AT = ++Cached::SEQ;
        }
    };

    ~Cached() {
        ++DESTROYED;
        Cached::DESTROYED_AT = ++Cached::SEQ;
    }
};
int Cached::FREED = 0;
int Cached::SEQ = 0;
int Cached::DESTROYED_AT = 0;
int Cached::FREED_AT = 0;

auto test_disposer() -> void {
    DESTROYED = 0;
    Cached::FREED = 0;
    Cached::SEQ = 0;
    {
        auto c = ustl::Box<Cached>::from_raw(new Cached());
        assert(c);
    }
    assert(DESTROYED == 1);     // payload destroyed first...
    assert(Cached::FREED == 1); // ...then storage reclaimed by Disposer
    assert(Cached::DESTROYED_AT == 1);
    assert(Cached::FREED_AT == 2);
}

auto test_self_move() -> void {
    DESTROYED = 0;
    auto b = ustl::Box<Item>::make(4);
    auto &alias = b;
    b = std::move(alias); // self-move must be safe (guarded); alias hides it from -Wself-move
    assert(b && b->value == 4);
    assert(DESTROYED == 0);
}

auto test_const_access() -> void {
    auto const b = ustl::Box<Item>::make(1);
    static_assert(std::is_same_v<decltype(b.as_ptr()), Item const *>);
    static_assert(std::is_same_v<decltype(b.operator->()), Item const *>);
    static_assert(std::is_same_v<decltype(*b), Item const &>);
    assert(b->value == 1);
}

} // namespace

auto main() -> int {
    test_basic();
    test_reset_replaces();
    test_nullable();
    test_polymorphic();
    test_raw_handoff();
    test_adopt();
    test_leak();
    test_clone();
    test_swap();
    test_disposer();
    test_self_move();
    test_const_access();
    std::printf("all box tests passed\n");
    return 0;
}
