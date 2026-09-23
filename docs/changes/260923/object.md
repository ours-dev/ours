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

## 句柄 arena、句柄表与 `ProcessDispatcher::current()`（2026-09-23）

**文件**
- `kernel/object/include/ours/object/handle.hpp`、`kernel/object/handle.cpp`（新）— 句柄 base value 的位布局常量；`Handle` 的实现；全系统唯一的 `HandleArena`（2^12 个槽，一次性备好）
- `kernel/object/include/ours/object/handle_table.hpp`、`kernel/object/handle_table.cpp`（新）— `value_of`/`add`/`remove`/`get_dispatcher<T>` 与五道关
- `kernel/object/include/ours/object/process_dispatcher.hpp`、`process_dispatcher.cpp` — 每个进程一张 `HandleTable`；`current()`
- `kernel/object/include/ours/object/thread_dispatcher.hpp`、`thread_dispatcher.cpp` — `current()`
- `kernel/include/ours/task/thread.hpp` — `user_thread_mut()`
- `kernel/lib/syscall/src/table.cpp` — vDSO 校验改经 `ProcessDispatcher::current()`，不再直接碰 `task::Thread`
- `system/public/ours/types.hpp` — `HANDLE_FIXED_BITS_MASK`

**症状**
`Handle::make/clone` 只有声明没有定义；`HandleTable` 没有任何存储（只有一段 TODO）；`table.cpp`
为了找「是谁陷入的」直接走 `task::Thread`，违反「系统调用只碰对象层」。在这三样补上之前，任何带
句柄参数的系统调用都写不出来。

**原因**
设计早已在 `docs/architecture/object/handle-lookup.md` 定下（arena + generation + 五道关），
只是没有实现。

**改法**
按那份文档实现，P0 的简化与方案 4.1 一致：

- **arena**：固定 2^12 个等宽槽，一次备好。内核地址空间不能按需补页，所以 Zircon 的「预留 2^18、
  用到再提交」要等显式提交的路径（P1）。耗尽时回空，记计数器并告警，**不 panic**——用户程序
  能触发的东西不能打死内核。
- 槽位上要比 Handle 活得久的信息都**放在槽外**：「上次用过的 base value」（下一代 generation 从它
  往上数）和「当前住在槽里的句柄的 base value」（查找不加锁、原子比较）。Zircon 的 GPArena 把
  base value 留在已释放的槽里再读回来；放在槽外是同样的信息，但不必去读一个析构过的对象。
- `~Handle()` 第一件事就是把自己从查找中撤下（`unpublish`），然后减句柄计数，最后一个句柄触发
  `on_zero_handles()`；槽位由 `ustl::Box` 的 `Disposer` 还回 arena。所以 `ustl::Box<Handle>` 就是
  HandleOwner，原来声明的返回类型不用改。
- **句柄表**：只用侵入式链表链住属于自己的句柄。`value_of()` 是 base value 和本表随机数的纯函数——
  出参句柄可以先算出值、拷给用户，成功后再装表，失败时没有任何东西曾经可见。
- 随机数预先左移 2 位再异或（照 Zircon `handle_table.cc`），否则会把两个固定位翻掉；P0 取 0。
- **锁**：每表一把 `sync::Mutex`；`remove()` 在锁内摘下，把 `Box` 交还调用者，销毁发生在锁外。
- `ProcessDispatcher::current()` / `ThreadDispatcher::current()`：对象层回答「谁在问」，lib/syscall
  从此不必碰 `task::Thread`。`task::Thread::user_thread()` 是只读的，而 `ustl::Rc` 会把 const 传到
  被指对象上，所以另加 `user_thread_mut()` 返回可变的裸指针。

解析的五道关（`HandleTable::lookup_locked`）：

```cpp
    if ((value & HANDLE_FIXED_BITS_MASK) != HANDLE_FIXED_BITS_MASK) {   // 1
        return nullptr;
    }
    auto const base_value = (value ^ m_random) >> HANDLE_RESERVED_BITS; // 2
    auto *const handle = Handle::from_base_value(base_value);           // 3, 4
    if (handle == nullptr) {
        return nullptr;
    }
    if (handle->table_koid() != m_koid) {                               // 5
        return nullptr;
    }
```

改前（`table.cpp`）：

```cpp
        auto const *const thread = task::Thread::Current::get();
        ...
        return user_thread->process().vdso_contains(frame->ip);
```

改后：

```cpp
        auto const *const process = object::ProcessDispatcher::current();
        if (process == nullptr) {
            return false;
        }
        return process->vdso_contains(frame->ip);
```

**参照**
Zircon `Handle::Make()`/`Handle::Delete()`、`NewHandleValue()`（`kernel/object/handle.cc`）；
`HandleTable::MapHandleToValue()`/`GetHandleLocked()`/`GetDispatcherWithRights()`（`handle_table.cc`）；
`ThreadDispatcher::GetCurrent()`/`ProcessDispatcher::GetCurrent()`。

**验证**
构建通过；两个镜像都启动到 shell 打完命令表——shell 的每一次系统调用都经过新的
`ProcessDispatcher::current()`。句柄的创建与解析本身要等第 5 步由用户态验证（关一个真句柄回 Ok，
再关一次回 BadHandle）。

**故意没做**
- 第 4 道关的**常数时间**比较（Zircon `conditional_select_nospec_eq`）：代码里留了 TODO(P1)。
- `HandleTable` 里旧设计留下的 `Allocator` 模板和 `NUM_HANDLES_PER_BLOCK` 原样保留，只在那段 TODO 前
  补了一句「存储问题已由 handle-lookup.md 定下」。
- 进程死亡时清空句柄表：`~HandleTable()` 已经会逐个关闭，但 P1 之前进程不会死。
