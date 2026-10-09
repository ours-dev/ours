# kernel/time — 待修

历史与已修部分见 `docs/repairs-20260830.md` 第二十六至三十九节。

现状：`SteadyClock::now()` 给出正确纳秒，周期 tick 以正确频率产生（QEMU 实测偏差约 1%），
设备选择、`Timer` 的同步 cancel、`ClockEventDevice` 的自动 vtable 都已就位。

---

## 2. jiffies 还没作为 ClockSource 入队

`ClockSource::Default::init()` 只有声明没有定义，`SOURCE` 恒为空，所以 `Default::get()` 不能调。
后果：HPET 在 `InitLevel::Irq + 2` 入队之前，`SteadyClock::now()` 一直返回 0。

要做的：造一个读 `Default::read()` 的 `ClockSource`，以 rating 1（Linux 给 jiffies 的值）、
频率 HZ、**不带** `Flags::HpClock` 入队，挂一个 `InitLevel::Irq` 的 init hook（早于 Irq+1 的
PIT）。不带 HpClock 是对的：`select_best_source()` 在 oneshot 模式下会跳过它。

顺带：`ClockSource::enroll()` 目前不接受 flags，所以 **HPET 也没有 `Flags::HpClock`**——
一旦 tick 设备进 oneshot 模式，`select_best_source()` 会一个源都选不出来。加 flags 参数时
一并修。

## 3. `GCED_MUTEX` 是睡眠锁，却配 `IrqSave`

`ClockEventDevice::GCED_MUTEX` 是 `sync::Mutex`（基于 `task::OwnedWaitQueue` 的睡眠锁），
而 `enroll()` 里用的是 `Guard<sync::Mutex, IrqSave>`。这个组合本身就是错的：
关中断的临界区里不能睡。

现在没出事的原因有两个，都不可靠：`IrqSave` 目前是空操作（见 `sync.md`），
而这段代码只在 init 期无争用地过一遍。修 `Guard` 之后这里会变成一个真正的
"关中断 + 可能睡眠"。（审查编号 C13，八步计划里漏了。）

`GCED` 这个链表本身也值得看一眼：它只被 `push_back`，从没被遍历过——名字像是
"garbage collected"，但没有回收逻辑。

## 4. `ClockSource::Vtable` 仍是手写打包

`ClockEventDevice::Vtable` 已改成按方法自动生成（`priv::clkevt_vtable` +
`CLKEVT_VTABLE_OF`，见第三十七节）。`ClockSource::Vtable` 还是老样子，`HpetClockSource` 里那张
表仍要逐槽列 `nullptr`。同一套 `consteval` 探测直接套用即可，槽位只有五个。

## 5. 驱动注册表（重构方案 C）

现在 PIT / HPET / LAPIC 靠三个手写 init hook 和手挑的级别
（`arch_time_init` Irq+1、`platform_hpet_init` Irq+2、`init_bsp_apic_timer` Irq+4）。
形式上照 `Scheduler::init()` 读 `SCHEDULES_START/END` 的 linker section 做成自注册。
解决的是"谁来探测"，与 vtable 生成正交。

## 6. 没有实现的功能

- **slack 合并**：`Deadline` 的 `earliest()/latest()` 区间在，但 `TimerQueue::enqueue_locked()`
  只按 deadline 插入、不合并。当前效果等价于"late"策略：带 slack 的 timer 总是把 slack 用满。
  要做的话只改 `enqueue_locked()`，参照 Zircon `InsertIntoTimerList()`。
- **tick 广播**：全局设备（PIT/HPET）一次只能打断一颗 CPU。没有广播，所以
  `TickDevice::check_new_device()` 里输掉选择的设备是直接放在一边，而不是装成广播设备。
- **clocksource watchdog**：`ClockSource::m_max_cycle` / `m_last_cycle` 声明了没人读，
  `Flags::Unstable` 也没人设。Linux `clocksource_watchdog` 用它们发现跳变的源。
- **NOHZ**：`TickSched::WorkMode::HpTick` 的路径写好了（`Timer` 自重编程 + `forward()`），
  但没有任何地方调 `emulate(HpTick)`，`SCHED_NOHZ` 也还是 false。

## 7. 悬空声明

- `kernel/platform/include/ours/platform/timer.hpp`：整个文件（`current_mono_ticks()`、
  `current_mono_time()`、`platform_convert_mono_time_to_ticks()`、`platform_set_oneshot_timer()`、
  `get_periodic_time()`）没有任何定义，也没有任何调用者。
- `system/public/ours/types.hpp` 的 `Time` / `Instant`（都是 `usize`）只被上面那个文件用到。

## 8. 没被验证到的路径

- **oneshot 路径和 `TimerQueue` 整体**：目前没有任何 timer 被武装过，`run_expired_timers()`
  在真实启动里从没跑过非空队列。需要先解掉 `scheduler.md` 里那个启动阻塞。
- **两个真实时钟源之间的换源**：全程只有 HPET 一个源入过队。第 2 条做完就能观察到一次真实换源。
