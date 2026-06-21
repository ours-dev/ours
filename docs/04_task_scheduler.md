# 04 — 任务与调度器子系统 (Task & Scheduler)

## 1. 概述

任务子系统管理内核线程和调度。调度器采用 **EEVDF (Earliest Eligible Virtual Deadline First)** 算法 —— Linux 6.6+ 中取代 CFS 的新一代完全公平调度算法。

整体架构：

```
Thread (任务抽象)
├── ArchThread            ← 架构相关的寄存器上下文
├── sched::Context        ← 调度实体 (类似 Linux sched_entity)
│   ├── Profile           ← 三层优先级 (base / effective / inherited)
│   ├── CpuAffinity       ← CPU 亲和性 (hard / soft)
│   ├── RunQueueState     ← 运行队列状态 (active / inactive)
│   ├── SchedParams       ← 调度参数 (vruntime, vdeadline, vlag)
│   └── Timeslice         ← 时间片管理
├── PreemptionState       ← 抢占状态跟踪
├── WaiterState           ← 等待状态 (嵌入 WaitQueue 使用)
└── MemoryAllocationState ← 内存分配策略状态

Scheduler (每 CPU 调度器实例)
├── Runtime (运行队列元数据)
│   ├── curr/prev/next_thread
│   ├── total_weight / num_queued
│   └── Driver (调度时钟 + 抢占定时器)
└── std::span<Schedule*> m_schedules  ← 调度类实例数组 (Fair, ...)
```

## 2. Thread — 线程抽象

### 2.1 线程状态机

```
Initial ──→ Ready ──→ Running ──→ Dead
              ↑          │
              │          ├──→ Parked ──→ Ready
              │          ├──→ Sleeping ──→ Ready
              │          └──→ Suspend ──→ Ready
              │                           
              └──────── 被唤醒 ────────────┘
```

`Special = Parked | Sleeping | Suspend` — 这些状态都意味着线程不在运行队列中。

### 2.2 线程生命周期

```cpp
// 创建
Thread::spawn(name, routine, priority) → Thread*
Thread::spawn_etc(koId, name, routine, profile, trampoline, user_thread) → Status

// 控制
thread->resume()    // READY → 入队 → 唤醒
thread->suspend()   // 设置 Suspend 标志 → 如有信号则 kill
thread->kill()      // 设置信号 → 唤醒
thread->join(retcode, deadline)   // 等待线程退出
thread->detach()    // 分离，退出时自动回收

// 调度
Thread::Current::yield()          // 主动让出 CPU
Thread::Current::yield_to(next)   // 让给指定线程
Thread::Current::preempt()        // 被抢占（中断返回路径）
Thread::Current::reschedule()     // 重新调度（非原子上下文）
Thread::Current::become_idle()    // 成为空闲线程 (永不返回)
```

### 2.3 用户线程绑定

```cpp
// 内核线程可以绑定一个用户态 ThreadDispatcher
thread->bind_user_thread(Rc<ThreadDispatcher>);

// 用户态入口 (由 ThreadDispatcher::trampoline 调用)
// → x86_enter_uspace() → iretq 进入 ring 3
```

## 3. 调度器架构

### 3.1 调度类接口 (Schedule)

```cpp
class Schedule {  // 抽象调度类
    virtual Status enqueue_thread(Runtime&, Thread*, EnqueueControl);
    virtual Status dequeue_thread(Runtime&, Thread*, DequeueControl);
    virtual Thread* evaluate_next_thread(Runtime&, Thread* curr);
    virtual void   on_thread_tick(Runtime&, Thread*, bool queued);
    virtual Status override_thread_base_profile(Runtime&, Thread*, BaseProfile const&);
    virtual void   put_prev_thread(Runtime&, Thread*);
    virtual void   set_next_thread(Runtime&, Thread*);
    // ...
};
```

每个 CPU 有一个 `Scheduler` 实例，持有 `std::span<Schedule*>` 数组。
调度类通过链接器段注册 (`OURS_DEFINE_SCHEDULE` / `OURS_DECLARE_SCHEDULE`)：

```
__schedules_start → [Fair instance] → ... → __schedules_end
```

当前只有 `Fair` 一个调度类。

### 3.2 EEVDF 调度算法 (Fair)

EEVDF 是 Linux 6.6 引入的调度算法，改进 CFS：

```
CFS (Linux < 6.6):      按 vruntime 排序，选择最小 vruntime 任务
EEVDF (Linux ≥ 6.6):    按 deadline 排序，选择最早 deadline 的 eligible 任务

deadline = 当前时间 + (时间片 × 权重比例)
eligible = lag >= 0  (即 vruntime 没有落后太多)
```

**RunQueue** (Fair 内部)：
- 侵入式 `MultiSet<Context>` 按 `deadline` 排序
- 维护 `min_vruntime`, `total_vruntime`, `weight_sum`
- `avg_vruntime()` 计算加权平均 vruntime

**核心操作**：

```
enqueue_thread():
  1. 计算 lag (滞后量)
  2. 设置 vlag
  3. 插入 runqueue

evaluate_next_thread():
  1. 遍历 runqueue
  2. 找到第一个 eligible (lag >= 0) 的任务
  3. 返回其 Thread*
  4. 无 eligible → 返回 idle thread

set_next_thread():
  1. 从 runqueue 中出队
  2. 设置下一周期的 deadline
  3. 可选：重置 NOHZ 抢占定时器

on_thread_tick():
  1. 更新 runtime, vruntime
  2. 检查 deadline 是否到期
  3. 如到期 → 标记抢占 pending
```

**Delay Dequeue**：如果任务不 eligible 且正在睡眠，可能留在 runqueue 上不做 dequeue/enqueue 开销（需配置 `SCHED_FEATURE_DELAY_DEQUEUE`）。

**vlag 调整**：每个任务维护一个虚拟滞后量 (`vlag`)，用于在 EEVDF eligibility 检查中补偿不公平：
- `adjust_vlag()` — 确保 vlag 在合理范围
- `update_vlag()` — 根据实际运行时间更新 vlag

### 3.3 优先级与权重

```cpp
// 32 级优先级 → 权重映射 (几何级数)
LOWEST_PRIORITY  = 0   → weight = 121
DEFAULT_PRIORITY = 15  → weight = 1024
HIGHEST_PRIORITY = 31  → weight = 65536
IDLE_PRIORITY    = 7

priority_to_weight(usize priority) → Weight  // 查表 O(1)
```

### 3.4 三层 Profile 模型

借鉴 Linux `sched_attr` 设计：

```cpp
BaseProfile {         // 用户指定的调度参数
    SchedWeight weight;
    Discipline discipline;  // Normal, Deadline
};

EffectiveProfile {    // 实际调度参数 (考虑 PI)
    Weight weight;
    Duration initial_timeslice_ns;
    // recompute: effective = max(base, inherited)
};

InheritedProfile {    // 从 PI 链继承的优先级
    SchedWeight inherited_weight;
};

Profile = BaseProfile + EffectiveProfile + InheritedProfile
```

## 4. 调度流程

### 4.1 核心调度路径 (reschedule_common)

```
reschedule_common(curr, mode):
  1. 断言 pi_lock 持有 + IRQ 禁用 + 抢占禁用
  2. 设置 TransientState = Rescheduling
  3. 获取 rqlock
  4. 更新调度时钟
  5. flush 远程 pending 抢占
  6. 如果 Idle 模式且无可运行线程 → 返回
  7. 如果非自愿 (Preempt/Sleeping) → dequeue_thread(curr)
  8. evaluate_next_thread(curr):
     a. 先检查 yield_to 目标
     b. 再问当前调度类的 evaluate_next_thread
     c. 尝试 steal_work_from(其他 CPU)  ← 桩代码
     d. 回退到 curr 或 idle
  9. switch_context(curr, next)
```

### 4.2 上下文切换

```
switch_context(curr, next):
  1. set prev_thread / curr_thread
  2. 如果地址空间不同 → VmAspace::switch_context()
  3. 释放 rqlock
  4. Thread::switch_thread(curr, next)
     └── ArchThread::switch_thread()
         ├── 更新 TSS.sp0
         ├── 交换 FS/GS base
         └── x86_switch_context() [汇编]
             ├── push callee-saved regs
             ├── 保存 RSP → *prev
             ├── 加载 RSP ← next
             ├── pop callee-saved regs
             └── ret (跳转到 next 的 RIP)
  5. finish_switch_context():
     ├── 释放 prev 的 pi_lock
     └── 如果 next 是 idle → 设置状态为 Idling
```

### 4.3 抢占模型

```cpp
class PreemptionState {  // 打包到单个 32-bit 位域
    u8  preemption_disabled_count;       // 抢占禁用计数 (>0 = 禁用)
    u8  eager_resched_disabled_count;    // 急切重调度禁用计数
    u4  irq_nested_count;                // IRQ 嵌套深度
    u4  nmi_nested_count;                // NMI 嵌套深度
    u1  no_resched_preempt;             // 禁止抢占标志
    u1  no_resched_eager;               // 禁止急切重调度标志
    u1  timeslice_extension;            // 时间片已扩展
};
```

**抢占触发路径**：
- `flush_pending_local()` — 检查当前 CPU 的 pending 位，如设置则 `Thread::Current::preempt()`
- `flush_pending_remote()` — 对 pending CPU 发送 `arch_mp_reschedule()` IPI

**RAII 辅助**：
- `AutoPreemptDisabler` — 禁用/恢复抢占
- `AutoEagerReschedDisabler` — 禁用/恢复急切重调度
- `AutoMigrateDisabler` — 桩代码（构造/析构为空）

## 5. 空闲线程

```cpp
class IdleThread {  // 每 CPU 一个
    StateMachine { kIdle, kRunning };  // 目标状态
    ArchIdleThread m_arch;             // 架构特定空闲 (x86: HLT/MWAIT)
    LazyInit<Thread> m_thread;         // 延迟初始化的内核线程

    void run() {
        while (true) {
            if (target == kIdle) {
                m_arch.idle();          // HLT 等待中断
                accumulate_idle_time();
            } else {
                Thread::Current::reschedule();  // 切换到下一个任务
            }
        }
    }
};
```

## 6. DPC (Deferred Procedure Call)

```cpp
class DpcRunner::Queue {
    // 在 Thread::exit() 时用于延迟销毁
    i32 do_work();              // 事件循环
    void enqueue(Dpc& dpc);    // 入队
};
```

## 7. IPI 服务

```cpp
class IpiService {
    // 向目标 CPU 发送重调度 IPI
    static void send_resched(CpuNum cpu);
    static void send_resched(CpuMask mask);
};
```

`send_resched` 检查目标 CPU 是否活跃（通过 `Scheduler::peek_active_mask()`），然后调用 `arch_mp_reschedule()`。

## 8. CPU 迁移和负载均衡

- **select_target_cpu**：选择 `total_expected_runtime` 最小的 CPU（在亲和性掩码内）
- **steal_work_from**：桩代码（返回 nullptr，无负载均衡）
- **Migrator::move_queued_thread**：跨 CPU 迁移线程的基础原语（已实现）
- **process_migrating_threads**：处理迁移列表中的线程

## 9. Priority Inheritance (PI) 集成

调度器通过 `PiOperation` CRTP 模式处理 PI 图变更：

| PI 操作 | 含义 |
|---------|------|
| `JoinNodeToPiGraph` | 线程加入 PI 图 (成为 OWQ 的等待者) |
| `SplitNodeFromPiGraph` | 线程离开 PI 图 (被唤醒/超时) |
| `OnThreadBaseProfileOverrided` | 线程基础优先级被修改 |
| `OnUpstreamThreadBaseProfileOverrided` | 上游线程/OWQ 优先级变更传播 |

每个操作：
1. 如果线程当前在 CPU 上：dequeue → 重算 effective profile → 更新参数 → 重新 enqueue
2. 如果线程在 WaitQueue 中：调用 `wq->update_parked_thread_effective_profile()`

## 10. 实现状态一览

| 组件 | 状态 | 备注 |
|------|------|------|
| Thread 生命周期 | **完整实现** | spawn/resume/suspend/kill/join/exit |
| Thread 状态机 | **完整实现** | 7 个状态的完整转换 |
| 抢占状态跟踪 | **完整实现** | disable/enable/eager/pending/flush |
| 空闲线程 | **完整实现** | 每 CPU 的 HLT 循环 |
| EEVDF (Fair) 调度类 | **完整实现** | enqueue/dequeue/pick/tick |
| 上下文切换 | **完整实现** | switch_thread + trampoline |
| Unpark (唤醒) | **完整实现** | 单线程 + 批量 |
| Reschedule | **完整实现** | 本地 + IPI + eager/lazy 模式 |
| Sleep | **完整实现** | deadline + 超时 + 可中断 |
| CpuAffinity | **部分实现** | hard/soft 设置完整，迁移触发未完成 |
| select_target_cpu | **完整实现** | min total_expected_runtime 策略 |
| TimeSlice 扩展 / NOHZ | **实现** | set_next_thread 中的抢占定时器重置 |
| PI 图操作 | **完整实现** | PiOperation CRTP 四种操作 |
| Profile 重算 | **完整实现** | base→effective 重算 + PI 链传播 |
| steal_work_from | **桩代码** | 返回 nullptr |
| migrate_thread | **桩代码** | 仅有状态检查 |
| AutoMigrateDisabler | **桩代码** | 构造/析构为空 |
| HPTick 调度 | **桩代码** | reschedule_common 中逻辑块为空 |
| 多调度类支持 | **完整实现** | 链接器段注册 + span 迭代 |

## 11. 设计要点与改进建议

### 11.1 当前设计优点
1. **EEVDF 算法**：采用了 Linux 最新调度算法，非经典 CFS
2. **链接器段注册**：调度类通过 linker section 自动发现，易于扩展
3. **三层 Profile**：base/effective/inherited 分离，PI 集成自然
4. **严格锁序**：pi_lock → rqlock，TA_REQUIRE 注解强制

### 11.2 可改进之处
1. **无负载均衡**：`steal_work_from()` 为桩代码，多核系统空闲 CPU 无法获取工作
2. **无 CPU 迁移**：`migrate_thread()` 仅检查状态不执行迁移
3. **无 Deadline 调度类**：虽然定义了 `Discipline::Deadline`，但未实现对应的 `Schedule` 子类
4. **HPTick 未实现**：高精度调度 tick 的逻辑块为空
5. **无可配置调度域**：缺少类似 Linux cgroup/sched_domain 的分组调度

### 11.3 从 Linux/Zircon 可借鉴的设计
- **Linux load balancing**：`load_balance()` + `find_busiest_queue()`，使用 `cpu_load` 指标而非 `total_expected_runtime`
- **Linux deadline scheduler**：`SCHED_DEADLINE` 的 CBS (Constant Bandwidth Server) 算法
- **Linux sched_ext**：可扩展调度器 (BPF)，为未来调度策略实验提供接口
- **Zircon Scheduler**：使用更简单的优先级轮转 + deadline 继承，没有完全公平调度
- **Zircon DPC**：更完善的 DPC 队列 (高/中/低优先级)，当前项目仅有一个基础 Queue
