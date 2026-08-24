/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
/// Host smoke test for the redesigned intrusive Rc/Weak (ustl::rc.hpp).
///
/// Covers: strong/weak counts, weak upgrade/lock races, polymorphic
/// destruction, disposer priority, checked downcast, single-threaded policy
/// and into_raw ownership transfer.

#include <ustl/rc.hpp>

#include <utility>

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

int g_destroyed = 0;

struct Item: ustl::RefCounted<Item> {
    int value;
    explicit Item(int v) : value(v) {}
    ~Item() {
        ++g_destroyed;
    }
};

// Polymorphic hierarchy: base Rc must destroy the most-derived object.
struct Animal: ustl::RefCounted<Animal> {
    virtual ~Animal() = default;
};
struct Dog: Animal {
    ~Dog() override {
        ++g_destroyed;
    }
};

// Disposer customization: nested Disposer takes priority over operator delete.
struct Cached: ustl::RefCounted<Cached> {
    static int g_freed;
    struct Disposer {
        auto operator()(Cached *) const noexcept -> void {
            ++Cached::g_freed;
        }
    };
    ~Cached() {
        ++g_destroyed;
    }
};
int Cached::g_freed = 0;

// Checked downcast opt-in: static tag + virtual override.
struct Vehicle: ustl::RefCounted<Vehicle> {
    static constexpr std::uintptr_t TYPE_TAG = 1;
    [[nodiscard]]
    auto rc_type_tag() const noexcept -> std::uintptr_t override {
        return TYPE_TAG;
    }
    virtual ~Vehicle() = default;
};
struct Car: Vehicle {
    static constexpr std::uintptr_t TYPE_TAG = 2;
    [[nodiscard]]
    auto rc_type_tag() const noexcept -> std::uintptr_t override {
        return TYPE_TAG;
    }
    ~Car() override {
        ++g_destroyed;
    }
};

// Single-threaded (non-atomic) policy: the default `Rc<T>` (Rust's `Rc`).
struct St: ustl::RefCounted<St> {
    ~St() {
        ++g_destroyed;
    }
};

// Thread-safe (atomic) policy: used with `Arc<T>` (Rust's `Arc`).
struct Shared: ustl::RefCounted<Shared, ustl::sync::ThreadSafePolicy> {
    ~Shared() {
        ++g_destroyed;
    }
};

// Exposes the protected deallocate hook for per-instance reclaim (the pattern
// an object-cache-served class uses in its own constructor).
struct Cacheable: ustl::RefCounted<Cacheable> {
    using ustl::RefCounted<Cacheable>::set_deallocate;
    ~Cacheable() {
        ++g_destroyed;
    }
};

auto test_basic() {
    g_destroyed = 0;
    auto a = ustl::Rc<Item>::adopt(new Item(1));
    assert(a->value == 1);
    assert(a.use_count() == 1);
    assert(a.is_unique());
    {
        auto b = a; // copy
        assert(a.use_count() == 2);
        assert(!a.is_unique());
        assert(a.try_get_mut() == nullptr);
        auto c = std::move(b); // move
        assert(a.use_count() == 2);
        assert(c.use_count() == 2);
        assert(c->value == 1);
    }
    assert(a.use_count() == 1);
    a = nullptr;
    assert(!a);
    assert(g_destroyed == 1);
}

auto test_weak() {
    g_destroyed = 0;
    auto a = ustl::Rc<Item>::adopt(new Item(7));
    auto w = a.downgrade();
    assert(!w.expired());
    {
        auto b = w.lock();
        assert(b);
        assert(b.use_count() == 2);
        assert(b->value == 7);
    }
    assert(a.use_count() == 1);
    a = nullptr;
    assert(g_destroyed == 1); // payload gone
    assert(w.expired());
    assert(!w.lock()); // cannot resurrect a destroyed object
    auto w2 = w;       // copy of expired weak
    assert(w2.expired());
    w = nullptr;
    assert(w.expired());
}

auto test_polymorphic() {
    g_destroyed = 0;
    ustl::Rc<Animal> a = ustl::Rc<Dog>::adopt(new Dog());
    assert(a.use_count() == 1);
    a = nullptr;
    assert(g_destroyed == 1); // ~Dog ran through Animal*
}

auto test_disposer() {
    g_destroyed = 0;
    Cached::g_freed = 0;
    {
        auto c = ustl::Rc<Cached>::adopt(new Cached());
        assert(c.use_count() == 1);
    }
    assert(g_destroyed == 1);     // payload destroyed first
    assert(Cached::g_freed == 1); // then storage reclaimed by Disposer
}

auto test_arc() {
    g_destroyed = 0;
    ustl::Arc<Shared> s = ustl::Arc<Shared>::adopt(new Shared());
    auto copy = s; // Arc copies share ownership
    assert(s.use_count() == 2);
    auto w = s.downgrade(); // Weak<Shared, ThreadSafePolicy>
    assert(w.lock());
    s = nullptr;
    copy = nullptr;
    assert(g_destroyed == 1);
    assert(!w.lock());
}

auto test_single_threaded() {
    g_destroyed = 0;
    {
        ustl::Rc<St, ustl::sync::SingleThreadedPolicy> s =
            ustl::Rc<St, ustl::sync::SingleThreadedPolicy>::adopt(new St());
        auto w = s.downgrade();
        assert(!w.expired());
        assert(w.lock());
    }
    assert(g_destroyed == 1);
}

auto test_downcast() {
    g_destroyed = 0;
    ustl::Rc<Vehicle> v = ustl::Rc<Car>::adopt(new Car());
    auto car = v.downcast<Car>();
    assert(car);
    assert(v.use_count() == 2); // downcast adopts (increments)
    auto bad = ustl::Rc<Vehicle>::adopt(new Vehicle());
    assert(!bad.downcast<Car>()); // type mismatch -> empty, no UB

    // Free-function forms (the pattern used by the kernel's VmAreaOrMapping
    // hierarchy): both lvalue and rvalue (move) overloads.
    auto via_lvalue = ustl::downcast<Car>(v);
    assert(via_lvalue);
    auto via_rvalue = ustl::downcast<Car>(ustl::Rc<Vehicle>(v));
    assert(via_rvalue);

    // Compile-time safe upcast helper.
    ustl::Rc<Car> car2 = ustl::Rc<Car>::adopt(new Car());
    ustl::Rc<Vehicle> up = ustl::upcast<Vehicle>(std::move(car2));
    assert(up);
}

auto test_into_raw() {
    g_destroyed = 0;
    auto a = ustl::Rc<Item>::adopt(new Item(3));
    auto *raw = a.into_raw();
    assert(!a);
    assert(raw->value == 3);
    assert(g_destroyed == 0);            // transferred ref keeps the object alive
    auto b = ustl::Rc<Item>::adopt(raw); // incrementing re-wrap
    assert(b.use_count() == 2);
    b = nullptr;
    assert(g_destroyed == 0); // still held by the raw (leaked) ref
    // The remaining strong reference is intentionally leaked: into_raw()
    // transfers ownership out of the counting machinery.
}

auto test_custom_deallocate() {
    g_destroyed = 0;
    Cached::g_freed = 0;
    auto *raw = new Cacheable();
    // Simulate an object-cache-like instance: replace the reclaim strategy.
    raw->set_deallocate([](void *) noexcept { ++Cached::g_freed; });
    {
        auto c = ustl::Rc<Cacheable>::adopt(raw);
    }
    assert(g_destroyed == 1);
    assert(Cached::g_freed == 1);
}

} // namespace

auto main() -> int {
    test_basic();
    test_arc();
    test_weak();
    test_polymorphic();
    test_disposer();
    test_single_threaded();
    test_downcast();
    test_into_raw();
    test_custom_deallocate();
    std::printf("all rc tests passed\n");
    return 0;
}
