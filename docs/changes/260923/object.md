## Dispatcher 带上对象类型；KernelHandle 算作一个持有者；去掉必然失败的 canary 校验（2026-09-23）

**文件**
- `system/public/ours/types.hpp` — 新增 `ObjectType`，取值同 Zircon `ZX_OBJ_TYPE_*`
- `kernel/object/include/ours/object/dispatcher.hpp` — `type_tag()`、`Dispatcher::type()`；`SoloDispatcher`/`PeeredDispatcher` 以 `Derived::TYPE_TAG` 回答 `rc_type_tag()`
- `kernel/object/include/ours/object/*_dispatcher.hpp` — 各自公开 `TYPE_TAG`
- `kernel/object/include/ours/object/handle.hpp` — `KernelHandle` 析构时调 `on_zero_handles()`；新增 `release()`、无参 `reset()`；修好调用了不存在的 `on_zero()` 的 `reset()`
- `kernel/object/dispatcher.cpp` — `attach_observer`/`detach_observer` 去掉 `m_canary.verify<Self>()`

**症状**
潜伏，三处都要等第一个真正经过句柄的系统调用才会触发：
1. 句柄里存的是 `Arc<Dispatcher>`，系统调用拿到后要转回它期望的具体类型，而今天没有任何受检的转法；
2. `attach_observer`/`detach_observer` 在 debug 构建里对任何真实对象都会断言失败；
3. 一个从没进过用户句柄表的对象（比如 channel 对的一端在出错路径上被丢掉），永远收不到「最后一个持有者没了」，
   等 PEER_CLOSED 的对端会一直等下去。

**原因**
1. ustl 其实已经有受检的向下转型：`Rc::downcast<U>()`，前提是类型公开 `TYPE_TAG` 并由 `rc_type_tag()` 回答它。
   dispatcher 们只是没接上。
2. canary 由 `SoloDispatcher` 的构造函数用 `Derived::MAGIC` 初始化，而基类方法校验的是
   `magic<Dispatcher>()`——拿 "ChannelDispatcher" 去比 "Dispatcher"，必然不等。一直没炸只是因为没人调用过。
3. `KernelHandle` 的析构是 `= default`。Zircon 里 KernelHandle 就是一个「内核自己持有的句柄」，析构时会调
   `on_zero_handles()`；转进用户句柄时走 `release()`，把引用交出去而不发通知。

**改法**
- 对象类型直接做成 `rc_type_tag()` 的值：`type_tag(ObjectType::Channel)`。「这是什么对象」和
  「向下转型前检查什么」于是是同一个数。复用的是 `RefCounted` 已有的虚函数，没有新增虚函数；
  `ObjectType` 放在公共头里，取 Zircon 的数值，将来 `object_get_info` 可以原样上报。
- canary：基类不知道具体类型，所以不在基类里校验；调用它们的 dispatcher 方法会通过 `verify_canary()` 校验。

改前：

```cpp
    ~KernelHandle() = default;
    ...
    auto reset(ustl::Arc<U> dispatcher) -> void {
        if (m_dispatcher) {
            m_dispatcher->on_zero();
        }
        m_dispatcher = ustl::move(dispatcher);
    }
```

改后：

```cpp
    ~KernelHandle() {
        if (m_dispatcher) {
            m_dispatcher->on_zero_handles();
        }
    }

    [[nodiscard]]
    auto release() -> ustl::Arc<T> {
        return ustl::move(m_dispatcher);
    }

    auto reset(ustl::Arc<U> dispatcher) -> void {
        if (m_dispatcher) {
            m_dispatcher->on_zero_handles();
        }
        m_dispatcher = ustl::move(dispatcher);
    }
```

移动赋值改为 `reset(other.release())`，被覆盖的旧对象同样会收到通知。

**参照**
Zircon `KernelHandle`（`kernel/object/include/object/handle.h`：析构与 `reset()` 调 `on_zero_handles()`，
`release()` 不调）；`DownCastDispatcher<T>()`（`kernel/object/include/object/dispatcher.h`，按 `get_type()` 比较）；
`ZX_OBJ_TYPE_*`（`system/public/zircon/types.h`）。

**验证**
构建通过；两个镜像都启动到 shell 打完命令表。现有的 Process/Thread/Vma 的 `on_zero_handles()` 都是空操作，
所以 userboot 丢弃它们的 KernelHandle 时行为不变。

**故意没做**
没有照 Zircon 在基类里存一个 `m_type` 字段：`rc_type_tag()` 已经在，多存一份就是同一个事实的两个所有者。
