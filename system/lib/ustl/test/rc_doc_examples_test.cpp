/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
/// Compiles-and-runs the code examples from docs/rc.md to keep the tutorial honest.

#include <ustl/rc.hpp>

#include <utility>

#include <cassert>
#include <cstdint>

namespace {

int g_destroyed = 0;

// ==== 快速开始（§2）====
class Session: public ustl::RefCounted<Session> {
  public:
    explicit Session(int id) : m_id(id) {}
    [[nodiscard]]
    auto id() const -> int {
        return m_id;
    }
    ~Session() {
        ++g_destroyed;
    }

  private:
    int m_id;
};

auto quickstart() -> void {
    g_destroyed = 0;
    auto session = ustl::Rc<Session>::adopt(new Session(42));
    auto copy = session;
    auto weak = session.downgrade();
    assert(session->id() == 42);
    assert(copy.use_count() == 2);
    copy = nullptr;
    assert(weak.lock());
    session = nullptr;
    assert(g_destroyed == 1);
    assert(!weak.lock());
}

// ==== 线程策略（§4）====
class CpuLocalObj: public ustl::RefCounted<CpuLocalObj, ustl::sync::SingleThreadedPolicy> {
  public:
    ~CpuLocalObj() {
        ++g_destroyed;
    }
};

auto policies() -> void {
    g_destroyed = 0;
    ustl::Rc<CpuLocalObj, ustl::sync::SingleThreadedPolicy> o =
        ustl::Rc<CpuLocalObj, ustl::sync::SingleThreadedPolicy>::adopt(new CpuLocalObj());
    assert(o);
    o = nullptr;
    assert(g_destroyed == 1);
}

// ==== 模式 B：单例保活（§6）====
static Session *g_singleton;

auto singleton() -> void {
    g_destroyed = 0;
    auto rc = ustl::Rc<Session>::adopt(new Session(1));
    g_singleton = rc.into_raw();                     // 让渡，计数 1 永久保活
    auto handle = ustl::adopt<Session>(g_singleton); // 递增回包
    assert(handle.use_count() == 2);
    handle = nullptr; // 回 1
    // 单例永不回收（有意的）；测试收尾时手动回收
    auto fin = ustl::Rc<Session>::adopt(g_singleton);
    fin = nullptr;
    g_singleton = nullptr;
}

// ==== 模式 C：缓存 / 观察者（§6）====
auto weak_cache() -> void {
    g_destroyed = 0;
    class CacheEntry: public ustl::RefCounted<CacheEntry> {
      public:
        ~CacheEntry() {
            ++g_destroyed;
        }
    };
    ustl::Rc<CacheEntry> entry = ustl::Rc<CacheEntry>::adopt(new CacheEntry());
    ustl::Weak<CacheEntry> slot = entry.downgrade();
    if (auto hit = slot.lock()) {
        (void)hit;
    }
    entry = nullptr;
    assert(g_destroyed == 1);
    assert(!slot.lock()); // 命中失败，不悬垂
}

// ==== 模式 D1：嵌套 Disposer（§6）====
struct SelfCache;
struct MemCache: ustl::RefCounted<MemCache> {
    static int g_freed;
    struct Disposer {
        auto operator()(MemCache *) const noexcept -> void {
            ++MemCache::g_freed;
        }
    };
    ~MemCache() {
        ++g_destroyed;
    }
};
int MemCache::g_freed = 0;

auto disposer_class() -> void {
    g_destroyed = 0;
    MemCache::g_freed = 0;
    {
        auto m = ustl::Rc<MemCache>::adopt(new MemCache());
    }
    assert(g_destroyed == 1);
    assert(MemCache::g_freed == 1); // 自定义回收优先于 delete
}

// ==== 模式 D2：set_deallocate（§6）====
struct Cacheable: ustl::RefCounted<Cacheable> {
    using ustl::RefCounted<Cacheable>::set_deallocate;
    ~Cacheable() {
        ++g_destroyed;
    }
};
int g_reclaimed = 0;

auto disposer_instance() -> void {
    g_destroyed = 0;
    g_reclaimed = 0;
    auto *raw = new Cacheable();
    raw->set_deallocate([](void *) noexcept { ++g_reclaimed; });
    {
        auto c = ustl::Rc<Cacheable>::adopt(raw);
    }
    assert(g_destroyed == 1);
    assert(g_reclaimed == 1);
}

// ==== 模式 E：checked downcast（§6）====
class VmAreaOrMapping: public ustl::RefCounted<VmAreaOrMapping> {
  public:
    static constexpr std::uintptr_t TYPE_TAG = 1;
    [[nodiscard]]
    auto rc_type_tag() const noexcept -> std::uintptr_t override {
        return TYPE_TAG;
    }
    virtual ~VmAreaOrMapping() = default;
};
class VmArea final: public VmAreaOrMapping {
  public:
    static constexpr std::uintptr_t TYPE_TAG = 2;
    [[nodiscard]]
    auto rc_type_tag() const noexcept -> std::uintptr_t override {
        return TYPE_TAG;
    }
    ~VmArea() override {
        ++g_destroyed;
    }
};

auto downcast_checked() -> void {
    g_destroyed = 0;
    ustl::Rc<VmAreaOrMapping> aom = ustl::Rc<VmArea>::adopt(new VmArea());
    if (auto area = ustl::downcast<VmArea>(aom)) {
        assert(area);
    }
    assert(!ustl::downcast<VmArea>(ustl::Rc<VmAreaOrMapping>::adopt(new VmAreaOrMapping())));
    // 向上转型永远安全
    ustl::Rc<VmArea> area = ustl::Rc<VmArea>::adopt(new VmArea());
    ustl::Rc<VmAreaOrMapping> base = ustl::upcast<VmAreaOrMapping>(std::move(area));
    assert(base);
}

} // namespace

auto main() -> int {
    quickstart();
    policies();
    singleton();
    weak_cache();
    disposer_class();
    disposer_instance();
    downcast_checked();
    return 0;
}
