# 05 — 时钟与定时器子系统 (Time & Timer)

## 1. 概述

时钟子系统负责内核时间的维护和定时器管理。设计同时受到 Zircon 的 `ClockSource`/`ClockEventDevice` 模型和 Linux 的 `clocksource`/`clockevent` 架构的影响。

整体架构：

```
硬件层
├── TSC / HPET / PIT          ← 时钟源 (ClockSource)
└── LAPIC Timer / HPET Timer   ← 时钟事件设备 (ClockEventDevice)

抽象层
├── ClockSource               ← 单调递增的硬件计数器抽象
│   ├── Default               ←   软件 jiffies 回退
│   └── HpetClockSource       ←   HPET 主计数器
├── ClockEventDevice          ← 可编程的到期事件设备
│   ├── ApicTimerEvent        ←   LAPIC 定时器
│   └── HpetChannel           ←   HPET 比较器通道
├── Clock                     ← 基于 ClockSource 的累积时间
│   ├── SystemClock           ←   系统时钟 (CLOCK_REALTIME)
│   └── SteadyClock           ←   单调时钟 (CLOCK_MONOTONIC)
├── TickDevice (per-CPU)      ← 封装 ClockEventDevice
├── TickSched (per-CPU)       ← 软件 tick 模拟 (HP 模式)
├── Timer / TimerQueue        ← 内核定时器
└── TimerScheduler (per-CPU)  ← 定时器调度器
```

## 2. 时钟源 (ClockSource)

### 2.1 设计

```cpp
class ClockSource {
    Vtable {            // 虚函数表 (非虚函数，手动函数指针)
        read, enable, disable, suspend, resume
    };
    u32 priority;       // 优先级 (越高越好)
    u64 freq_khz;       // 频率 (kHz)
    Ratio numerator, denominator;  // ticks → ns 转换比例
};
```

**注册与选择**：
- `enroll()` — 注册到全局列表 `g_registered_sources`（按优先级排序）
- `enqueue_source()` — 互斥锁保护下插入
- `select_best_source()` — 选择最佳源：优先 HP 时钟 + oneshot 模式

### 2.2 Default 时钟源 (软件 jiffies)

```cpp
class ClockSource::Default {  // CPU-local 单例
    atomic<Ticks> s_ticks;
    Ticks s_last_ticks;
    Ticks s_next_period;

    static void advance_ticks(Ticks);   // 在 tick 中断中调用
    static void advance_time(Duration); // 时间增量转 tick
};
```

当没有硬件时钟源可用时使用，由 PIT/HPET 周期性中断驱动。

### 2.3 与 Linux/Zircon 对比

| 概念 | Linux | Zircon | 本项目 |
|------|-------|--------|--------|
| 时钟源 | `struct clocksource` (rating) | `platform_clock` (vtable) | `ClockSource` (vtable+priority) |
| 默认源 | jiffies | monotonic | `ClockSource::Default` |
| 最佳选择 | `clocksource_select()` | Platform 层 | `select_best_source()` |
| tick→ns | mult+shift | 硬编码比例 | Ratio numerator/denominator |

## 3. 时钟事件设备 (ClockEventDevice)

```cpp
class ClockEventDevice {
    Mode mode;              // Detached, Shutdown, Periodic, Oneshot, OneshotStopped
    Features features;      // Periodic, Oneshot, TimePoint

    Vtable {
        set_next_event_tick,    // 编程: N ticks 后到期
        set_next_event_time,    // 编程: 绝对时间点到期
        set_mode_periodic,      // 切换到周期模式
        set_mode_oneshot,       // 切换到单次模式
        stop_oneshot, resume, suspend, shutdown, broadcast
    };
    Handler handler;        // 事件回调: Fn<void(Self*)>
    i32 irqnum;             // 关联的 IRQ 号
};
```

### 3.1 注册

- `entroll()` — 验证参数，配置 scaler (tick↔ns 转换)，加入全局列表 `s_gced`
- 注册后自动通知 `TickDevice::Current::check_new_device()`

### 3.2 模式切换

```
Detached → Shutdown → Periodic
                   → Oneshot → OneshotStopped
```

### 3.3 事件编程

```cpp
program_event(expiry, min_delta, max_delta):
  1. 检查 Shutdown → 返回
  2. 计算 delta_ticks = (expiry - now) / scaler
  3. clamp(delta_ticks, min_delta, max_delta)
  4. vtable.set_next_event_tick(delta_ticks)
```

## 4. 时间累积 (Clock)

```cpp
class Clock {
    ClockSource* m_clocksource;
    TimePoint m_time_base;   // 基准时间
    Duration m_time_shift;   // 累积偏移
    AtomicU8 m_seq;          // seqlock 风格的并发读保护

    TimePoint now() {
        update_time();       // 读周期计数器 → 计算 delta → 累加
        return m_time_base + m_time_shift;
    }
};
```

两个全局单例：
- **SteadyClock** — 单调递增，不受 NTP 调整影响
- **SystemClock** — 可调整的墙上时钟，支持 `notify(ClockSource*)` 热切换时钟源

## 5. Tick 设备 (TickDevice)

每个 CPU 拥有一个 `TickDevice`，封装 `ClockEventDevice` 并管理周期性/单次模式：

```cpp
class TickDevice {
    ClockEventDevice* m_clkdev;  // 当前绑定的硬件设备
    Mode m_mode;                  // Periodic / Oneshot

    void program_event(expiry);  // 编程下一个到期时间
    void rebind_device(new_dev); // 热切换设备 (转移 handler, 重新编程)
    void handle_periodic_event(); // 周期事件: jiffy++，如果设备在 oneshot 模式则重编程
};
```

`rebind_device()` 的流程：转移事件处理器 → 处理 CPU 亲和性 → 重新编程模式 → 关闭旧设备 → 分配新设备。

### 与 Linux 对比

| 概念 | Linux | 本项目 |
|------|-------|--------|
| 每 CPU tick | `tick_cpu_device` + `tick_handle_periodic` | `TickDevice::Current` |
| 设备切换 | `tick_check_new_device` | `rebind_device()` |
| 广播 | `tick_broadcast` | `ClockEventDevice::broadcast()` |

## 6. TickSched — 高精度 Tick 模拟

当硬件仅支持 oneshot 模式时，`TickSched` 使用内核 `Timer` 模拟周期性 tick：

```cpp
class TickSched {
    WorkMode mode;       // Shutdown, Legacy, HpTick
    Timer m_timer;       // 内核定时器 (非硬件)

    void emulate(WorkMode);
    // HpTick: 设置定时器 deadline → 到期时调用:
    //   Scheduler::Current::timer_tick(now)
    //   TimerScheduler::Current::run_local_queues()
    //   重设定时器向前 kNsecPerTick
};
```

## 7. 内核定时器 (Timer)

```cpp
class Deadline {
    TimePoint when;       // 目标到期时间
    Duration slack;       // 容忍窗口 (earliest..latest)
};

class Timer {
    ClockId clockid;
    Deadline deadline;
    Fn<HandlerResult(Timer*)> handler;  // Handled / Restart
    AtomicU32 active_cpu;
    AtomicBool cancelled;

    // 激活：设置 deadline + handler → 加入 TimerScheduler
    void activate(Deadline, Handler);

    // 取消：仅设置 atomic 标志 (惰性出队)
    void cancel();

    // 前移：修改 deadline 但不重排队
    void forward(Duration);
};
```

**侵入式存储**：Timer 是侵入式 set 节点，存储在 `TimerQueue` 的 `m_active_timers` 中。

**惰性取消**：`cancel()` 仅设置 `m_cancelled` 原子标志，定时器到期时 handler 检查标志并跳过。

## 8. 定时器调度 (TimerQueue + TimerScheduler)

```
TimerScheduler (per-CPU)
├── TimerQueue[Monotonic]   ← ClockId::Monotonic 的定时器
├── TimerQueue[Boottime]    ← ClockId::Boottime 的定时器
└── ...
```

```cpp
class TimerQueue {
    ClockId clockid;
    ClockSource* clock_source;
    Deadline m_next_timer_deadline;      // 最近的到期时间
    IntrusiveSet<Timer> m_active_timers; // 按 deadline 排序

    void insert_timer(Timer& timer);     // 入队 + 更新 next_deadline
    void remove_timer(Timer& timer);     // 桩代码
    void run_expired_timers(CpuNum);     // 遍历到期定时器，调用 handler
    void update_next_deadline();         // 重新计算 → 编程 TickDevice
};
```

**到期处理** (`run_expired_timers`)：
```
while (队首 timer.deadline.latest <= now):
    timer = 出队
    if timer.cancelled: continue
    result = timer.handler()
    if result == Restart:
        timer.forward(period)
        重新入队
```

## 9. 配置

```cpp
HZ = 100 (默认)           // tick 频率
kNsecPerTick = 10_000_000  // 10ms per tick
```

可以通过 `OURS_CONFIG_HZ` 覆写。

## 10. 实现状态一览

| 组件 | 状态 | 备注 |
|------|------|------|
| ClockSource 注册/选择 | **完整实现** | 优先级排序 + 最佳源选择 |
| ClockSource::Default jiffies | **完整实现** | SpinLock 保护的 tick |
| Clock 时间累积 (seqlock) | **完整实现** | SystemClock + SteadyClock |
| ClockEventDevice 注册/配置 | **完整实现** | scaler + 模式机 |
| ClockEventDevice 模式切换 | **完整实现** | 全部五种模式 |
| TickDevice 热切换 | **完整实现** | 处理器转移 + 重编程 |
| TickDevice 周期处理 | **完整实现** | jiffy 前进 + oneshot 重编程 |
| TickSched HpTick 模式 | **完整实现** | 软件 Timer 模拟 |
| TickSched Legacy 模式 | **桩代码** | empty else 分支 |
| Timer::activate() | **部分实现** | 设置字段 OK，但调度器插入未完成 |
| Timer::cancel() | **部分实现** | atomic 标志 OK，但不出队 |
| Timer::forward() | **桩代码** | 空函数体 |
| TimerQueue::insert_timer | **完整实现** | |
| TimerQueue::remove_timer | **桩代码** | 空函数体 |
| TimerQueue::run_expired_timers | **完整实现** | 含 Restart 支持 |
| TimerScheduler::activate_timer | **桩代码** | 空函数体 — 定时器从不入队 |
| time::init() | **桩代码** | 空函数 |
| calculate_mult_shift() | **Bug** | 返回类型错误 (void 不应返回 pair) |

**已知 Bug**：
- `timer-queue.cpp` 字段命名与头文件不匹配 (`canary_` vs `m_canary` 等)，编译会失败
- `calculate_mult_shift()` 返回类型不匹配
- `TickDevice::rebind_device()` 使用旧设备而非新设备的 cpumask 设置亲和性

## 11. 设计要点与改进建议

### 11.1 当前设计优点
1. **清晰的双层抽象**：ClockSource（只读计数器）和 ClockEventDevice（可编程事件）分离，与 Linux/Zircon 一致
2. **Vtable 而非虚函数**：使用手动的函数指针表 (`gktl::VtablePacker`)，避免 vtable 开销
3. **Seqlock 并发读**：`Clock::now()` 使用 `AtomicU8 m_seq` 实现无锁读
4. **设备热切换**：`TickDevice::rebind_device()` 支持运行时切换时钟事件设备

### 11.2 可改进之处
1. **定时器入队链路断裂**：`TimerScheduler::activate_timer()` 为空，定时器创建后从不在调度器中排队
2. **RwLock 已声明但无实现**
3. **TimerQueue::remove_timer 未实现**，惰性取消依赖到期时检查 `cancelled` 标志
4. **TimerQueue 命名不匹配**导致编译失败
5. **无高精度超时**：当前最小超时粒度受限于 `kNsecPerTick` (10ms)

### 11.3 从 Linux 可借鉴的设计
- **Linux hrtimer**：高精度定时器基于红黑树，支持纳秒级超时
- **Linux timer wheel**：低精度定时器使用时间轮 (timer wheel)，O(1) 级联
- **Linux NO_HZ**：动态 tick，空闲时完全停止 tick
- **Linux clocksource watchdog**：时钟源监控和自动切换
- **Zircon timer_slack**：允许定时器合并到同一中断，减少唤醒次数
