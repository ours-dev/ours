# ustl::Rc / Weak / RefCounted 使用指南

> 适用于重写后的 `system/lib/ustl/include/ustl/rc.hpp`（侵入式引用计数智能指针）。
> 环境约束：`-fno-exceptions -fno-rtti -ffreestanding`，不能用 `std::shared_ptr` / `dynamic_cast`。

## 1. 概念总览

这一套工具由三个类型组成，各管一件事：

| 类型 | 角色 | 一句话 |
|---|---|---|
| `RefCounted<T, Policy>` | **内嵌计数器**（基类） | 你的类继承它，对象自带强/弱两个引用计数 |
| `Rc<T>` | **单线程**强所有权指针 | 非原子计数，Rust 的 `Rc`；最后一个 `Rc` 销毁时对象被析构 |
| `Arc<T>` | **跨线程**强所有权指针 | 原子计数，Rust 的 `Arc`（=`Rc<T, ThreadSafePolicy>` 别名） |
| `Weak<T, Policy>` | **弱观察**智能指针 | 不拥有对象，但能安全地问"对象还活着吗、给我一个 `Rc`" |

和 Rust / `std::shared_ptr` 的对应关系：

| Rust | std | ustl | 差异 |
|---|---|---|---|
| `Rc<T>` | — | `ustl::Rc<T>` | 非原子，仅单线程 |
| `Arc<T>` | `std::shared_ptr<T>` | `ustl::Arc<T>` | 原子计数；侵入式（计数内嵌对象，无独立控制块） |
| `Weak<Rc<T>>` / `Weak<Arc<T>>` | `std::weak_ptr<T>` | `ustl::Weak<T>` / `Weak<T, ThreadSafePolicy>` | 语义相同，实现更省 |

**什么时候用什么：**

- **单线程内共享所有权** → `Rc<T>`（零原子开销）。
- **跨 CPU / 多线程共享所有权** → `Arc<T>`（显式选择，Rust 风格）。
- **观察 / 缓存 / 父指针 / 打破环**（不能影响对象生死）→ `Weak<T>`（配 `Rc`）或 `Weak<T, ThreadSafePolicy>`（配 `Arc`）。

---

## 2. 快速开始

```cpp
#include <ustl/rc.hpp>

class Session : public ustl::RefCounted<Session> {
  public:
    explicit Session(int id) : m_id(id) {}
    auto id() const -> int { return m_id; }
  private:
    int m_id;
};

// 创建：对象用普通方式分配（new / new (gaf) / 对象缓存），然后用 adopt 包装
auto session = ustl::Rc<Session>::adopt(new Session(42));

auto copy = session;              // 拷贝共享所有权，计数 1 → 2
auto weak = session.downgrade();  // 建立弱引用

session->id();                    // 42
copy.use_count();                 // 2

copy = nullptr;                   // 计数 2 → 1，对象仍活着
weak.lock();                      // 成功：返回一个新的 Rc<Session>
// 最后一个 Rc 销毁时，Session 被析构，内存被回收
```

**注意**：`adopt` 要求对象"出生时计数为 0"（`new` / `new (gaf) T` / 对象缓存分配出来的对象天然满足）。`adopt` 把计数从 0 变成 1，完成所有权交接。

---

## 3. 生命周期模型（核心概念，务必理解）

对象里有两个计数，语义完全不同：

```
strong（强计数）= 当前有多少个 Rc<T> 持有这个对象
weak   （弱计数）= 当前有多少个 Weak<T> + 多少个 Rc<T>   // 每个 Rc 额外占一份 weak
```

**销毁分两步走**，这是整套设计的地基：

1. **strong 降到 0** → 执行 `~T()`（销毁对象内容）。内存**先不还**——弱计数还活着。
2. **weak 也降到 0** → 才把内存还给分配器。

推论：

- 只要还有任意一个 `Weak<T>` 存在，**对象的内存（含计数）就一定还活着**——这就是 `Weak::lock()` 无悬垂的保证。
- `Weak<T>` 无法"复活"已销毁的对象：`strong == 0` 时 `lock()` 返回空。
- 对象内容销毁后，`Weak` 还能安全地查 `expired()` / 尝试 `lock()`。

> 一句话记忆：**strong 管"内容生死"，weak 管"内存生死"。**

---

## 4. 线程策略：Rust 式的 `Rc` / `Arc` 分离

计数器按策略选择：**跨线程用 `std::atomic`，单线程用普通整数（零原子指令）**：

```cpp
namespace ustl::sync {
    struct ThreadSafePolicy {};        // 计数 = std::atomic，release/acquire 同步，可跨 CPU
    struct SingleThreadedPolicy {};    // 计数 = 普通 std::size_t，零原子操作（默认）
}
```

> `SingleThreadedPolicy` 的计数是纯非原子的普通整数——编译产物里**没有任何原子指令**（实测：aarch64 下 0 条，而 `ThreadSafePolicy` 有 7 条原子 RMW）。单线程热点路径零开销。

**默认是非原子的 `Rc<T>`**（Rust 语义：`Rc` 只用于单线程，跨线程要显式选 `Arc`）：

```cpp
// 单线程对象（默认策略）：用 Rc
class CpuLocalObj: public ustl::RefCounted<CpuLocalObj> { ... };
ustl::Rc<CpuLocalObj> o = ...;

// 跨 CPU 对象：显式选原子策略 + Arc
class Dispatcher: public ustl::RefCounted<Dispatcher, ustl::sync::ThreadSafePolicy> { ... };
ustl::Arc<Dispatcher> d = ...;
```

**类型级强制**（和 Rust 一样，编译器拦着你混用）：

- `Rc<T>` 要求 `T` 继承 `RefCounted<T>`（非原子）；`Arc<T>` 要求 `T` 继承
  `RefCounted<T, ThreadSafePolicy>`（原子）。
- 策略不匹配 → **编译报错**，不会出现"看着像 Arc、其实非原子"的隐患。
- `Rc<T>` 与 `Arc<T>` 是不同类型，互不转换（和 Rust 的 `Rc`/`Arc` 一致）。

---

## 5. API 速查

### `Rc<T>`（单线程）与 `Arc<T>`（=`Rc<T, ThreadSafePolicy>`，跨线程）

下文用 `Rc` 表示，`Arc` 的 API 完全相同（只是策略参数不同）。

| 成员 | 说明 |
|---|---|
| `static adopt(T *)` | 收养外部已分配对象（计数 0→1） |
| `Rc(T *)` | 同上，构造形式 |
| `Rc(Rc const&)` / `Rc(Rc&&)` | 拷贝（+1）/ 移动（偷走） |
| `Rc(Rc<U> const&)` / `&&` | 向上转型（`U` 派生自 `T`，编译期安全） |
| `operator=(...)` | 拷贝/移动/上转型赋值，自赋值安全 |
| `operator=(nullptr)` / `reset()` | 释放本引用 |
| `use_count()` | 当前强引用数（尽力而为，见 §8） |
| `is_unique()` | `use_count() == 1`（有竞态语义，见 §8） |
| `try_get_mut()` | 仅当独占时返回 `T*`，否则 `nullptr` |
| `as_ptr()` / `as_ptr_mut()` | 裸指针（`as_ptr_mut` 非 const 成员） |
| `as_ref()` / `as_ref_mut()` | 引用 |
| `operator->` / `operator*` | 成员访问 / 解引用 |
| `explicit operator bool` | 是否非空 |
| `downgrade()` | 建立 `Weak<T>` |
| `into_raw()` | 让渡强引用给裸指针（见 §6 模式 B） |
| `downcast<U>()` | checked 下行转换（需 opt-in 标签，见 §6 模式 E） |
| `operator==` | 指针相等比较（含 `nullptr`） |

### `Weak<T, Policy>`

| 成员 | 说明 |
|---|---|
| `lock()` / `upgrade()` | 尝试升级为 `Rc`；对象已亡则返回空 |
| `expired()` | 对象内容是否已销毁 |
| `use_count()` | 当前强引用数（尽力而为） |
| `as_ptr()` | 裸指针（仅供调试/判等） |
| `operator==` | 指针相等比较 |

### `RefCounted<T, Policy>`（对象基类）

| 成员 | 说明 |
|---|---|
| `retain()` / `release()` | 增/减一个强引用（`Rc` 内部调用） |
| `retain_weak()` / `release_weak()` | 增/减一个弱引用（`Weak` 内部调用） |
| `lock_strong()` | `Weak::lock()` 的底层 CAS 入口 |
| `strong_count()` / `weak_count()` | 当前计数 |
| `rc_type_tag()` | checked downcast 的类型标签（默认 0） |
| `set_deallocate(fn)` | 按实例定制内存回收（protected） |
| `virtual ~RefCounted()` | 虚析构：保证多态销毁正确 |

**一般你只需要**：继承 `RefCounted` → 用 `adopt` 建 `Rc` → 需要时 `downgrade` 拿 `Weak`。其余成员是 `Rc`/`Weak` 的实现细节，除非你要深度定制。

### 自由函数

```cpp
template <typename T, typename Policy = sync::SingleThreadedPolicy, typename U>
auto adopt(U *pointer) noexcept -> Rc<T, Policy>;   // 收养裸指针（跨线程显式传 Policy）
auto adopt<T>(Rc<U, Policy> const& / &&) -> Rc<T, Policy>;   // 向上转型（策略自动推导）
auto upcast<T>(Rc<U, Policy> const& / &&) -> Rc<T, Policy>;  // 显式上转型
auto downcast<T>(Rc<U, Policy> const&) -> Rc<T, Policy>;     // checked 下行转换
```
> 跨线程对象的裸指针收养写 `ustl::Arc<X>::adopt(ptr)`（或 `adopt<X, ThreadSafePolicy>(ptr)`）；
> 从已有 `Arc` 的转换无需指定策略（自动推导）。

---

## 6. 常见模式

### 模式 A：普通共享对象

```cpp
class VmObject : public ustl::RefCounted<VmObject> { ... };

auto vmo = ustl::Rc<VmObject>::adopt(new VmObject());
mapping->attach(vmo);   // 传入拷贝，计数 +1
```

### 模式 B：单例保活（`into_raw` + `adopt`）

把对象"永久"锚在一个静态裸指针上，每次取用时重新包装：

```cpp
static VmObject *g_singleton;   // 全局唯一，靠计数 1 保活

// 初始化：让渡一个强引用，对象从此永不被回收
g_singleton = rc.into_raw();

// 取用：递增包装（计数 1 → 2），用完释放回 1
auto handle = ustl::Arc<VmObject>::adopt(g_singleton);   // 内核对象跨 CPU，用 Arc
```

> `into_raw()` 是**转移所有权**：返回的裸指针带着一个强引用，对象因此永生。
> 不要把它当普通 `get()` 用；误用 = 静默泄漏。

### 模式 C：缓存 / 观察者（`Weak`）

```cpp
class CacheEntry : public ustl::RefCounted<CacheEntry> { ... };

ustl::Rc<CacheEntry> entry = ...;
ustl::Weak<CacheEntry> slot = entry.downgrade();   // 缓存里放弱引用

// 取缓存：活着就给，死了返回空，不会悬垂
if (auto hit = slot.lock()) {
    use(hit);
} else {
    // 缓存未命中，重新构造
}
```

### 模式 D：对象缓存回收（`T::Disposer` / `set_deallocate`）

内核对象常用 slab/object-cache 分配，内存**不能**交给 `operator delete`。两条路：

**D1：类级定制——嵌套 `Disposer`（编译期自动发现，优先于 `delete`）**

```cpp
class MemCache : public ustl::RefCounted<MemCache> {
  public:
    struct Disposer {                       // 命名必须叫 Disposer
        void operator()(MemCache *p) const noexcept {
            g_self_cache->free(p);          // 只负责"还内存"
        }
    };
    ...
};
```

**D2：实例级定制——`set_deallocate`（protected，在构造时调用）**

```cpp
MemCache::MemCache(...) {
    // 覆盖默认的 operator delete
    set_deallocate(&MemCache::reclaim_storage);
}
auto MemCache::reclaim_storage(void *p) noexcept -> void {
    g_self_cache->free(p);
}
```

> **契约**：`Disposer` / deallocate 函数只负责**回收存储**，**绝不**调用析构——
> 析构已由 `RefCounted` 在 strong→0 时执行。别写成 `delete p`（会二次析构）。

### 模式 E：多态与 checked downcast

无 RTTI 环境里，下行转换需要类型标签 **opt-in**：定义 `TYPE_TAG` 并覆写 `rc_type_tag()`。

```cpp
class VmAreaOrMapping : public ustl::RefCounted<VmAreaOrMapping> {
  public:
    static constexpr std::uintptr_t TYPE_TAG = 1;
    auto rc_type_tag() const noexcept -> std::uintptr_t override { return TYPE_TAG; }
};

class VmArea final : public VmAreaOrMapping {
  public:
    static constexpr std::uintptr_t TYPE_TAG = 2;   // 每类一个唯一标签
    auto rc_type_tag() const noexcept -> std::uintptr_t override { return TYPE_TAG; }
};

ustl::Rc<VmAreaOrMapping> aom = ...;
if (auto area = ustl::downcast<VmArea>(aom)) {   // 类型不符返回空，而非 UB
    ...
}
```

- **向上转型永远安全**：`Rc<Base> b = std::move(rcDerived);` 或 `ustl::upcast<Base>(...)`。
- **下行转换**：目标类型没有 `TYPE_TAG` 时，`downcast` 直接**编译期禁用**，从根上杜绝裸 `static_cast`。

---

## 7. 存储回收的优先级（disposer 决策树）

```
对象需要回收内存时（weak → 0）：
  └─ set_deallocate 被调用过？      → 用自定义函数
  └─ 否则 T 内定义了 Disposer？     → 用 T::Disposer
  └─ 否则                           → operator delete（内核里即 ktl::kfree）
```

默认 `operator delete` 只适用于 `new`/`new (gaf)` 分配的对象；**对象缓存分配的对象必须定制**（模式 D）。

---

## 8. 陷阱与注意事项

1. **`use_count()` / `is_unique()` 是"尽力而为"**。多线程下检查与使用之间，别的线程可能增删引用。
   要做"独占才修改"，用 `try_get_mut()` 而不是 `is_unique() + as_ptr_mut()`。
2. **`as_ptr_mut()` / `as_ref_mut()` 是非 const 成员**（const 修正）。`const Rc<T>` 只能读到 `T const*`。
3. **不要在析构函数里重建指向自己的 `Rc`**（`adopt(this)`）：strong 已归零，重建会二次析构。这是侵入式计数的通用禁忌，与 `shared_ptr` 相同。
4. **`into_raw()` 会永久保活对象**，必须配合 `adopt` 回收，否则泄漏。
5. **`adopt` 只接受"出生计数为 0"的裸指针**。对已在其他 `Rc` 管理下的指针再 `adopt` = 重复计数的逻辑错误。
6. **`Weak` 的存在会拖延内存回收**（weak→0 才还内存）。长生命周期缓存注意及时清空 `Weak`。
7. **策略不匹配会被编译器拦截**：`Arc<T>` 配单线程对象、`Rc<T>` 配跨线程对象都会编译报错。`Rc<T>` 与 `Arc<T>` 互不转换——这就是 Rust 式类型分离的意义。

## 9. 从旧 API 迁移（2025 初版 → 新版）

| 旧 | 新 |
|---|---|
| `class T : public ustl::RefCounter<T>` | `class T : public ustl::RefCounted<T>` |
| `ustl::make_rc<T>(ptr)` | `ustl::adopt<T>(ptr)` |
| `rc.take()` | `rc.into_raw()`（语义同：让渡强引用） |
| `rc.as_ptr_mut()`（const 上可用） | 仅非 const 可用 |
| `ustl::Rc<X>`（默认原子） | 跨线程对象用 `ustl::Arc<X>`；`ustl::Rc<T>` 默认非原子（单线程，Rust 语义） |
| `ustl::downcast<T>(rc)`（裸 static_cast） | `ustl::downcast<T>(rc)`（需标签，运行时判型） |

旧版没有的：`Weak<T>`、`SingleThreadedPolicy`、`try_get_mut`、checked downcast、`Disposer`/`set_deallocate`。

## 10. 内存序（给想深究的人）

计数器按策略**编译期标签分发**（`if constexpr`）：`ThreadSafePolicy` 用 `std::atomic<std::size_t>`，`SingleThreadedPolicy` 用普通 `std::size_t`。内存序只在 ThreadSafe 路径有意义：

- **`ThreadSafePolicy`**：计数是 `std::atomic<std::size_t>`。递增用 `std::memory_order_relaxed`（调用者必已持有引用，对象必然存活，无需同步）；可能到零的递减用 `std::memory_order_acq_rel`（释放对象状态 + 销毁/回收前获取）；`Weak::lock()` 的 CAS 成功用 `std::memory_order_acquire`（保证升级成功后能看到前一个持有者发布的对象状态）。
- **`SingleThreadedPolicy`**：计数是普通 `std::size_t`，直接 `++`/`--`，无原子操作、无内存序（单线程天然有序）。
- 销毁/回收后读取计数属于"延迟回收"惯用法：计数与 deallocate 指针是平凡析构字段，内存由
  `weak > 0 ⟹ 内存存活` 不变量保证可达，这是内核标准写法。
