# Linux 定时器架构

本文描述 Linux 内核从硬件计数器、定时器中断到用户态 API 的完整链路。具体实现会随内核版本、架构、配置和虚拟化环境变化。

## 1. 总体模型

| 层次 | 核心职责 | 代表组件 |
| --- | --- | --- |
| 硬件 | 提供计数器或产生中断 | TSC、HPET、APIC timer、ARM Generic Timer、RTC |
| 时间基础 | 读取当前时间 | clocksource、timekeeping |
| 事件调度 | 编程下一次唤醒 | clock_event_device、tick、clockevents |
| 定时器抽象 | 管理超时和到期通知 | timer wheel、hrtimer、POSIX timer、timerfd |

clocksource 只负责读时间；clockevent device 负责产生未来事件；timer wheel 适合大量低精度内核超时；hrtimer 适合高精度 deadline。

## 2. 总体结构图

~~~mermaid
flowchart TD
    H[硬件计时资源]
    H1[TSC / ARM Generic Timer]
    H2[HPET / ACPI PM Timer]
    H3[Local APIC Timer / PIT]
    H4[RTC / Always-on Timer]
    CS[Clocksource\n读取硬件计数器]
    CE[Clockevent Device\n编程下一次中断]
    TK[Timekeeping\n维护内核时间]
    CLK[POSIX Clocks\nREALTIME / MONOTONIC / BOOTTIME]
    TICK[Tick Subsystem\n周期 tick / oneshot / broadcast]
    W[Timer Wheel\njiffies 级内核定时器]
    HR[HRTimer\n高精度定时器]
    K[内核等待 API\nschedule_timeout / delayed_work]
    U[用户态 API\nsleep / POSIX timer / timerfd]

    H1 --> CS
    H2 --> CS
    H3 --> CE
    H2 --> CE
    H4 --> U
    CS --> TK --> CLK
    CE --> TICK
    TICK --> W
    TICK --> HR
    W --> K
    HR --> K
    HR --> U
    TK --> U
~~~

## 3. 硬件计时资源

### TSC、HPET 与传统设备

x86 TSC 是快速递增的 CPU 计数器。现代 CPU 通常提供 invariant TSC；老硬件、未同步多核系统或虚拟机中的 TSC 可能不稳定。HPET 精度高但读取通常慢于 TSC。ACPI PM Timer 可靠但较慢，PIT 主要用于兼容和回退。

### Local APIC Timer 与 ARM Generic Timer

Local APIC timer 是常见的 per-CPU clockevent device，支持周期、oneshot 或 TSC-deadline 模式。ARM Generic Timer 提供系统计数器以及每 CPU 的物理、虚拟计时器。

### RTC

RTC 由电池维持，可跨重启保存墙上时间。它通常用于启动时初始化 CLOCK_REALTIME、低功耗唤醒和 suspend/resume，而不是高频运行时调度。

## 4. Clocksource：读取时间

struct clocksource 抽象一个可读取的递增计数器。内核按照稳定性、精度、读取开销和架构能力选择当前 clocksource。

~~~bash
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
cat /sys/devices/system/clocksource/clocksource0/available_clocksource
~~~

常见 cycle 到纳秒转换：

~~~text
cycle_delta = current_cycle - base_cycle
time_delta  = (cycle_delta * mult) >> shift
~~~

clocksource 不主动产生中断；“什么时候唤醒”由 clockevent device 负责。

## 5. Timekeeping 与 POSIX 时钟

timekeeping 将 clocksource 读数、时间基准、频率校正、NTP 调整和 suspend 信息组合成内核时间：

| 时钟 | 语义 | 是否包含 suspend | 是否可跳变 |
| --- | --- | --- | --- |
| CLOCK_REALTIME | UTC 墙上时间 | 否 | 是 |
| CLOCK_MONOTONIC | 启动后的单调时间 | 否 | 不向后跳 |
| CLOCK_MONOTONIC_RAW | 未经 NTP 校正的原始时间 | 否 | 不向后跳 |
| CLOCK_BOOTTIME | 启动后的时间 | 是 | 不向后跳 |
| CLOCK_TAI | TAI 时间 | 依实现而定 | 与 UTC 语义不同 |
| CLOCK_PROCESS_CPUTIME_ID | 进程消耗的 CPU 时间 | 不适用 | 随 CPU 消耗增长 |
| CLOCK_THREAD_CPUTIME_ID | 线程消耗的 CPU 时间 | 不适用 | 随 CPU 消耗增长 |

测量超时使用 CLOCK_MONOTONIC；必须把休眠计入使用 CLOCK_BOOTTIME；日历时间才使用 CLOCK_REALTIME。

## 6. Clockevent、tick 与 NO_HZ

clock_event_device 抽象“如何编程下一次时间事件”，典型能力包括 periodic、oneshot、绝对 deadline、suspend/resume 和 broadcast。

传统 tick 负责调度时间片、进程 accounting、jiffies、timer 处理和 CPU 统计。CONFIG_HZ 决定常规 tick 频率，但不等于最高定时精度。

- CONFIG_NO_HZ_IDLE：CPU 空闲时停止周期 tick，只等待下一个实际事件。
- CONFIG_NO_HZ_FULL：指定 CPU 尽量避免周期 tick，适合 CPU isolation、实时和 HPC。
- tick broadcast：本地 timer 在深度 idle 不可用时，由其他 CPU 或 always-on timer 唤醒。
- CPU hotplug 时，per-CPU tick 和 timer 必须迁移。

### 6.1 主要 tick_* 结构体

Linux 将硬件事件设备、每 CPU 的 tick 状态、tickless 调度状态和 broadcast 状态分开建模。不同内核版本会增删字段或调整定义位置，下面列出稳定的职责边界，而不是某个版本的完整字段清单。

| 结构体/对象 | 作用域 | 主要职责 |
| --- | --- | --- |
| struct tick_device | 每 CPU | 保存当前绑定的 clock_event_device 以及 tick 工作模式 |
| tick_cpu_device | 每 CPU 变量 | struct tick_device 的 per-CPU 实例；CPU 启停或设备切换时更新 |
| struct tick_sched | 每 CPU | 保存 scheduler tick、tickless idle、调度 hrtimer 和下一次 tick 状态 |
| tick_sched_timer | 每 CPU hrtimer | 在 oneshot 模式下驱动或模拟 scheduler tick |
| tick_do_timer_cpu | 全局状态 | 指示哪个 CPU 负责推进全局 jiffies/timekeeping 的 tick 工作 |
| tick_broadcast | 全局状态 | 管理无法使用本地 timer 的 CPU、broadcast 设备和 deadline |
| tick_broadcast_device | 全局设备 | 保存承担 broadcast 唤醒任务的 clockevent device |
| tick_next_period | 全局/架构相关 | 维护周期 tick 模式下的下一个周期边界 |

它们之间的关系：

~~~mermaid
flowchart LR
    CED[clock_event_device\n硬件事件设备]
    TD[tick_cpu_device\nper-CPU tick_device]
    TS[tick_sched\nper-CPU tickless 状态]
    ST[tick_sched_timer\nper-CPU hrtimer]
    J[tick_do_timer_cpu\n全局 jiffies/timekeeping owner]
    B[tick_broadcast\n不可用本地 timer 的 CPU 集合]
    BD[tick_broadcast_device\nbroadcast clockevent]

    CED --> TD
    TD --> TS
    TS --> ST
    TD --> J
    TD -. 本地 timer 不可用 .-> B
    B --> BD
    BD --> CED
~~~

### 6.1.1 面向初学者的 tick 概念类图

下面的图不是 Linux 源码中的 C++ 类，而是把 C 结构体、per-CPU 变量和全局状态用 UML 风格画出来。重点先看关系：一个 CPU 有一个 tick device 和一个 tick sched；tick device 持有硬件事件设备；tick sched 使用 hrtimer；本地 timer 不可用时，再接入 broadcast。

~~~mermaid
classDiagram
    class clock_event_device {
        +name
        +features
        +mode
        +set_next_event()
        +set_state_oneshot()
        +set_state_periodic()
    }

    class tick_device {
        +evtdev: clock_event_device
        +mode: periodic or oneshot
    }

    class tick_sched {
        +sched_timer: hrtimer
        +nohz_mode
        +tick_stopped
        +next_deadline
        +enter_idle()
        +exit_idle()
    }

    class hrtimer {
        +expires
        +function
        +start()
        +cancel()
    }

    class tick_do_timer_cpu {
        <<global state>>
        +owner_cpu
        +update_jiffies()
    }

    class tick_broadcast {
        <<global state>>
        +broadcast_mask
        +next_event
        +enter()
        +exit()
    }

    class tick_broadcast_device {
        <<global device>>
        +evtdev: clock_event_device
        +broadcast_oneshot()
    }

    class CPU {
        +cpu_id
        +idle
    }

    CPU "1" *-- "1" tick_device : 拥有
    CPU "1" *-- "1" tick_sched : 拥有
    tick_device "1" o-- "1" clock_event_device : 当前绑定
    tick_sched "1" *-- "1" hrtimer : 使用
    hrtimer --> clock_event_device : 编程下一次事件
    tick_device --> tick_do_timer_cpu : 推进 tick/timekeeping
    tick_broadcast_device "1" o-- "1" clock_event_device : 使用
    tick_broadcast --> tick_broadcast_device : 深度 idle 时接管
    CPU --> tick_broadcast : 加入或离开 broadcast mask
~~~

用一句话记忆这张图：

~~~text
CPU
 ├─ tick_device  → “我用哪个硬件 timer？”
 └─ tick_sched   → “我现在是否需要 scheduler tick？”
       └─ hrtimer → “下一次软件 deadline 是什么时候？”

如果本地硬件 timer 不能工作：
tick_broadcast → tick_broadcast_device → 代替本地 CPU 唤醒
~~~

### 6.2 struct tick_device

tick_device 是硬件 clockevent 与 tick 核心逻辑之间的 per-CPU 绑定对象。它保存当前设备指针和工作模式：

~~~text
tick_cpu_device[cpu]
    ├── evtdev  → 当前 CPU 使用的 clock_event_device
    └── mode    → TICKDEV_MODE_PERIODIC 或 TICKDEV_MODE_ONESHOT
~~~

当 clockevent 驱动注册了更好的设备，tick common 代码会检查 rating、CPU mask 和 capabilities，停止旧设备，绑定新设备，设置 periodic/oneshot 模式并重新编程下一次事件。tick_check_new_device、tick_install_replacement 和 tick_shutdown 是理解这条路径的关键入口。

### 6.3 struct tick_sched 与 tick_sched_timer

tick_sched 描述每 CPU 的 scheduler tick 状态，尤其是 NO_HZ_IDLE 和 NO_HZ_FULL 下的状态。它通常记录：

- 当前 CPU 是否 idle、tick 是否停止以及进入/退出 idle 的统计。
- 下一个 tick 或 timer deadline。
- 与 scheduler tick 相关的高精度 timer。
- 最后一次 tick、idle jiffies 和 nohz 模式。

tick_sched_timer 是这个状态机使用的 hrtimer。周期 tick 被停止后，内核仍可能通过它在需要时触发 scheduler tick、更新时间、处理 RCU 或重新评估下一次 idle 事件。因此 tickless 不是“CPU 永远没有 timer”，而是把周期事件变为按需 oneshot 事件。

### 6.4 tick_do_timer_cpu

全局 jiffies 和部分 timekeeping 工作不一定由每个 CPU 同时推进。tick_do_timer_cpu 用于选择当前负责执行 do_timer 工作的 CPU，避免多 CPU 重复推进。CPU idle、hotplug 或 nohz 状态变化时，owner 可以转移；某 CPU 可以停止本地 scheduler tick，但系统仍必须保持全局时间正确。

### 6.5 tick_broadcast 与 broadcast device

进入深度 idle 后，本地 clockevent 可能停止，或者设备不支持在该 CPU 上继续运行。tick broadcast 维护需要被唤醒的 CPU 集合，并由 tick_broadcast_device 指向仍可工作的 clockevent device：

~~~text
CPU 进入深度 idle
    ↓
tick_broadcast_enter() 将 CPU 加入 broadcast mask
    ↓
broadcast device 被编程为这些 CPU 中最早的 deadline
    ↓
broadcast 中断到达
    ↓
tick_broadcast_oneshot_control() / tick_handle_oneshot_broadcast()
    ↓
移除已唤醒 CPU，并重新计算剩余 deadline
~~~

周期 broadcast 和 oneshot broadcast 的细节不同；阅读具体版本时，应同时查看 kernel/time/tick-broadcast.c 和对应架构的 clockevent 驱动。

### 6.6 tick 设备切换与模式切换

~~~mermaid
stateDiagram-v2
    [*] --> Shutdown
    Shutdown --> Periodic: 选择周期 clockevent
    Shutdown --> Oneshot: 支持 oneshot 且启用高精度/nohz
    Periodic --> Oneshot: tick_switch_to_oneshot()
    Oneshot --> Periodic: 回退或禁用 oneshot
    Oneshot --> Broadcast: CPU 深度 idle
    Broadcast --> Oneshot: CPU 唤醒
    Periodic --> Shutdown: CPU offline 或设备移除
    Oneshot --> Shutdown: CPU offline 或设备移除
~~~

## 7. Timer wheel：普通内核定时器

timer wheel 通过分桶管理大量低精度 timer，核心单位通常是 jiffies：

~~~c
struct timer_list timer;
timer_setup(&timer, callback, 0);
mod_timer(&timer, jiffies + msecs_to_jiffies(100));
del_timer_sync(&timer);
~~~

它适合协议重传、设备超时、重试延迟和普通 watchdog。优点是大量 timer 的插入和到期开销低；缺点是精度受 jiffies 和调度延迟影响，不应表达纳秒级 deadline。timer 通常按 per-CPU timer base 组织，CPU 下线时必须迁移。

## 8. HRTimer：高精度定时器

hrtimer 使用按到期时间排序的队列，将最早 timer 作为下一次 clockevent 的编程目标：

~~~text
插入 hrtimer → 找到最早到期项 → 编程 clockevent
        → 硬件中断 → hrtimer callback
~~~

典型使用者包括 nanosleep、clock_nanosleep、POSIX timer、timerfd、SCHED_DEADLINE 和实时子系统。callback 的执行上下文决定是否可以睡眠；较重工作应转交 workqueue。通常需要 CONFIG_HIGH_RES_TIMERS 和 oneshot clockevent 硬件。

## 9. 中断到 callback 的时序

~~~mermaid
sequenceDiagram
    participant HW as Clockevent 硬件
    participant IRQ as Timer IRQ
    participant T as Tick/Timer 子系统
    participant Q as Timer wheel 或 hrtimer
    participant C as Kernel callback
    participant U as 用户进程

    T->>Q: 插入 timer，计算最早 deadline
    T->>HW: 编程下一次事件
    HW->>IRQ: 到期并产生中断
    IRQ->>T: 进入 timer interrupt handler
    T->>T: 更新 timekeeping、tick、accounting
    T->>Q: 扫描已到期 timer
    Q->>C: 执行 callback 或触发 deferred work
    C-->>U: 唤醒任务、设置 fd 可读或发送信号
~~~

## 10. 内核态 API

| API | 机制 | 用途 |
| --- | --- | --- |
| struct timer_list | timer wheel | 普通超时 |
| hrtimer | 高精度队列 | deadline |
| schedule_timeout | 进程等待 | 睡眠一段时间 |
| wait_event_timeout | 等待队列 + timer | 等待条件或超时 |
| schedule_delayed_work | timer + workqueue | 可睡眠的延迟工作 |
| msleep / usleep_range | 睡眠 API | 简单延迟 |

需要睡眠的工作应使用 delayed work 或唤醒等待任务，而不是直接在不能睡眠的 timer callback 中阻塞。

## 11. 用户态 API

### nanosleep 与 clock_nanosleep

支持相对睡眠和绝对截止时间。周期任务优先使用绝对时间：

~~~text
deadline = start + period
循环：
    执行工作
    deadline += period
    睡眠直到 deadline
~~~

这能避免工作耗时造成累计漂移。

### POSIX timer

timer_create、timer_settime、timer_gettime 和 timer_delete 提供 POSIX 定时器。通知可通过信号、线程或 POSIX notification 机制传递，底层通常使用 hrtimer。

### timerfd

timerfd_create 创建文件描述符，timerfd_settime 设置到期时间，read 读取到期次数。它可接入 poll、select 和 epoll，适合事件循环且避免信号处理复杂性。

### alarm 与 setitimer

这是较老的 Unix 接口。setitimer 支持 ITIMER_REAL、ITIMER_VIRTUAL 和 ITIMER_PROF。新代码通常优先考虑 timerfd 或 POSIX timer。

## 12. 周期、overrun 与 timer slack

周期 timer 到期后，如果进程长时间没有处理通知，多个周期可能已经过去。timerfd 通过读取值返回累计到期次数；POSIX timer 提供 overrun 查询机制，应用不能假设一次通知只代表一个周期。

timer slack 允许内核在小范围延迟 timer，将相近唤醒合并，以减少中断、上下文切换和功耗。可通过 prctl(PR_SET_TIMERSLACK, ...) 调整。

相对 timer 容易受调度延迟和反复重置影响；绝对 timer 更适合周期任务和 deadline。

## 13. CPU timer、watchdog 与实时调度

CPU timer 按任务实际消耗的 CPU 时间计时，相关对象包括 CLOCK_PROCESS_CPUTIME_ID、CLOCK_THREAD_CPUTIME_ID、RLIMIT_CPU 和 SCHED_DEADLINE。

watchdog 用于故障检测或恢复：

- 硬件 watchdog：停止喂狗后复位机器，通常通过 /dev/watchdog 使用。
- softlockup、hardlockup、hung task detector：检测 CPU 不调度、CPU 卡死和任务停滞。
- RTC alarm：可在 suspend 或低功耗状态下唤醒系统；REALTIME_ALARM 和 BOOTTIME_ALARM 通常需要 CAP_WAKE_ALARM。

## 14. Suspend、CPU hotplug、虚拟化与 namespace

挂起时某些 CPU timer 会停止，恢复时要重建 clockevent、修正 timekeeping、处理已到期 timer 并重新编程下一事件；这也是 MONOTONIC 与 BOOTTIME 行为不同的原因。

KVM、Xen、Hyper-V 等可提供 paravirtualized clock 和 virtual timer。VM exit、host 调度、steal time、TSC 虚拟化和 live migration 都会影响 guest 精度。

Linux time namespace 可为容器的 MONOTONIC 和 BOOTTIME 提供不同偏移，适用于 checkpoint/restore 和进程迁移。

## 15. 关键源码位置

不同内核版本的文件会移动或拆分，主要入口包括：

~~~text
kernel/time/
    clocksource.c       clocksource 注册和选择
    clockevents.c       clockevent 抽象
    timekeeping.c       系统时间维护
    tick-common.c       通用 tick
    tick-sched.c        tickless/调度 tick
    hrtimer.c           高精度定时器
    timer.c             普通内核 timer
    posix-timers.c      POSIX timer
    namespace.c         time namespace

include/linux/
    timer.h
    hrtimer.h
    clocksource.h
    clockchips.h

drivers/clocksource/   各架构和平台 clocksource/clockevent
drivers/rtc/           RTC 和唤醒 alarm
kernel/sched/          调度器 tick、CPU timer、deadline
~~~

## 16. 完整示例：timerfd

~~~mermaid
sequenceDiagram
    participant A as 应用
    participant F as timerfd
    participant H as HRTimer
    participant E as Clockevent
    participant W as 硬件 IRQ

    A->>F: timerfd_create(CLOCK_MONOTONIC)
    A->>F: timerfd_settime()
    F->>H: 配置并插入 hrtimer
    H->>E: 请求编程最早 deadline
    E->>W: 设置下一次硬件事件
    W-->>H: 到期中断
    H->>F: 累计 expiration count
    F-->>A: fd 变为可读
    A->>F: read() 获取到期次数
~~~

## 17. 选择建议

| 需求 | 推荐选择 |
| --- | --- |
| 内核普通超时 | struct timer_list |
| callback 需要睡眠 | delayed work 或等待队列超时 |
| 高精度内核 deadline | hrtimer |
| 事件循环中的周期事件 | timerfd + epoll |
| 信号/线程式通知 | POSIX timer |
| 测量间隔 | CLOCK_MONOTONIC |
| 休眠时间必须计入 | CLOCK_BOOTTIME |
| 日历时间触发 | CLOCK_REALTIME，并处理时钟跳变 |
| 故障复位 | hardware watchdog |

最终链路：

~~~text
硬件计数器 → clocksource → timekeeping
硬件中断器 → clockevent → tick
tick → timer wheel / hrtimer
定时器 → 内核 callback / 调度器 / timerfd / POSIX notification
~~~
