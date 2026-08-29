/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
/// Compiles-and-runs the code examples from docs/box.md to keep the tutorial honest.

#include <ustl/box.hpp>

#include <utility>

#include <cassert>
#include <cstdio>

namespace {

int g_destroyed = 0;
int g_cache_freed = 0;

// ==== 快速开始（§2）====
struct Message {
    int len;
    char payload[4];

    Message() : len(0), payload{} {}
    ~Message() {
        ++g_destroyed;
    }
};

auto quickstart() -> void {
    g_destroyed = 0;
    auto boxed = ustl::Box<Message>::make();
    boxed->len = 4;
    assert(boxed->len == 4);

    auto other = std::move(boxed);
    assert(!boxed && other); // NOLINT(bugprone-use-after-move) moved-from box is specified empty

    auto deep = other.clone();
    assert(deep->len == 4 && deep.as_ptr() != other.as_ptr());

    other = nullptr; // §2：离开作用域时销毁并回收（这里显式置空）
    assert(g_destroyed == 1);
    deep = nullptr;
    assert(g_destroyed == 2);
}

// ==== 模式 A：普通独占对象（§5）====
struct IrqEvent {
    explicit IrqEvent(int line) : m_line(line) {}
    int m_line;
    ~IrqEvent() {
        ++g_destroyed;
    }
};

auto pattern_plain() -> void {
    g_destroyed = 0;
    auto event = ustl::Box<IrqEvent>::make(42);
    assert(event->m_line == 42);
}

// ==== 模式 B：所有权让渡（§5）====
IrqEvent *g_callback_arg;

auto pattern_raw_handoff() -> void {
    g_destroyed = 0;
    auto box = ustl::Box<IrqEvent>::make(7);
    auto *raw = box.into_raw();
    assert(!box);
    g_callback_arg = raw; // platform_set_callback_arg(raw)
    assert(g_destroyed == 0);

    auto back = ustl::Box<IrqEvent>::from_raw(g_callback_arg); // take 回来
    assert(back->m_line == 7);
}

// ==== 模式 C：泄漏保活（§5）====
struct Registry {
    int entries = 0;
    ~Registry() {
        ++g_destroyed;
    }
};
Registry *g_registry;

auto pattern_leak() -> void {
    g_destroyed = 0;
    auto box = ustl::Box<Registry>::make();
    g_registry = &box.leak();
    assert(!box);
    assert(g_registry->entries == 0); // 与内核同寿命，永不销毁
    assert(g_destroyed == 0);
}

// ==== 模式 D：对象缓存回收（§5）====
struct SlabObject {
    static int g_seq;

    struct Disposer {
        auto operator()(SlabObject *p) const noexcept -> void {
            ++g_cache_freed; // g_self_cache->free(p)
            static_cast<void>(p);
        }
    };

    ~SlabObject() {
        ++g_destroyed;
        ++SlabObject::g_seq;
    }
};
int SlabObject::g_seq = 0;

auto pattern_object_cache() -> void {
    g_destroyed = 0;
    g_cache_freed = 0;
    SlabObject::g_seq = 0;
    {
        // g_self_cache->alloc<SlabObject>() 的替身：普通 new，回收走 Disposer
        auto owned = ustl::Box<SlabObject>::from_raw(new SlabObject());
        assert(owned);
    }
    assert(g_destroyed == 1); // ~SlabObject() 先执行
    assert(SlabObject::g_seq == 1);
    assert(g_cache_freed == 1); // Disposer 把存储还给缓存
}

// ==== 模式 E：多态（§5）====
struct Handler {
    virtual auto invoke() -> int = 0;
    virtual ~Handler() = default;
};
struct TimerHandler final: Handler {
    auto invoke() -> int override {
        return 1;
    }
};

auto pattern_polymorphic() -> void {
    g_destroyed = 0;
    ustl::Box<Handler> handler = ustl::Box<TimerHandler>::make();
    assert(handler->invoke() == 1);
}

// ==== 模式 F：深拷贝（§5）====
struct Config {
    int level = 3;
};

auto pattern_clone() -> void {
    auto config = ustl::Box<Config>::make();
    config->level = 5;
    auto snapshot = config.clone();
    snapshot->level = 9;
    assert(config->level == 5); // 深拷贝互不影响
}

} // namespace

auto main() -> int {
    quickstart();
    pattern_plain();
    pattern_raw_handoff();
    pattern_leak();
    pattern_object_cache();
    pattern_polymorphic();
    pattern_clone();
    std::printf("all box doc examples passed\n");
    return 0;
}
