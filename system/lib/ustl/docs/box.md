# ustl::Box 使用指南

> 适用于 `system/lib/ustl/include/ustl/box.hpp`（仿 Rust `Box<T>` 的独占所有权智能指针）。
> 环境约束：`-fno-exceptions -fno-rtti -ffreestanding`，不能用 `std::unique_ptr` / `std::shared_ptr`。

## 1. 概念总览

`Box<T>` 是对象的**唯一拥有者**：不可拷贝（编译期禁止），移动即移交所有权，
盒子销毁的瞬间对象被析构、存储被回收。它补全了本仓库的所有权词汇表：

| 场景 | 用什么 |
|---|---|
| 独占所有权，超出作用域存活 | `ustl::Box<T>` |
| 单线程共享所有权 | `ustl::Rc<T>`（见 [rc.md](rc.md)） |
| 跨 CPU 共享所有权 | `ustl::Arc<T>` |
| 不影响对象生死的观察 | `ustl::Weak<T>` |
| 纯借用（不拥有） | 裸指针 / 引用 |

和 Rust / std 的对应关系：

| Rust | std | ustl | 差异 |
|---|---|---|---|
| `Box<T>` | `std::unique_ptr<T>` | `ustl::Box<T>` | 可为空（默认构造为空，同 `Rc` 形态）；回收支持 `T::Disposer` |
| `Box::new(v)` | `make_unique<T>(v)` | `Box<T>::make(v)` | `make` **原地构造**，免去完整对象的移动 |
| `Box::from_raw` / `into_raw` | `release()` / `reset(p)` | 同名方法 | 语义相同 |
| `Box::leak` | — | `leak()` | 放弃销毁，对象永生 |
| `#[derive(Clone)] Box<T>` | 拷贝构造（深拷贝） | `clone()` | 要求 `T` 可拷贝构造；`Box` 本身不可拷贝 |

**选型一句话**：只有一个所有者、又需要它在堆上（大于栈预算、或要移动所有权）→ `Box<T>`；
一旦可能出现第二个所有者，直接上 `Rc<T>` / `Arc<T>`，不要用裸指针“模拟共享”。

---

## 2. 快速开始

```cpp
#include <ustl/box.hpp>

struct Message {
    usize len;
    char payload[64];
};

auto boxed = ustl::Box<Message>::make();   // 分配 + 原地默认构造
boxed->len = 4;                            // operator->：和裸指针一样用

auto other = std::move(boxed);             // 移动 = 所有权移交
// assert(!boxed);                         // 移动源变空，不可再用

auto deep = other.clone();                 // 深拷贝（要求 Message 可拷贝构造）
// other 离开作用域时：~Message() 执行，然后存储被回收
```

---

## 3. 生命周期模型

`Box` 的销毁是**严格两步**，与 `RefCounted` 的存储回收共用一套契约：

1. 执行 `~T()`（销毁对象内容）；
2. 回收存储：`T` 定义了嵌套 `Disposer` 就用它，否则 `operator delete`
   （内核里即 `ktl::kfree`）。

```
盒子销毁时：
  └─ T 内定义了 Disposer？   → 用 T::Disposer
  └─ 否则                    → operator delete
```

> **契约**：`Disposer` 只负责“还内存”，**绝不**调用析构——析构已由 `Box`
> 在第一步执行。别写成 `delete p`（会二次析构）。

与 `Rc` 不同，`Box` **没有计数**：不存在“最后一个引用”的概念，盒子死了对象就死。
也因此 `Box` 天然独占，随时可以拿可变引用（`as_ref_mut()` / 非 const 的
`operator->`），不需要 `try_get_mut()` 这类独占检查。

---

## 4. API 速查

| 成员 | 说明 |
|---|---|
| `static make(args...)` | 分配 + 原地构造（Rust `Box::new`） |
| `static from_raw(T *)` | 收养外部存储（Rust `Box::from_raw`），成为唯一拥有者 |
| `static adopt(T *)` | 同 `from_raw`；`Rc::adopt` 的独占版叫法，仓库词汇统一 |
| `Box(T *)` | 同上，构造形式（explicit） |
| `Box(Box&&)` / `operator=(Box&&)` | 移动 / 移动赋值（自赋值安全） |
| `Box(Box<U>&&)` | 向上转型（`U` 派生自 `T`，编译期安全） |
| 拷贝构造 / 拷贝赋值 | **`= delete`**（Rust 的 `Box` 不可 `Copy`） |
| `operator=(nullptr)` / `reset()` | 销毁所拥有对象并置空 |
| `reset(T *)` | 销毁旧对象、收养新指针（自收养为无操作） |
| `swap(Box&)` / `swap(a, b)` | 交换所有权 |
| `into_raw()` | 让渡所有权给裸指针（见 §5 模式 B） |
| `leak()` | 放弃销毁，对象永生，返回 `T&`（见 §5 模式 C） |
| `clone()` | 深拷贝，仅当 `T` 可拷贝构造时可用；空盒克隆为空 |
| `as_ptr()` / `as_ptr_mut()` | 裸指针（`as_ptr_mut` 非 const 成员） |
| `as_ref()` / `as_ref_mut()` | 引用 |
| `operator->` / `operator*` | 成员访问 / 解引用（const 重载齐全） |
| `explicit operator bool` | 是否非空 |
| `operator==` | 指针相等比较（含 `nullptr`） |

---

## 5. 常见模式

### 模式 A：普通独占对象

```cpp
struct IrqEvent {
    explicit IrqEvent(u32 line) : m_line(line) {}
    u32 m_line;
};

auto event = ustl::Box<IrqEvent>::make(42);
event->m_line;   // 42
```

### 模式 B：所有权让渡（`into_raw` + `from_raw`）

把对象交给不认识 `Box` 的 C 接口 / 平台结构，事后再拿回来：

```cpp
auto box = ustl::Box<IrqEvent>::make(42);
auto *raw = box.into_raw();   // box 变空；raw 独占所有权
platform_set_callback_arg(raw);
// ...
auto back = ustl::Box<IrqEvent>::from_raw(platform_take_callback_arg());
```

> `into_raw()` 之后**没有任何东西会销毁对象**：要么 `from_raw` 收回，要么就是泄漏。

### 模式 C：泄漏保活（`leak`）

启动期对象、全局注册表节点——本来就是“永不销毁”的：

```cpp
static Registry *g_registry;

auto init() -> void {
    auto box = ustl::Box<Registry>::make();
    g_registry = &box.leak();   // 有意泄漏：Registry 与内核同寿命
}
```

### 模式 D：对象缓存回收（`T::Disposer` + `from_raw`）

slab / object-cache 分配的对象，存储不能交给 `operator delete`。和 `Rc`
共享同一套定制点：

```cpp
struct SlabObject {
    struct Disposer {                       // 命名必须叫 Disposer
        auto operator()(SlabObject *p) const noexcept -> void {
            g_self_cache->free(p);          // 只负责"还内存"
        }
    };
};

auto *storage = g_self_cache->alloc<SlabObject>();  // 缓存分配 + placement-new
auto owned = ustl::Box<SlabObject>::from_raw(storage);
// owned 销毁时：~SlabObject() → Disposer 把存储还给缓存
```

> `make()` 用全局 `new` 分配，只配默认的 `operator delete` 回收；
> **带 `Disposer` 的类型不要用 `make()`**，一律缓存分配 + `from_raw`。

### 模式 E：多态（`Box<Base>`）

Rust 的 `Box<dyn Trait>` 在 C++ 里就是指向虚基类的 `Box`。**基类必须有虚析构**：

```cpp
struct Handler {
    virtual auto invoke() -> void = 0;
    virtual ~Handler() = default;
};
struct TimerHandler final: Handler {
    auto invoke() -> void override { /* ... */ }
};

ustl::Box<Handler> handler = ustl::Box<TimerHandler>::make();
handler->invoke();   // 动态分发
```

### 模式 F：深拷贝（`clone`）

`Box` 不可拷贝（防止无意识的双重释放/浅拷贝），需要副本时显式说：

```cpp
auto config = ustl::Box<Config>::make();
auto snapshot = config.clone();   // Config 需可拷贝构造
```

---

## 6. 陷阱与注意事项

1. **`Box` 不可拷贝**——`auto b2 = b;` 编译报错，这是特性不是缺陷；要副本用 `clone()`，
   要移交用 `std::move`。
2. **移动源是空盒**。移动后继续解引用 = 对空指针解引用；先 `if (box)` 再用。
3. **`into_raw()` 之后对象无人看管**，必须 `from_raw` 收口，否则泄漏（模式 B）。
4. **`leak()` 是有意泄漏**，仅用于确知与内核同寿命的对象；用错了就是内存泄漏。
5. **多态销毁需要虚析构**。`Box<Base>` 存放派生对象而 `Base` 无虚析构 = 未定义行为，
   和裸指针完全一样。
6. **`Disposer` 只还内存，不析构**（契约见 §3）；写成 `delete p` 会二次析构。
7. **空盒上的一切访问都是 UB**（同裸指针）。`Box` 的空态只用于“还没拥有”的中间态，
   不做延迟解引用的 Option 用——要“有或没有”，配合判空使用，或用 `ustl::Option`。
