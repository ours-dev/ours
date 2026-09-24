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

## 信号核心：电平触发的 attach、`update_state`、`cancel`，与 `object::wait_one`（2026-09-23）

**文件**
- `kernel/include/ours/sync/event.hpp` — 补构造函数
- `kernel/object/include/ours/object/dispatcher.hpp`、`dispatcher.cpp` — `attach_observer` 电平触发；`update_state`/`update_state_locked`/`cancel`/`signals()`
- `kernel/object/include/ours/object/handle_table.hpp`、`handle_table.cpp` — `attach_observer(value, ...)` 动词；`remove()` 与析构时 `cancel(handle)`
- `kernel/object/include/ours/object/handle.hpp` — `Handle::dispatcher()` 的非 const 重载
- `kernel/object/include/ours/object/wait.hpp`、`kernel/object/wait.cpp`（新）— `WaitSignalObserver` 与 `wait_one`

**症状**
没有任何办法等一个对象。`SignalObserver` 的骨架在，但：
- `attach_observer` 只是把观察者挂上链表——条件已经成立时，等待者要等的是一次「已经发生过的变化」，永远等不到；
- 没有任何代码改变过对象的信号，也就没有任何代码通知过观察者；
- 句柄被关时，经它挂着的等待无人收拾；
- `sync::Event` 没有构造函数，放在栈上时 `m_flags` 是垃圾，`AutoUnsignal` 位随机。

**原因**
这一层一直没有调用者，缺的东西都没被逼出来。

**改法**
照 Zircon 的信号机制补齐：

- `attach_observer`：**电平触发**。已经置位的条件当场 `on_match()`，观察者不入链。等可读的 channel 里
  已经有消息，不能去等「下一条」。
- `update_state(clear, set)`：先清后置，**集合没变就不通知**；变了才对匹配的观察者 `on_match()`。
  另有 `_locked` 版本给已经持锁的调用者（channel 入队与置 READABLE 要一步完成）。
- `cancel(handle)`：结束所有经这个句柄做的观察，各自 `on_cancel()`。`HandleTable::remove()` 在表锁下、
  别人看到句柄消失之前调用它，否则一个线程在另一个线程关掉的句柄上 `wait_one` 会永远睡下去。
- `HandleTable::attach_observer(value, ...)`：查句柄、查 `Wait` 权限、挂观察者在**同一把表锁**下完成，
  句柄不可能在「找到」与「开始观察」之间被关掉。锁序：表 → 对象。
- `object::wait_one`：挂上 → 在栈上的 `Event` 上睡 → 摘下 → 读当前信号，整个流程封在对象层，
  lib/syscall 只搬参数。P0 只接受 deadline 为 0（轮询）或 `INSTANT_INFINITE`，其余回 NotSupported——
  用户态还读不到时钟，算不出别的截止时间。

改前（`attach_observer`）：

```cpp
    observer->m_handle = handle;
    observer->m_triggering_signals = signals;
    m_observers.push_back(*observer);
```

改后：

```cpp
    observer->m_handle = handle;
    observer->m_triggering_signals = signals;

    auto const current = m_signals.load(std::memory_order_relaxed);
    if (!!(current & signals)) {
        observer->on_match(current);
        return Status::Ok;
    }

    m_observers.push_back(*observer);
```

**参照**
Zircon `Dispatcher::AddObserver()`/`RemoveObserver()`/`UpdateState()`/`Cancel()`
（`kernel/object/dispatcher.cc`）；`WaitSignalObserver`（`kernel/object/wait_signal_observer.cc`）；
`sys_object_wait_one()`（`kernel/lib/syscalls/object_wait.cc`）；`HandleTable::RemoveHandleLocked()`。

**验证**
构建通过；两个镜像都启动到 shell 打完命令表，`close` 两次的结果不变。等待机制本身没有内核自测，
按方案在第 7 步由用户态验证（`wait_one` 立即满足、关闭对端后得到 PEER_CLOSED）。

**故意没做**
- 带中间值的 deadline：要先有用户态可读的时钟（P1-E）。
- 被 kill 打断的等待把内部码带回用户态的问题，归 P1-A 的「sysret 前检查挂起信号」。

## Channel：共享锁的一对端点，只传字节（2026-09-23）

**文件**
- `kernel/object/include/ours/object/dispatcher.hpp` — 新增 `PeerHolder`；填好 `PeeredDispatcher`（构造、`init_peer`、`peer()`、`peer_lock()`，以及原先为空的 `on_zero_handles()`）
- `kernel/object/include/ours/object/message_packet.hpp`、`message_packet.cpp`（新）
- `kernel/object/include/ours/object/channel_dispatcher.hpp`、`channel_dispatcher.cpp`（`.cpp` 新）
- `kernel/object/BUILD.gn` — 新源文件；依赖 `//kernel/lib/user_copy:headers`

**症状**
`ChannelDispatcher` 只有一个 `create()` 声明，第三个出参类型写成了 `Signals *`，没有实现文件；
`PeeredDispatcher` 没有构造函数、没有 peer，它的 `on_zero_handles()` 是个 `final {}` 空函数。
有了 `wait_one` 也没有任何对象可以等。

**原因**
这是方案里 P0 切片的核心对象，此前没有调用者。

**改法**
- **`PeerHolder`**：两端共享一把 `sync::Mutex`。写入要在对端排队并置对端 READABLE，关闭要清对端
  WRITABLE——一把锁让这些都成为对端看不到半截的单步操作。P0 不像 Zircon 那样把它变成
  `Dispatcher::get_lock()` 的返回值（那要改基类）；各端的信号仍由各自的 `m_lock` 守护，锁序固定为
  holder → 端点 `m_lock`。
- **`PeeredDispatcher::on_zero_handles()`**：照 Zircon，把「断开这一对」集中在基类模板里——两端互相
  持有 `Arc`，在 holder 锁下**两个方向都断**，再通知幸存的一端 `on_peer_zero_handles_locked()`。
  对端的 `Arc` 在锁内移出、锁外释放，若是最后一个引用，析构发生在锁外。
- **`MessagePacket`**：在取锁**之前**把用户字节直接拷进一块 kmalloc 缓冲（≤ `CHANNEL_MAX_MSG_BYTES`），
  读出时先出队、放锁、再由调用者拷出。零字节消息不分配缓冲。
- **读写语义**（照 Zircon `ChannelDispatcher`）：
  - 写：对端已关回 `PeerClosed`；对端队列满（`CHANNEL_MAX_PENDING_MESSAGES = 64`）回 `NoResource`，
    **不清 WRITABLE**（决策 D11：WRITABLE 只表示对端存活）。
  - 读：队空且对端存活回 `ShouldWait`，队空且对端已关回 `PeerClosed`——对端关闭前写下的消息仍先交付；
    缓冲不够回 `BufferTooSmall` 并报出长度，消息留在队首；读空后清 READABLE。
- 这一端未读的消息留到对象析构时再释放——那时已不持有任何锁，也没有别人能碰到队列。
- `create()` 的第三个出参改为 `Rights *`：工厂在对象旁边报告的是给它们的权限，这里其他 `create()` 都是这样。

改前：

```cpp
    static auto create(KernelHandle<Self> *handle0, KernelHandle<Self> *handle1, Signals *signals)
        -> Status;
    ...
    auto on_zero_handles() -> void final {}
```

改后（节选）：

```cpp
    auto on_zero_handles() -> void final {
        ustl::Arc<Derived> peer;
        {
            Guard guard{m_holder->lock()};
            peer = ustl::move(m_peer);
            static_cast<Derived *>(this)->on_zero_handles_locked();
            if (peer) {
                static_cast<Self &>(*peer).m_peer = ustl::Arc<Derived>();
                peer->on_peer_zero_handles_locked();
            }
        }
    }
```

**参照**
Zircon `PeerHolder`、`PeeredDispatcher::on_zero_handles()`/`InitPeer()`（`kernel/object/include/object/dispatcher.h`）；
`ChannelDispatcher::Create()`/`Write()`/`Read()`/`OnPeerZeroHandlesLocked()`（`kernel/object/channel_dispatcher.cc`）；
`MessagePacket`（`kernel/object/message_packet.cc`）。

**验证**
构建通过；两个镜像都启动到 shell 打完命令表。channel 此时还没有调用者，读写语义在第 7 步由用户态的
自环测试验证。

**故意没做**
- 消息里带句柄、`channel_call`、64 KiB 消息：P1。
- 把共享锁并进 `Dispatcher::get_lock()`：等 EventPair/Socket 出现时再做（方案 2.4）。

## 句柄 arena 的两处边界：先占槽再取引用；generation 永不为 0（2026-09-23）

**文件**
- `kernel/object/handle.cpp`、`include/ours/object/handle.hpp` — `reserve_slot()` 从 `emplace()` 里拆出；`make(KernelHandle)` 先占槽再 `release()`；`reserve()` 回绕时跳过 generation 0；`lookup()` 拒绝 generation 0

**症状**
对抗式审查发现，两处都潜伏：
1. arena 满时，`make(KernelHandle)` 已经 `release()` 了引用，`reserve()` 失败后对象被悄悄丢掉——没人调用
   它的 `on_zero_handles()`。一个 channel 端在这里被丢掉，它的对端永远等不到 PEER_CLOSED。
2. 空槽记录的「当前 base value」是 0，而下标 0、generation 0 的 base value 也是 0。generation 从 1 数起，
   但回绕后会到 0；此外用户值 `0b11` 解码出的正是 0，于是 `lookup(0)` 会把一个空槽的存储当成句柄交出去
   （第 5 道关最终会挡住，但那是在读一块没有对象的内存）。

**原因**
1. 引用转移和占槽的顺序反了。Zircon `Handle::Make()` 先从 arena 取槽，取到了才从 KernelHandle 拿走引用。
2. 「0 表示没有句柄」这个约定没有被 generation 的取值范围守住。

**改法**
改前：

```cpp
auto Handle::make(KernelHandle<Dispatcher> kernel_handle, Rights const rights) -> ustl::Box<Self> {
    auto dispatcher = kernel_handle.release();
    if (!dispatcher) {
        return ustl::Box<Self>();
    }
    return emplace(ustl::move(dispatcher), rights);
}
```

改后：

```cpp
auto Handle::make(KernelHandle<Dispatcher> kernel_handle, Rights const rights) -> ustl::Box<Self> {
    if (!kernel_handle.dispatcher()) {
        return ustl::Box<Self>();
    }
    auto const base_value = reserve_slot();
    if (!base_value) {
        return ustl::Box<Self>();
    }
    return emplace(*base_value, kernel_handle.release(), rights);
}
```

槽占不到时，KernelHandle 仍握着对象，析构时照常通知 `on_zero_handles()`。

generation：`reserve()` 回绕到 0 时改用 1；`lookup()` 先拒绝 generation 为 0 的 base value，
「0 = 空槽」于是不会和任何能走到比较那一步的查询相等。

**参照**
Zircon `Handle::Make()`（`kernel/object/handle.cc`：`GetNewBaseValue()` 成功后才 `kernel_handle.release()`）。

**验证**
构建通过；两个镜像启动日志不变（`'shell' starts with handle 0x100003`，close 两次得 Ok / BadHandle）。
arena 耗尽和 generation 回绕都没有实测——前者要 4096 个在用句柄，后者要同一槽复用 4095 次。

**故意没做**
无。

## 队列满时被拒的消息在 channel 锁外释放（2026-09-23）

**文件**
- `kernel/object/channel_dispatcher.cpp`、`include/ours/object/channel_dispatcher.hpp` — `write_self_locked()` 改收 `ustl::Box<MessagePacket> &`，只在成功时 `into_raw()`

**症状**
潜伏：对端队列满（`NoResource`）时，被拒的 MessagePacket 作为 `write_self_locked()` 的按值参数在函数
返回时析构——此时 holder 锁还拿着，`kfree` 发生在锁内。违反 `message_packet.hpp` 写明的「在锁外创建、
在锁外释放」。

**原因**
所有权通过按值参数传进了持锁的函数，失败路径上它在那里死掉。

**改法**
改前：

```cpp
    return peer->write_self_locked(ustl::move(message));
...
auto ChannelDispatcher::write_self_locked(ustl::Box<MessagePacket> message) -> Status {
```

改后：

```cpp
    return peer->write_self_locked(message);
...
auto ChannelDispatcher::write_self_locked(ustl::Box<MessagePacket> &message) -> Status {
```

成功时照旧 `message.into_raw()` 入队；失败时包仍归 `write()` 的参数所有，参数的生命期在 `write()` 的局部
变量（锁的 guard）之后才结束，释放于是落在锁外。

**参照**
Zircon `ChannelDispatcher::Write()` 失败时由调用方的 `MessagePacketPtr` 在锁外释放。

**验证**
构建通过；两个镜像启动正常。队列满的路径需要 64 条未读消息，channel 的运行时测试还被 mm 的 AP 分配问题挡着，未实测。

**故意没做**
无。

## 删掉 `HandleTable` 里旧的每表分配器桩与那段 TODO（2026-09-23）

**文件**
- `kernel/object/include/ours/object/handle_table.hpp` — 删 `Allocator<T>`、`NUM_HANDLES_PER_BLOCK` 与描述旧 `Deque` 存储的 TODO；给 `m_koid` 补一行说明

**症状**
无行为差异：全树没有任何引用，`Allocator::allocate()` 只有声明没有定义。问题在于读者：同一个类里并存
两种「句柄存在哪」的说法，TODO 还写着「存储尚待设计」，而存储早已定了。

**原因**
旧想法是每张表按块分配自己的句柄（一个 `ustl::Deque`，原先建在 Boost.Container 上，从未通过类型检查）。
`docs/architecture/object/handle-lookup.md` 选了全系统一个 arena，理由正是句柄值要能用一次算术定位到槽位——
每表一套存储会破坏这个前提。实现落地之后，这个桩描述的方向已经被设计本身否掉。

**改法**
改前：

```cpp
    template <typename T>
    class Allocator {
        ...
        auto allocate() -> Handle *;
    };

    constexpr static auto NUM_HANDLES_PER_BLOCK = 8;

    // ... TODO(SmallHuaZi): the handle storage is still to be designed. ...

    KoId m_koid;
```

改后：

```cpp
    /// What every handle in this table carries as its table koid -- the fifth
    /// check, which keeps one process's values from resolving in another.
    KoId m_koid;
```

按主人的决定删除（桩一律不顺手删，这次是明确要求）。旧想法留在 git 历史里。

**参照**
`docs/architecture/object/handle-lookup.md`「决定：`Handle` 住在一块 arena 里」。

**验证**
构建通过；两个镜像启动正常，shell 关句柄两次仍得 Ok / BadHandle。

**故意没做**
无。

## 进 ring 3 前关中断：`ThreadDispatcher::trampoline` 补上 `local_irq_disable`（2026-09-23）

**文件**
- `kernel/object/thread_dispatcher.cpp` — `arch_enter_uspace()` 之前关中断
- `kernel/arch/x86/kernel/include/ours/arch/entry.hpp` — `arch_enter_uspace()` 断言中断已关，并注释为什么

**症状**
潜伏，当前没观察到。用户线程第一次进 ring 3 的最后几条指令里如果恰好来一个时钟中断，
中断处理会用用户态的 GS base 做每 CPU 访问：读到的是地址 0 附近的垃圾，写就是 #PF，
而 #PF 处理本身也要访问每 CPU 数据，结果是嵌套故障甚至三重错误。窗口只有几条指令宽，
所以启动测试几乎不可能撞上，但 CPU 多、时钟频率高时迟早会撞上。

**原因**
调度器的线程蹦床（`scheduler.cpp`）在调用线程入口前打开中断——新线程的上下文切换
不携带 RFLAGS，不这样做线程会一直 IF=0 地跑下去。所以到 `ThreadDispatcher::trampoline`
时中断是开着的。

`x86_enter_uspace` 的最后一段是：

```asm
    swapgs
    iretq
```

`swapgs` 之后、`iretq` 之前，GS base 已经是用户的值，而 CPU 仍在 ring 0。
这时来一个中断，CPU 压的帧里 CS 是内核代码段；中断入口桩按惯例看帧里的 CS
决定要不要 `swapgs`——"从内核来的，不用换"——于是处理函数带着用户 GS base 跑起来，
每一次 `CPU_LOCAL_ACCESS` 都落到错误的地址。这就是为什么所有内核都要求
"进用户态的最后一跳必须关中断"：换 GS 和降特权这两件事之间不能被打断，而它们又
没法用一条指令同时完成。

打开中断的正确时机是 `iretq` 本身：帧里的 RFLAGS 带着 IF=1，`iretq` 在切换到 ring 3
的同一条指令里把它装进去，中间没有任何可被打断的空隙。

**改法**
改前：

```cpp
    arch::IrqFrame frame{};
    arch_build_uspace_irqframe(&frame, task.entry, task.stack, task.arg1, task.arg2);

    arch_enter_uspace(&frame);
```

改后：

```cpp
    arch::IrqFrame frame{};
    arch_build_uspace_irqframe(&frame, task.entry, task.stack, task.arg1, task.arg2);

    arch::local_irq_disable();
    arch_enter_uspace(&frame);
```

并在 `arch_enter_uspace()` 里加 `DEBUG_ASSERT(arch::irqs_disabled())`，把这个前提
从"调用者记得"变成"忘了就在 dbg 镜像里断言失败"。

关在 `trampoline` 而不是 `x86_enter_uspace` 汇编里加一条 `cli`：后者能用，但把前提
藏进了汇编，调用者读 C++ 代码时看不到"这里中断必须是关的"。Zircon 也是 C++ 侧关、
arch 侧断言，照着它做，两边对照时一一对得上。

**参照**
- `Zircon ThreadDispatcher::StartRoutine()`：`arch_disable_ints()` 紧挨着 `arch_enter_uspace(&iframe)`。
- `Zircon arch_enter_uspace()`（`kernel/arch/x86/arch.cc`）：第一行 `DEBUG_ASSERT(arch_ints_disabled())`。
- Linux 的返回用户态路径（`syscall_exit_to_user_mode()` → `local_irq_disable_exit_to_user()`）同理：
  检查完待处理工作之后关中断，直到 `swapgs; iretq/sysretq` 都不再打开。

**验证**
构建通过；`kernel.dev.obi` 与 `kernel.dev.dbg_1.obi` 各启动一次，init↔shell 对话
与 shell 的通道自检照常输出；dbg_1 下 `DEBUG_ASSERT` 生效且没有触发。窗口本身太窄，
没有构造出"改前必现"的复现。

**故意没做**
没有顺手把 `x86_enter_uspace` 的其它寄存器处理（比如清掉未指定的通用寄存器、防止内核值
泄漏到用户态）一起改：帧是 `IrqFrame frame{}` 零初始化后填的，当前不泄漏；那是另一件事。
