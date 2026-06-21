# 03 — 同步子系统 (Synchronization)

## 1. 概述

同步子系统提供内核并发控制所需的所有原语。设计深受 Zircon `OwnedWaitQueue` / `WaitQueue` 体系的影响，引入了 Linux 的 PI (Priority Inheritance) 和 lockdep (Lock Dependency) 机制。

核心原语层次：

```
SpinLock         ← 最底层 (自旋等待，禁用抢占/IRQ)
  │
  ├── WaitQueue            ← 阻塞/唤醒基础原语，内部使用 SpinLock
  │     └── OwnedWaitQueue   ← 带优先级继承的等待队列
  │           └── Mutex      ← 基于 OWQ 的互斥锁
  ├── Event                ← 基于 WaitQueue 的事件通知
  ├── Semaphore            ← 基于 WaitQueue 的计数信号量 (桩代码)
  └── RwLock               ← 基于 WaitQueue/OWQ 的读写锁 (桩代码)
```

## 2. SpinLock

自旋锁是最底层原语，使用 CAS (Compare-And-Swap) 自旋等待，锁值编码持有者 `Thread*` 指针：

```cpp
template <Options>
class SpinLock {
    atomic<usize> m_lockval;  // 0 = 空闲, 非0 = Thread* | 状态位

    void acquire();           // CAS(0 → current_thread), arch::suspend() 等待
    bool try_acquire();       // 单次 CAS
    void release();           // store(0)
};
```

**Option 位掩码**：

| Option | 含义 |
|--------|------|
| `Monitored` | 启用 lockdep 监控 |
| `NoTrace` | 禁用锁追踪 |
| `DisablePreempt` | 获取时禁用抢占 |
| `DisableIrq` | 获取时禁用 IRQ |

**预定义别名**：
- `SpinLock` = `SpinLock<DisablePreempt>` (默认)
- `MonitoredSpinLock` = `SpinLock<Monitored>` (带 lockdep)
- `NoTraceSpinLock` = `SpinLock<NoTrace>`

**状态保存器** (`LOCKDEP_STATE_SAVER` 宏生成)：
- `SpinLockIrqSave` — 获取锁前保存 IRQ 状态，释放后恢复
- `SpinLockPreemptDisable` — 获取锁前禁用抢占，释放后恢复

### 与 Zircon/Linux 对比

| 概念 | Zircon | Linux | 本项目 |
|------|--------|-------|--------|
| 底层自旋锁 | `SpinLock` (arch_spin_lock) | `spinlock_t` (qspinlock) | `spinlock::SpinLock<>` |
| IRQ 安全版本 | `InterruptState` 手动保存 | `spin_lock_irqsave` | `SpinLockIrqSave` RAII |
| Lockdep 集成 | 有 (`LOCK_DEP`) | 有 (`lock_acquire/release`) | 有 (`Monitored` 选项) |
| 持有者追踪 | 无 | `lock-&gt;owner` | `Thread*` 编码在锁值中 |

## 3. WaitQueue — 等待队列

`WaitQueue` 是阻塞/唤醒系统的核心，受内部 `SpinLock` 保护：

```cpp
class WaitQueue {
    // 休眠：将线程放入等待队列，设置定时器，调用 Scheduler::park_thread
    Status park_thread(Thread*, Deadline, Signals, Interruptible);

    // 唤醒：从队列移除，调用 Scheduler::unpark_thread
    void   unpark_thread(Thread*, Status);

    // 通知操作 (桩代码)
    void notify_one(Status);
    void notify_all(Status);
};
```

**WaiterState** — 嵌入每个 `Thread` 中，记录线程在等待队列中的状态：
- `sleep()` — 调用 `Scheduler::Current::park_thread()` 进入 PARKED 状态
- `unsleep()` — 调用 `Scheduler::unpark_thread()` 恢复
- 支持 `Interruptible::Yes/No` 控制是否可被信号唤醒

**等待队列的等待者集合**：使用侵入式 multiset，按 deadline 排序。支持超时唤醒（`handle_timeout()` 回调）。

## 4. OwnedWaitQueue — 带优先级继承的等待队列

`OwnedWaitQueue` 继承 `WaitQueue`，添加所有者跟踪和 PI 图管理：

```
Thread A (owner)                    Thread B (waiter)
    │                                   │
    │  m_owned_wait_queues              │  m_waiter_state
    │       ↓                           │       ↓
    └──→ [OwnedWaitQueue] ←─────────────┘  (parked)
              │
              │ m_owner = Thread A
              │ m_ipv    = InheritedProfile (继承优先级)
```

**PI 图传播操作**：

| 操作 | 方向 | 含义 |
|------|------|------|
| `AddSingleEdge` | Thread → OWQ | 等待者加入队列 |
| `RemoveSingleEdge` | OWQ → Thread | 等待者出队或超时 |
| `BaseProfileOverrided` | upstream → downstream | 上游线程基础优先级变更 |

**关键方法**：

```cpp
// 设置所有者（建立 PI 边）
Status assign_owner(Thread* new_owner);

// 休眠并将所有权转移给 new_owner
Status park_and_assign_owner(Thread* curr, Thread* new_owner, Deadline, Interruptible);

// 锁住 PI 链 (所有者和等待者之间的锁链)
static void lock_pi_chain_common(Applicant* start, Thread* blocking_thread);

// 通知操作 (含 PI 传播)
template<typename UnparkThreads>
void notify_threads_locked(UnparkThreads, INotifyRequeueHook&, NotifyControl);
```

**实现状态**：
- `assign_owner`, `set_owner`, `reset_owner` — 完整实现
- `disown_all_queues()` — 完整实现（线程销毁时断开所有 OWQ）
- `begin_propagate` / `complete_propagate` — 骨架存在但逻辑不完整
- `lock_pi_chain_common` / `unlock_pi_chain_common` — 部分实现（PI 链遍历未完成）
- `lock_for_notify_operation` / `notify_threads_locked` — 声明但未定义
- `park_and_assign_owner` — 桩代码（返回 Ok 但无实际操作）

## 5. Mutex

互斥锁基于 `OwnedWaitQueue`，支持快速路径 CAS 和慢速路径自旋+阻塞：

```cpp
class Mutex {
    atomic<State> m_state;   // (Thread* | Contended | HandOff)
    OwnedWaitQueue m_waiters;

    void acquire(Duration spinning_duration);
    void release();
};
```

**状态编码** (复用 Thread* 的低位)：

```
m_state: [Thread* (高位)] [HandOff (bit1)] [Contended (bit0)]

Available  = 0
Contended  = BIT(0)        ← 有人自旋等待
HandOff    = BIT(1)        ← 释放时直接交给等待者
```

**获取流程**：

```
acquire()
├── acquire_common() fast path:
│   CAS: Available → (current_thread | Contended)
│   └── 成功 → 记录 CPU，返回
│
└── acquire_slowpath() slow path:
    ├── 自旋等待 (检查 contention + CPU 亲和性)
    ├── CPU 自旋期间调用 arch::halt()
    └── 自旋超时后: [阻塞在 OWQ 上 — 未完成]
```

**释放流程**：

```
release()
├── try_release():
│   CAS: (current_thread) → Available
│   └── 成功 → 无竞争，返回
│
└── release_slowpath():
    ├── 获取 OWQ 默认唤醒钩子
    ├── lock_for_notify_operation()
    ├── CAS: (current_thread | Contended) → (new_owner | Contended) 或 Available
    └── notify_threads_locked() → 交给下一个等待者
```

**CriticalMutex**：`Mutex` 的子类（protected 继承），用于关键区段的标记，无额外方法。

**实现状态**：
- 快速路径 — 完整实现
- 自旋等待 — 完整实现
- OWQ 阻塞路径 — 未完成（`acquire_slowpath` 自旋后不进入阻塞）
- 释放 + 移交路径 — 完整实现

## 6. Event

```cpp
class Event {
    Option<Status> m_result;    // 已信号化的结果
    Flags m_flags;               // AutoUnsignal 标志
    WaitQueue m_wq;              // 等待者队列
};
```

- `signal(Status)` — 唤醒等待者（AutoUnsignal 模式只唤醒一个）
- `unsignal()` — 重置事件
- `wait(Deadline, Interruptible, Signals)` — 阻塞等待

**实现状态**：`signal` 和 `wait_etc` 已实现，但 `wait_etc` 中的条件判断可能反转（仅在有信号时阻塞）。

## 7. 锁层次与 Lockdep

### 7.1 当前锁排序规则

```
pi_lock   →  rqlock     (调度器内部: 先锁线程 PI 锁，再锁运行队列锁)
SpinLock  →  Mutex      (中断上下文禁用 Mutex，自旋锁可在 IRQ 中使用)
```

### 7.2 Lockdep 集成

`system/lib/lockdep/` 提供了锁依赖追踪框架：

- **LockClass** — 每个锁类型/位置对应一个锁类，`add_dependency(prev, next)` 记录边
- **LockContext** — 每线程持有的锁集合，`acquire()` 时验证排序
- **LoopDetector** — 声明了但算法未实现
- **Guard\<LockType\>** — RAII 守卫

支持的锁标志：
- `IrqSafe` — 可在 IRQ 上下文中获取
- `Nestable` — 可嵌套获取 (不同顺序)
- `Leaf` — 叶锁 (不可在获取后获取其他锁)
- `MultiAcquire` — 允许多实例获取

### 7.3 TA (Thread Safety Annotation)

同步原语均标注了 Clang Thread Safety Analysis 属性：
- `TA_CAP("SpinLock")`, `TA_CAP("Mutex")`, `TA_CAP("Cirtical-Mutex")`
- `TA_REQUIRE`, `TA_ACQUIRE`, `TA_RELEASE`, `TA_GUARD`

## 8. CpuStopper

`CpuStopper` 利用 Event 实现跨 CPU 同步：

```cpp
class CpuStopper {
    struct Work {
        Event completion;
        // ...
    };
    // stop_cpu: 队列工作 → 设置完成事件 → worker 线程执行
};
```

工作线程通过 `Event::wait()` 等待任务，完成时通过 `Event::signal()` 通知发起者。

## 9. 实现状态一览

| 原语 | 状态 | 备注 |
|------|------|------|
| SpinLock (三种变体) | **完整实现** | CAS + arch::suspend() 自旋 |
| SpinLockIrqSave / PreemptDisable | **完整实现** | RAII 状态保存器 |
| WaitQueue park/unpark | **完整实现** | 含超时处理 |
| WaitQueue notify_one/all | **桩代码** | lock_for_notify 返回 none |
| OwnedWaitQueue 所有者管理 | **完整实现** | set/reset/disown |
| OwnedWaitQueue PI 传播 | **部分实现** | begin/complete_propagate 骨架存在 |
| OwnedWaitQueue 通知 | **桩代码** | notify_threads_locked 未定义 |
| Mutex 快速路径 | **完整实现** | CAS + 释放移交 |
| Mutex 慢速路径 | **部分实现** | 自旋完整，阻塞路径缺失 |
| Mutex 释放路径 | **完整实现** | 含 HandOff 协议 |
| Event | **部分实现** | 核心逻辑存在但等待条件可疑 |
| Semaphore | **桩代码** | wait 返回 Unimplemented |
| RwLock (PI / NoPi) | **桩代码** | 全部方法未实现 |
| LocalLock | **桩代码** | 仅有头文件声明 |
| CpuStopper | **完整实现** | 基于 Event |

## 10. 设计要点与改进建议

### 10.1 当前设计优点
1. **锁值编码持有者**：SpinLock 和 Mutex 都将持有者 Thread* 编码在原子值中，支持高效断言和死锁检测
2. **PI 图框架**：Thread ↔ OwnedWaitQueue 的双向 PI 边设计为完整的优先级继承奠定了基础
3. **HandOff 协议**：Mutex 释放时直接移交（`Contended | HandOff`），避免惊群效应
4. **可选的 Lockdep 集成**：通过模板 Option 控制，不影响性能关键路径

### 10.2 可改进之处
1. **PI 链锁定** (`lock_pi_chain_common`) 未完成，无法安全地进行并发 PI 传播
2. **Mutex 阻塞路径**缺失，慢速路径最终不会睡眠，可能导致活锁
3. **WaitQueue notify 未实现**，Event 的 signal 逻辑不完整
4. **Semaphore / RwLock 未实现**，仅存骨架
5. **Lockdep 循环检测**未实现，无法检测死锁
6. **无死锁检测超时**：SpinLock 无限自旋，无超时/panic 机制

### 10.3 从 Linux/Zircon 可借鉴的设计
- **Linux qspinlock**：使用 MCS 队列减少 cache line 争用，当前项目使用简单的 test-and-set
- **Linux mutex**：乐观自旋 (osq_lock) + MCS 等待队列，当前项目仅有简单自旋
- **Zircon BrwLock**：偏向读取的读写锁，比当前 RwLockPi 更实用
- **Linux lockdep**：完整的 BFS 循环检测 + 栈回溯报告，当前项目的 LoopDetector 为空
- **Linux RCU**：读写多场景下的无锁同步，当前项目无此机制
