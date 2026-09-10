/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
/// Compiles-and-runs the code examples from docs/box.md to keep the tutorial honest.

#include <utility>

#include <cassert>
#include <cstdio>

#include <ustl/box.hpp>

namespace {

int DESTROYED = 0;
int CACHE_FREED = 0;

// ==== 快速开始（§2）====
struct Message {
    int len;
    char payload[4];

    Message() : len(0), payload{} {}
    ~Message() {
        ++DESTROYED;
    }
};

auto quickstart() -> void {
    DESTROYED = 0;
    auto boxed = ustl::Box<Message>::make();
    boxed->len = 4;
    assert(boxed->len == 4);

    auto other = std::move(boxed);
    assert(!boxed && other); // NOLINT(bugprone-use-after-move) moved-from box is specified empty

    auto deep = other.clone();
    assert(deep->len == 4 && deep.as_ptr() != other.as_ptr());

    other = nullptr; // §2：离开作用域时销毁并回收（这里显式置空）
    assert(DESTROYED == 1);
    deep = nullptr;
    assert(DESTROYED == 2);
}

// ==== 模式 A：普通独占对象（§5）====
struct IrqEvent {
    explicit IrqEvent(int line) : m_line(line) {}
    int m_line;
    ~IrqEvent() {
        ++DESTROYED;
    }
};

auto pattern_plain() -> void {
    DESTROYED = 0;
    auto event = ustl::Box<IrqEvent>::make(42);
    assert(event->m_line == 42);
}

// ==== 模式 B：所有权让渡（§5）====
IrqEvent *CALLBACK_ARG;

auto pattern_raw_handoff() -> void {
    DESTROYED = 0;
    auto box = ustl::Box<IrqEvent>::make(7);
    auto *raw = box.into_raw();
    assert(!box);
    CALLBACK_ARG = raw; // platform_set_callback_arg(raw)
    assert(DESTROYED == 0);

    auto back = ustl::Box<IrqEvent>::from_raw(CALLBACK_ARG); // take 回来
    assert(back->m_line == 7);
}

// ==== 模式 C：泄漏保活（§5）====
struct Registry {
    int entries = 0;
    ~Registry() {
        ++DESTROYED;
    }
};
Registry *REGISTRY;

auto pattern_leak() -> void {
    DESTROYED = 0;
    auto box = ustl::Box<Registry>::make();
    REGISTRY = &box.leak();
    assert(!box);
    assert(REGISTRY->entries == 0); // 与内核同寿命，永不销毁
    assert(DESTROYED == 0);
}

// ==== 模式 D：对象缓存回收（§5）====
struct SlabObject {
    static int SEQ;

    struct Disposer {
        auto operator()(SlabObject *p) const noexcept -> void {
            ++CACHE_FREED; // SELF_CACHE->free(p)
            static_cast<void>(p);
        }
    };

    ~SlabObject() {
        ++DESTROYED;
        ++SlabObject::SEQ;
    }
};
int SlabObject::SEQ = 0;

auto pattern_object_cache() -> void {
    DESTROYED = 0;
    CACHE_FREED = 0;
    SlabObject::SEQ = 0;
    {
        // SELF_CACHE->alloc<SlabObject>() 的替身：普通 new，回收走 Disposer
        auto owned = ustl::Box<SlabObject>::from_raw(new SlabObject());
        assert(owned);
    }
    assert(DESTROYED == 1); // ~SlabObject() 先执行
    assert(SlabObject::SEQ == 1);
    assert(CACHE_FREED == 1); // Disposer 把存储还给缓存
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
    DESTROYED = 0;
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
