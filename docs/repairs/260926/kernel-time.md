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
