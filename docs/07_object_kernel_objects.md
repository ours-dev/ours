# 07 — 内核对象系统 (Kernel Object System)

## 1. 概述

内核对象系统提供了用户态与内核对象交互的句柄（Handle）机制。这是 Zircon 内核最核心的设计模式之一，本项目几乎完全照搬了这套体系。

核心概念对应关系：

```
用户态                            内核
──────                          ──────
handle_value (u32)  ───────→   Handle → Dispatcher (引用计数)
                                   │
                                   ├── ProcessDispatcher
                                   ├── ThreadDispatcher
                                   ├── ChannelDispatcher
                                   ├── JobDispatcher
                                   ├── ResourceDispatcher
                                   ├── VmAreaDispatcher
                                   └── PagerDispatcher
```

## 2. 两层引用模型

对象生命周期由两层引用计数共同管理：

```
Handle Count (m_handle_count)
├── 每个 Handle::make()  → increase_handle_count()
├── 句柄关闭            → decrease_handle_count()
└── 到达 0               → on_zero_handles()

RefCounter (ustl::RefCounter)
├── 每个 Rc<Dispatcher>  → 增加引用
├── 最后一个 Rc 销毁    → delete Dispatcher
└── 内核内部引用 (如调度器持有 ThreadDispatcher)
```

**关键**：`on_zero_handles()` 不等同于对象销毁。即使所有用户态句柄关闭，内核内部引用（Rc）仍可保持对象存活。

## 3. Dispatcher — 内核对象基类

```cpp
class Dispatcher : public RefCounter<Dispatcher> {
    Canary m_canary;               // 对象完整性校验
    KoId m_koid;                    // 全局唯一 ID
    Mutex m_lock;                   // 保护观察者列表
    AtomicU32 m_handle_count;       // 句柄计数
    Atomic<Signal> m_signals;       // 信号状态
    SignalObserver::List m_observers; // 信号观察者链表

    KoId koid();
    Mutex& get_lock();
    void increase_handle_count();   // Relaxed atomic inc
    bool decrease_handle_count();   // AcqRel dec → true if 0
    virtual void on_zero_handles(); // 最后句柄关闭回调

    // 信号观察者管理
    Status attach_observer(SignalObserver*, const void* handle, Signals);
    Status detach_observer(SignalObserver*, Signals*);
    void   notify_observers_locked(Signals);
};
```

**CRTP 辅助类**：

```cpp
// 单端对象: Process, Thread, Job, Resource, VmArea, Pager
template<typename Derived, Rights DefaultRights, Signals ExtraSignals>
class SoloDispatcher : public Dispatcher { ... };

// 对等对象: Channel (两端各有一个 Dispatcher)
template<typename Derived, Rights DefaultRights, Signals ExtraSignals>
class PeeredDispatcher : public Dispatcher {
    // on_zero_handles() 标记为 final
    // 当一端关闭时通知另一端
};
```

## 4. Handle — 句柄

```cpp
class Handle {
    Rc<Dispatcher> m_dispatcher;   // 引用内核对象
    Rights m_rights;                // 授予的权限
    KoId m_koid;                    // 内核对象 ID
    u32 m_baseval;                  // 句柄值 (用户态引用)

    static Handle* make(KernelHandle<Dispatcher>, Rights);
    static Handle* clone(Handle*, Rights);  // 派生句柄 (可能降权)
};

template<typename T>
class KernelHandle<T> {  // 类型化的 Rc 包装
    Rc<T> m_dispatcher;

    T* dispatcher() const;
    void reset(Rc<U>);  // 替换 → 旧对象的 on_zero() 被调用
};
```

`KernelHandle<T>` 是仅可移动的类型化引用，`Handle` 是无类型的用户态句柄容器。

## 5. HandleTable — 句柄表

```cpp
class HandleTable : public RefCounter<HandleTable> {
    // 内部使用 Deque<Handle> 存储 (每块 8 个句柄)
    HandleList m_handles;  // kNumHandlesPerBlock = 8
    KoId m_koid;

    // 声明但未实现: get_handle, remove_handle, add_handle
    // 查询、添加、移除句柄的方法尚未定义
};
```

`HandleTable` 是每进程的对象，被 `ProcessDispatcher` 持有。

## 6. 信号系统 (Signal + SignalObserver)

```cpp
class SignalObserver {
    const void* m_handle;              // 被观察的句柄
    Signals m_triggering_signals;      // 关注的信号掩码

    virtual void on_match(Signals);    // 信号匹配 → 回调 (可能删除自身)
    virtual void on_cancel(Signals);   // 句柄关闭 → 回调
    virtual bool match_key(const void* port, u64 key);  // 端口键匹配
};
```

**信号通知流程**：
```
Dispatcher 状态变更
  → 设置 m_signals 位
  → notify_observers_locked(changed_signals):
      遍历 m_observers
      如果 observer.triggering_signals ∩ changed_signals ≠ ∅
        → 从链表移除 observer
        → observer.on_match(changed_signals)
```

## 7. 具体 Dispatcher 类型

### 7.1 ProcessDispatcher

```cpp
class ProcessDispatcher : public SoloDispatcher<...> {
    State m_state;                // Initial, Running, Dying, Dead
    ProcessDispatcher* m_parent;
    VmAspace* m_aspace;           // 地址空间
    List<ThreadDispatcher> m_thread_list;  // 本进程的线程

    static Status create(..., KernelHandle<Self>*, Rights*, Rc<VmArea>*, Rights*);
    static Status create_shared(...);  // 共享地址空间版本 (未实现)
    Status attach_thread(ThreadDispatcher&);   // 已实现
    Status detach_thread(ThreadDispatcher&);   // 桩: Unimplemented
};
```

### 7.2 ThreadDispatcher

```cpp
class ThreadDispatcher : public SoloDispatcher<...> {
    Flags m_flags;             // Active 标志
    State m_state;             // Running
    ParkedReason m_parked;     // 停车原因 (空枚举)
    task::Thread* m_kernel_thread;  // 底层内核线程
    Rc<ProcessDispatcher> m_process;
    TaskState m_user_entry;     // 用户入口点

    static Status spawn(Rc<Process>, name, KernelHandle<Self>* out);  // 已实现
    Status start(TaskState);     // 设置入口 → 绑定到进程 → 激活
    Status activate(bool suspend); // 将用户线程 Rc 绑定到内核线程
    Status resume();             // 桩: 仅 canary 检查
    Status suspend();            // 桩: 仅 canary 检查
    static i32 trampoline(Self*); // 构造 IrqFrame → 进入用户态
};
```

`trampoline()` 是最关键的方法 — 这是内核态到用户态的跳板：
```
ThreadDispatcher::trampoline(this)
  → 构造 arch::IrqFrame (设置用户态 SS, RSP, RFLAGS, CS, RIP)
  → arch_build_uspace_irqframe()
  → arch_enter_uspace()     [x86: swapgs + iretq]
```

### 7.3 ChannelDispatcher

```cpp
class ChannelDispatcher : public PeeredDispatcher<...> {
    // 仅声明 create，未实现
    static Status create(KernelHandle<Self>* handle0, KernelHandle<Self>* handle1, Signals*);
};
```

Channel 是唯一的 `PeeredDispatcher` 子类。创建时生成一对 `KernelHandle`，每个端点有独立的 handle。

### 7.4 JobDispatcher

仅声明 `static Rc<Self>& root_job()`（返回根 job 单例），未实现。

### 7.5 ResourceDispatcher

```cpp
class ResourceDispatcher : public SoloDispatcher<...> {
    Name m_name;             // ktl::Name<32>
    RsrcKind m_kind;         // 资源类型
    RsrcFlag m_flags;        // 访问标志
    PhysAddr m_base, m_size; // 物理地址范围
    List m_rsrclist;         // 所属的资源链表

    static Status create(..., KernelHandle<Self>*, Rights&, Kind, PhysAddr, PhysAddr, ...);
    // create() 实现中间做了基础验证，但未真正构造 Handle 对象
};
```

全局根资源列表：`g_system_resources` (ResourceDispatcher::Storage)。

### 7.6 其他 Dispatcher

- **VmAreaDispatcher**：完全空壳，无任何方法
- **PagerDispatcher**：声明了 `operate_range()`（未实现），用于用户态 pager

## 8. 实现状态一览

| 组件 | 状态 | 备注 |
|------|------|------|
| Dispatcher 基类 | **完整实现** | 引用计数 + 信号 + 观察者 |
| Handle / KernelHandle | **完整实现** | 类型化/非类型化包装 |
| HandleTable | **骨架** | 存储结构存在，操作方法未声明 |
| SignalObserver | **完整实现** | 侵入式链表节点 + 虚方法 |
| SoloDispatcher / PeeredDispatcher | **完整实现** | CRTP 辅助类 |
| ProcessDispatcher | **部分实现** | attach_thread 已实现，detach 和 create 未实现 |
| ThreadDispatcher | **大部分实现** | spawn/start/activate/trampoline 已实现，resume/suspend 桩代码 |
| ChannelDispatcher | **未实现** | 仅有 create 声明 |
| JobDispatcher | **未实现** | 仅有 root_job 声明 |
| ResourceDispatcher | **部分实现** | 验证逻辑存在，构造未完成 |
| VmAreaDispatcher | **空壳** | 完全无方法 |
| PagerDispatcher | **未实现** | 仅有 operate_range 声明 |

## 9. 设计要点与改进建议

### 9.1 当前设计优点
1. **两层引用模型**：Handle Count + RefCounter 分离用户态和内核态的引用生命周期
2. **权限控制**：每个 Handle 携带 Rights，`clone()` 可以降权
3. **CRTP 代码复用**：`SoloDispatcher`/`PeeredDispatcher` 消除了重复代码
4. **信号系统**：Signal + SignalObserver 提供了灵活的对象状态通知机制

### 9.2 可改进之处
1. **HandleTable 无操作方法**：`add_handle`、`remove_handle`、`get_handle` 均未声明
2. **ProcessDispatcher 创建未实现**：无法通过系统调用创建进程
3. **ChannelDispatcher 创建未实现**：IPC 通道无法使用
4. **resume/suspend 为桩代码**：线程挂起/恢复不工作
5. **缺少 Port/Wait 系统**：Zircon 的 `PortDispatcher` 提供 `object_wait_many` 功能，当前无此概念

### 9.3 从 Zircon 可借鉴的设计
- **Port / WaitQueue**：Zircon 的 `sys_object_wait_many` 通过 `PortDispatcher` 实现多对象等待
- **Futex**：Zircon 的 `FutexDispatcher` 提供高效的用户态同步
- **StreamDispatcher / SocketDispatcher**：用于数据流传输
- **EventPair**：与 Event 类似但有两个端点
- **Profile 系统**：Zircon 将调度优先级封装为 `ProfileDispatcher` 对象，权限可控
