# kernel/time 待修项（2026-09-26）

## 1. `rebind_device()` 改设备处理函数时没关中断，同一个 CPU 上该设备的中断可能读到写了一半的 `std::function`

**现象**：潜伏。按现在装的处理函数，实际上观察不到后果（见下），但属于未定义行为。

**原因**：BSP 在 `InitLevel::Irq+4` 注册 LAPIC 定时器时中断是开着的（`init_bsp_apic_timer()` 里
还断言了这一点，`local_apic.cpp:771`），而 HPET 此时已经在经 IRQ0 触发
`timer_interrupt -> CLOCK_EVENT(HPET)->handle_event()`（arch `time.cpp:23-27`、`hpet.cpp:382-384`）。
`TickDevice::rebind_device()` 对**同一个** HPET 设备执行 `m_clkdev->set_event_handler(nullptr)`
（`tick_device.cpp:195`）。处理函数是 `std::function`，赋值是多字交换，中断落在中间就会读到
撕裂的对象。

目前无害，是因为唯一装过的处理函数是 `setup_periodic` 里一个不捕获的 lambda，存在小缓冲里：
libc++ 下 `__f_` 指向的 `__func` 虽已析构但可平凡析构、vptr 还在；libstdc++ 先检查
`_M_manager`、最后才换 invoker。所以记为潜伏的 UB。

没有迁移问题：BSP 在 `task::init` 之前跑这段，AP 在中断关着时跑（`bootstrap16.S:28` 的 `cli`
之后到 `become_idle` 之前没人开中断），`Cpu::id()`（`tick_device.cpp:161/183`）是稳定的。

**照谁改**：Linux `clockevents_register_device()`（`kernel/time/clockevents.c`）在
`raw_spin_lock_irqsave(&clockevents_lock, flags)` 下调 `tick_check_new_device()`。这里在 GCED
Mutex 之外，用一个关中断的区间把 `TickDevice::Current::get().check_new_device()` 包起来即可。

**来源**：2026-09-26 per-CPU 访问审计，已复核。

## 2. `TickSched::on_tick()` 不分工作模式都跑 `run_expired_timers()`，进了 HpTick 就会在定时器回调里重入定时器队列

**现象**：潜伏，当前触发不到。所有 CPU 都停在 `WorkMode::Legacy`（`start_legacy_tick()`），`emulate(HpTick)`
和 `TickDevice::switch_to_oneshot()` 都还没有调用者。HpTick 接上的那一刻就会出现。

**原因**：`run_expired_timers()` 全树只有 `on_tick()` 一个调用者（`tick_sched.cpp:62`）。在 Legacy 下这是对的，
而且是唯一跑定时器的地方：TickDevice 处于 Periodic 时 `TickDevice::program_event()` 直接返回 Ok，TimerQueue
算出来的截止时间根本不会装进设备，定时器只能每个 tick 查一次，精度 1/HZ。

到了 HpTick，tick 本身变成一个 Timer：`TickSched::handle_event()` 是它的回调，而回调是由
`run_expired_timers()` 在自己的循环里调用的。回调里再调 `on_tick()`，`on_tick()` 又调
`run_expired_timers()`，就是在定时器队列的处理循环里重入它自己。里层那一轮会：

- 把 `m_next_deadline` 重置成 `INFINITE_TIME`，结束时再 `reprogram_locked()` 一次，外层回来后又重做一遍；
- 把 `m_running` 先改成里层的定时器、再清成 null，而外层的 tick 定时器其实还在栈上跑——`m_running` 的注释
  说它"非 null 恰好当且仅当回调在本 CPU 栈上"，这个不变量被打破。

另外 oneshot 模式下设备中断本身该跑定时器队列（Linux 的 `hrtimer_interrupt()`），这个处理函数还没写；
那时 `on_tick()` 再跑一次就是多余的第二个入口。

**照谁改**：Linux `hrtimer_run_queues()`（`kernel/time/hrtimer.c`，由 `update_process_times()` →
`run_local_timers()` 调用）开头就是 `if (__hrtimer_hres_active(cpu_base)) return;`：低精度模式靠 tick 查定时器，
高精度模式交给 `hrtimer_interrupt()`，tick 路径不碰。这里对应的是在 `on_tick()` 里只在
`m_workmode == WorkMode::Legacy` 时调 `run_expired_timers()`，注释写明对照 `hrtimer_run_queues()` 的 hres 检查。
今天行为不变。设备中断侧的 `hrtimer_interrupt()` 对应物，随 HpTick 一起做。

**来源**：2026-09-26 用户在修周期 tick 补拍活锁之后提问，本会话复核调用关系。
