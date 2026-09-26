# kernel/time 改动记录（2026-09-26）

## 删除无人调用的 `calculate_mult_shift()`（2026-09-26）

**文件**
- `kernel/include/ours/time/mod.hpp` — 删掉 `calculate_mult_shift()` 的声明
- `kernel/kernel/time/mod.cpp` — 删除；它只有这一个函数
- `kernel/kernel/time/BUILD.gn` — `sources` 去掉 `mod.cpp`

**症状**
无运行时症状。`calculate_mult_shift()` 在全树没有调用者；`260903/kernel-time.md` 已经记过"留着没动（仍无人调用）"。

**原因**
修改前：

```cpp
// kernel/include/ours/time/mod.hpp
auto current_mono_time() -> Time;

auto calculate_mult_shift(usize *mult, usize *shift, usize from, usize to, usize maxsec) -> void;

// kernel/kernel/time/mod.cpp
auto calculate_mult_shift(usize *mult, usize *shift, usize from, usize to, usize maxsec) -> void {
    ...
    usize s = 32;
    for (; s > 0; --s) {
        m = ((t << s) + (f >> 1)) / f; // See (1)
        if ((m >> mbits) == 0) {
            break;
        }
    }

    *mult = m;
    *shift = s;
}
```

它是 Linux `clocks_calc_mult_shift()` 的移植：挑一个 `(mult, shift)`，让 `(count * mult) >> shift` 近似
`count * to / from`。这套办法的前提是 `count * mult` 装得进 64 位，也就是 delta 有界——Linux 靠每个 tick 一次的
`timekeeping_update()` 推进 `cycle_last` 来保证这一点，`maxsec` 参数就是这个上界。

本内核在 0830 把 `Clock::now()` 改成了纯函数（`anchor.time + source->ticks_to_time(read() - anchor.ticks)`，
读路径不写任何东西），delta 就变成"自上次换 source 以来的全部 ticks"，没有上界，`maxsec` 这个前提不成立。
所以当时 `ClockSource` 和 `ClockEventDevice` 都改用了 `mtl::Ratio<u32>`：约分后的精确分数，
`mtl::scale()` 把 64 位值拆成高低 32 位分别乘除，中间量不会溢出（理由见 `docs/repairs-20260830.md`
「为什么是 `mtl::Ratio` 而不是 `calculate_mult_shift()`」）。从那以后这个函数就没有调用者了。

留着它，下一个读代码的人会以为这是时钟换算的正路，而它依赖的「delta 有界」前提在本内核里已经不存在了。

**改法**
修改后：

```cpp
// kernel/include/ours/time/mod.hpp
auto current_mono_time() -> Time;

} // namespace ours::time
```

`mod.cpp` 整个删除，`BUILD.gn` 的 `sources` 去掉它。

**参照**
Linux `clocks_calc_mult_shift()`（`kernel/time/clocksource.c`）是它的原型；本内核的换算对应的是 Zircon 的
`affine::Ratio`（`platform_get_ticks_to_time_ratio()`），也就是现在的 `mtl::Ratio`。

**验证**
- `clang-format` / `clang-tidy -p out/bringup.x86` 对 `mod.hpp` 无告警。
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过（GN 自动重新生成）。
- QEMU 启动 `kernel.dev.obi`、`kernel.dev.dbg_1.obi` 各约 50 s：都进 shell，无 panic / 断言，
  dbg_1 的 54 条 warn 与修改前相同（用户态 #DE 测试的寄存器转储）。

**故意没做**
- `kernel/lib/clock` 的 `FixedPointRatio` 是同一个 mult/shift 思路的另一个实现（128 位中间量，不受 delta 有界的限制），
  同样没有调用者，`calibration.hpp` 还引用了不存在的 `BinaryFixedPointRatio`。它对应
  `docs/development/kernel/time.md` 里的 TODO 设计，是否删除要单独决定，不在这一条里。
- 时钟换算没有改用 ffl。`ffl::Fixed<Integer, FractionalBits>` 的小数位数是编译期模板参数，而时钟比值要到运行时
  （校准之后）才知道，范围还很宽：32 kHz 的计数器每 tick 约 30517 ns，3 GHz 的 TSC 每 tick 约 0.33 ns，反方向
  （ns → ticks）同样跨这么多个数量级。一个编译期格式只能照顾两头中的一头，这正是 Linux 要在运行时算 `shift` 的原因。
  另外，ffl 的收敛舍入是为"反复累加许多舍入值"设计的（调度器的虚拟时间），而比值常数只量化一次、之后乘以很大的
  tick 数，误差是随 tick 数线性增长的系统偏差，ties-to-even 抵消不了；`mtl::Ratio` 约分后是精确的，没有这个误差。
  Zircon 也是这样分工：ffl 只用在调度器，时间换算用 `affine::Ratio`。
- `current_mono_time()` 在 `mod.hpp` 和 `platform/timer.hpp` 各声明了一次，没有动。

## 模拟周期 tick 改在同一次中断里补拍，一拍变贵不再活锁（2026-09-26）

**文件**
- `kernel/include/ours/time/clock_event_device.hpp` — 新增 `ClockEventDevice::LateDeadline { Force, Reject }`；
  `program_event()` 多一个参数 `late`，默认 `Force`；声明里的形参名 `expired` 改成和定义一致的 `expiry`
- `kernel/kernel/time/clock_event_device.cpp` — `program_event()` 在 `Reject` 下对已过去的截止时间、以及设备拒绝的
  截止时间直接返回 `Status::TimeOut`，不钳到最小间隔、不重试；`Force` 的行为一字未变
- `kernel/kernel/time/tick_device.cpp` — `handle_periodic_event()` 改成 Linux `tick_handle_periodic()` 的循环；
  描述"一次中断补一拍"是预期行为的那段长注释整段重写

**症状**
`kernel.dev.dbg_1.obi` 大约三分之二的启动卡在 shell 的 fault_probe 上：新 probe 的线程已经
`userabi: 'fault_probe' starts`，却再也打不出 `fault_probe: <name>`，cpu0 不停打
`Exception[apic timer] from user space`。`kernel.dev.obi` 不受影响。`faa117f7`（LAPIC 定时器按 CPU 取设备，
每个 CPU 的 tick 从 `HZ / 4` 恢复到 `HZ`）之后才出现。`faa117f7` 本身是对的，没有回退。

**原因**
修改前：

```cpp
// kernel/kernel/time/tick_device.cpp
auto TickDevice::handle_periodic_event(ClockEventDevice *device) -> void {
    auto const cpu = Cpu::id();
    if (cpu == ClockSource::Default::CPUNUM) {
        ClockSource::Default::advance_ticks(1);
    }

    TickSched::get().on_tick(SteadyClock::now());

    if (!device->is_mode_oneshot()) {
        return;
    }

    auto const next = device->next_event() + NSEC_PER_TICK;
    auto const status = device->program_event(next);
    DEBUG_ASSERT(Status::Ok == status, "Failed to re-arm the emulated periodic tick on CPU[{}]",
                 cpu);
}

// kernel/kernel/time/clock_event_device.cpp，program_event() 里
    auto const now = SteadyClock::now();
    auto const delta = std::clamp(expiry - now, m_min_delta, m_max_delta);
```

LAPIC 定时器只有 oneshot，周期 tick 是靠这个处理函数每次往后排一拍模拟出来的。排的基准是**上一次的截止时间**
而不是 now，这是对的（中断延迟不会累积成漂移）；问题在于处理函数晚了一个周期以上时怎么办。那时
"上次截止 + 一个周期"已经过去，`program_event()` 的 clamp 把负的 delta 钳到 `m_min_delta`（几微秒），也就是
"马上再来一次中断"，丢掉的那一拍由下一次中断补上，再下一拍再由下一次补。

这个补法稳不稳定，取决于一拍的开销 C 和周期 P（10 ms）的大小关系，而且是悬崖，不是斜坡：

- 落后时中断一个接一个来，每次中断花掉 C 的墙钟时间，只把截止时间往后推 P，所以每补一拍，落后量变化 C − P。
- C < P：落后量每拍缩小 P − C，追得上，追上后恢复正常。
- C > P：落后量每拍**增大** C − P，永远追不上。两次中断之间回到用户态的时间只有 `m_min_delta` 那么长，
  被打断的用户线程实际上一条指令也跑不了。没有任何环节会丢弃负载，所以不会自己恢复。

dbg_1 里 C 恰好越过了 P。`faults.cpp:437` 对每个来自 ring 3 的中断打一行 `log::trace`，而 cpu2/3 上的测试线程
同时在往同一个串口刷 `I'm Thread N`，这一行要排队等 UART——卡住时抓的寄存器里 cpu0/2/3 都在
`SerialLogger::write()`。更糟的是这里有一个自我选择：trace 只在中断来自用户态时打，而钳到 `m_min_delta` 的补拍
恰好总是在刚回到用户态时落下，所以每一次补拍走的都是最贵的那条路径。卡住期间 cpu0 来自用户态的 tick 平均间隔
16.4 ms，比 10 ms 的周期还长——这就是 C > P 的直接读数。`faa117f7` 之前每个 CPU 实际是 25 Hz，P 等于 40 ms，
同样的 C 落在悬崖的安全一侧，所以看不出来。

`faults.cpp:437` 那一行只是诱因，不是病根。活锁的条件是 C > P，而 C 不归时间子系统管：任何让 tick 路径变贵的东西
都算——别处的任何一行日志抢串口、一个慢的定时器回调、调度器路径上的一把锁；P 也会变（`OURS_CONFIG_HZ = 1000`
时 P 只有 1 ms）。把这一行降级只是把 C 调小、把余量调大，悬崖还在原地，下一个让 C 变大的改动还会掉下去。
能消除它的只有一件事：让"追赶"本身不再花一整次中断的代价。

**改法**
修改后（源码里的长注释从略，枚举的注释缩成了行尾一句）：

```cpp
// kernel/include/ours/time/clock_event_device.hpp
    enum class LateDeadline : u8 {
        Force,  // 钳到最小间隔、设备拒绝就往后重试：Linux 的 force = true
        Reject, // 什么都不装，返回 Status::TimeOut：Linux 的 force = false 和 -ETIME
    };

    auto program_event(TimePoint<> expiry, LateDeadline late = LateDeadline::Force) -> Status;

// kernel/kernel/time/clock_event_device.cpp，program_event() 里
    auto const now = SteadyClock::now();
    if (late == LateDeadline::Reject && expiry <= now) {
        return Status::TimeOut;
    }
    auto const delta = std::clamp(expiry - now, m_min_delta, m_max_delta);

    auto const status = set_next_event_tick(time_to_ticks(delta));
    if (Status::Ok == status) {
        return status;
    }
    if (late == LateDeadline::Reject) {
        return status;
    }
    return program_min_delta();

// kernel/kernel/time/tick_device.cpp
auto TickDevice::handle_periodic_event(ClockEventDevice *device) -> void {
    auto const cpu = Cpu::id();
    auto const do_timer = [cpu] {
        if (cpu == ClockSource::Default::CPUNUM) {
            ClockSource::Default::advance_ticks(1);
        }
    };

    auto next = device->next_event();

    do_timer();
    TickSched::get().on_tick(SteadyClock::now());

    if (!device->is_mode_oneshot()) {
        return;
    }

    for (;;) {
        next += NSEC_PER_TICK;
        auto const status = device->program_event(next, ClockEventDevice::LateDeadline::Reject);
        if (Status::TimeOut != status) {
            DEBUG_ASSERT(Status::Ok == status,
                         "Failed to re-arm the emulated periodic tick on CPU[{}]", cpu);
            return;
        }
        do_timer();
    }
}
```

截止时间已过，`Reject` 就把它原样退回来，处理函数在**同一次中断里**记下这一拍、再往后排一拍，直到排到将来。
每轮只花几条指令、却把截止时间推一整个周期，所以循环一定很快结束；不管这次中断晚了多久，它留下的下一次中断
都在将来，从现在到那一刻的时间归被打断的线程。C > P 时的结果是 tick 实际每 ⌈C/P⌉ 个周期来一次——多出来的周期
被计进 jiffies，但不再各自花一次中断去执行，这就是原来缺的那个"丢弃负载"的环节。

`Force` 的代码路径和以前完全相同：timer queue（经 `TickDevice::program_event()`）和 `setup_periodic()` 的首次装填
都靠它"过期了也给我一个尽快的中断"，那个理由没变，所以默认值留在 `Force`，只有周期 tick 这一处在调用点上写明
`Reject`。`program_min_delta()` 注释里"周期 tick 会断言"那半句随之改掉：周期 tick 不再走到那里。

设计点：补上的那几拍只推进 jiffies，**不**调 `TickSched::on_tick()`。Linux 每补一拍调一次完整的 `tick_periodic()`，
包括 `update_process_times()`，那是因为它的 CPU 时间记账按次数计（`account_process_tick()` 每次记一个 `TICK_NSEC`），
少调一次就少记 10 ms。这里 `on_tick()` 下面没有一样东西按次数计：

- 调度器 `Scheduler::tick_thread()` 走 `update_clock()`，按时钟差值记账；
- `TimerQueue::run_expired_timers()` 跑的是所有截止时间不晚于 now 的定时器；

所以循环前那一次 `on_tick(now)` 已经覆盖了全部流逝的时间，再调一次什么也找不到。唯一按次数计的是 prof 采样器的
tick 源（`m_tick_counter % period`），多调几次只会在同一个时刻把同一个被打断的上下文记好几遍——那是捏造样本，
不是补样本。还有最要紧的一条：循环必须便宜才保证能结束。`on_tick()` 的开销没有上界，把它放进循环，循环就只在
"一拍比一个周期便宜"时才结束——恰恰是这次失效的那个条件，还被挪进了一次关着中断的中断里，CPU 连回来的机会都没有。
jiffies 本身则必须每个周期都计：它就是周期的计数，local APIC 的校准也是拿它当尺子。

循环还依赖一个前提：推进 jiffies 不会推进 `program_event()` 用来量 now 的那个时钟。Linux 用
`timekeeping_valid_for_hres()` 挡在循环里的 `tick_periodic()` 前面就是为此——时钟源是 jiffies 时，每轮 now 跟着截止
时间一起走，循环永远追不上。这里 jiffies 根本还不是 ClockSource（`ClockSource::Default::init()` 没有实现），
`SteadyClock` 读 HPET，HPET 注册前读 0，截止时间永远在将来；这一点写进了注释，jiffies 注册成 source 的那次改动
需要把这个判断加回来。

`next` 在 `on_tick()` 之前读，和 Linux 在 `tick_periodic()` 之前读 `dev->next_event` 一样：后面每一拍都从这次中断
回应的那个截止时间算起，`on_tick()` 做什么都不该挪动这个基准。

否掉的方案：

- **回退 `faa117f7`**：回到每 CPU 25 Hz 的错误频率，只是把余量藏起来。
- **从 now 往后排一拍**（`now + NSEC_PER_TICK`）：也稳定，但中断延迟会累积成漂移，丢掉的周期也不再计进 jiffies。
  Linux 不这么做。
- **用除法一次算出丢了几拍**：O(1)，但"过没过期"要在 `program_event()` 之外再读一次 now 自己判断，和真正写硬件时
  那一次 now 之间有竞争；还会漏掉设备自己的拒绝（HPET 的比较器写晚了返回 `TimeOut`）。让写硬件的同一处给出判定，
  是 Linux 这个循环的结构，迭代次数只是"落后量 / P"。
- **`bool force` 形参**：调用点上 `program_event(next, false)` 看不出 false 是什么；枚举把选择写在调用点上。
- **另起一个 `try_program_event()`**：两个名字、两份入口，却只有一处调用者需要非默认的那个；一个函数加一个默认参数
  和 Linux 的一个函数加 `force` 对得上。

**参照**
- Linux `tick_handle_periodic()`、`tick_periodic()`（`kernel/time/tick-common.c`）：oneshot 设备模拟周期时的
  `for (;;) { next += TICK_NSEC; if (!clockevents_program_event(dev, next, false)) return; tick_periodic(cpu); }`。
- Linux `clockevents_program_event()`、`clockevents_program_min_delta()`（`kernel/time/clockevents.c`）：
  `delta <= 0` 时 `force ? clockevents_program_min_delta(dev) : -ETIME`，设备拒绝时 `(rc && force) ? ... : rc`。
- Linux `timekeeping_valid_for_hres()`、`account_process_tick()`：上面"只推 jiffies"和"时钟不能是 jiffies"两条
  取舍的对照。
- Zircon 没有对应物：它没有周期 tick，`timer_queue` 只按下一个截止时间装 oneshot（`platform_set_oneshot_timer()`），
  不存在"补拍"这件事。

**验证**
- `clang-format`；`clang-tidy -p out/bringup.x86` 对 `tick_device.cpp`、`clock_event_device.cpp` 在改动行上无新告警
  （只有 `clock_event_device.hpp` 里原有的 `GCED`/`GCED_MUTEX` 命名告警）。
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过。
- QEMU：每轮 3 个 dbg_1 并行加 1 个 dev，约 90 s 后杀掉；通过 = dbg_1 出现 `probes done`、`bootfs checks passed`
  和 `--- end ---`（dev 只看 `--- end ---`）。验证期间工作区里有 mm 会话还没提交的 `fault.cpp`（page fault 开中断，
  带临时 PROBE 日志，后来提交为 `36a67da5`），所以 A/B 两组都在**同一份** `fault.cpp` 下构建：修复前那组是把这三个
  文件换回 HEAD 重建的，两组只差这次改动。最后一轮在提交后的 `36a67da5` 加本改动上重新构建。
  "最长停滞"是去掉 apic-timer trace 和 `I'm Thread` 两类刷屏之后、到 `--- end ---` 为止相邻两条内核日志的最大间隔
  （没跑完的算到日志末尾）；"cpu0 用户态 tick"是 cpu0 上 `Exception[apic timer] from user space` 的条数和平均间隔。
  时间是日志里的 TSC 读数（QEMU 下约等于纳秒），按秒写。

| 轮次 | 镜像 | 结果 | `--- end ---` | 最长停滞 | cpu0 用户态 tick |
|---|---|---|---|---|---|
| 修复前（更早一版 `fault.cpp`） | dbg_1 #0 | 卡死：probe 已 starts，之后只剩 apic timer | — | 46.9 s | 4935 条 / 15.5 ms |
| | dbg_1 #1 | 卡死 | — | 43.9 s | 4451 条 / 17.0 ms |
| | dbg_1 #2 | `probes done` 之后停住 | — | 29.1 s | 4161 条 / 18.3 ms |
| | dev | 通过 | 3.8 s | 1.5 s | — |
| 修复前（与修复后同一 `fault.cpp`） | dbg_1 #0 | 卡死 | — | 83.9 s | 4482 条 / 18.7 ms |
| | dbg_1 #1 | 跑完，但中途停了 48.5 s | 75.4 s | 48.5 s | 819 条 / 82.9 ms |
| | dbg_1 #2 | 卡死 | — | 79.7 s | 4217 条 / 19.5 ms |
| | dev | 通过 | 3.7 s | 1.5 s | — |
| 修复后 第 1 轮（主机忙，QEMU 实际只跑了约 50 s） | dbg_1 #0 | 已过 `bootfs checks passed`，被截断 | — | 8.4 s | 696 条 / 62.4 ms |
| | dbg_1 #1 | 已过 `probes done`，被截断 | — | 7.0 s | 649 条 / 61.1 ms |
| | dbg_1 #2 | 通过 | 13.2 s | 1.4 s | 102 条 / 50.2 ms |
| | dev | 通过 | 3.7 s | 1.4 s | — |
| 修复后 第 2 轮 | dbg_1 ×3 | 3/3 通过 | 20.6 / 11.4 / 9.0 s | ≤ 1.6 s | 301 / 74 / 33 条 |
| | dev | 通过 | 3.7 s | 1.4 s | — |
| 修复后 第 3 轮 | dbg_1 ×3 | 3/3 通过 | 25.2 / 21.6 / 24.8 s | ≤ 3.0 s | 291 / 322 / 270 条 |
| | dev | 通过 | 3.5 s | 1.3 s | — |
| `36a67da5` + 本改动 | dbg_1 ×3 | 3/3 通过 | 32.3 / 31.3 / 24.4 s | ≤ 3.1 s | 360 / 410 / 201 条，平均 58–80 ms |
| | dev | 通过 | 3.7 s | 1.5 s | — |

  读法：修复前卡住的那几次，cpu0 在用户态挨了四千多次 tick、平均间隔 15–20 ms——比 10 ms 的周期还长，就是
  C > P 时背靠背的补拍；修复后同样的负载下只有几十到几百次，平均间隔 40–80 ms，用户线程拿到了时间。修复后第 1 轮
  没跑完的两次不是卡住：那一轮日志的 TSC 在杀掉时只走到 49.9 s（其余各轮都到 90 s），是 `qemu_boot.py` 在主机忙时
  建 ESP 慢、QEMU 起得晚；那两次最长的空档 8.4 s / 7.0 s 里 cpu0 的用户态 tick 间隔在几十到几百毫秒之间，用户态在跑。
  全部日志无 panic、无断言（包括新的 re-arm `DEBUG_ASSERT`），dbg_1 每次 53 条 warn，都是用户态异常测试的寄存器转储。

**故意没做**
- `faults.cpp:437` 那行每中断一条的 trace 没有降级。它属于 x86 中断入口，另行处理；如上所述，降级它只增加余量，不是修复。
- 补拍不调 `on_tick()`，理由见改法。将来若有按 tick 次数记账的消费者加进 `on_tick()`，它应该改成读时钟差值，
  而不是把 `on_tick()` 放进这个循环。
- Linux 在 `tick_periodic()` 之后检查 `dev->event_handler != tick_handle_periodic`（这次 tick 里 CPU 可能已经切到
  highres/nohz）。这里没有这种切换——`switch_to_oneshot()` 还没有调用者——等 HpTick 接上时要补这个检查。
- `Feature::TimePoint` 设备在两种策略下都原样收下绝对截止时间，过期判定交给驱动自己（Linux 对
  `CLOCK_EVT_FEAT_KTIME` 也是如此）。现在没有这种驱动，注释里写明了将来用它模拟周期 tick 时驱动要自己返回 `TimeOut`。
- CPU 被停很久（比如调试器停了 10 s）之后第一次中断，循环要转约 1000 轮，每轮读一次 HPET，毫秒级，可以接受，
  Linux 同样如此。
