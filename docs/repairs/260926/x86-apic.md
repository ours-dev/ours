# arch/x86 apic 待修项（2026-09-26）

## 1. `LAPIC_TIMER_EVENT` 是全局变量，每个 CPU 的 LAPIC 定时器中断都去处理最后一个 AP 的设备

**现象**：多核下每个 CPU 的 tick 频率降到大约 `HZ / N`（N 是 CPU 数），包括推进 jiffies 的
那个 CPU；`TimerQueue` 的定时精度同样降 N 倍。bring-up 期间还可能让某个 CPU 的 tick 永久停掉。
没有断言会报，只会表现为时间走慢、超时变长。

**原因**：`kernel/arch/x86/apic/local_apic.cpp:366` 的
`static ClockEventDevice *LAPIC_TIMER_EVENT;` 没有 `CPU_LOCAL`（旁边的 `CLKDEV_STORAGE`、
`XAPIC_STORAGE` 都有）。每个 CPU 在 `init_apic_timer()` 里执行
`LAPIC_TIMER_EVENT = &self.m_clkevt;`（:666），BSP 先写，AP 后写，启动完它指向最后一个 AP 的设备。
`apic_handle_timer_irq()`（:369-372）却是所有 CPU 的 `ApicTimer` 向量共用的入口，
都去 `LAPIC_TIMER_EVENT->handle_event()`。

LAPIC 设备只声明 `Feature::Oneshot`（:161），`TickDevice` 从没切到 oneshot 模式，于是
`setup_periodic()` 用 oneshot 模拟周期（`tick_device.cpp:239-243`），
`handle_periodic_event()`（:288-304）读写**那一个共享设备**的 `m_next_event`，再经 `XAPIC_STORAGE`
（per-CPU）给**自己的** LAPIC 编程。N 个 CPU 轮流把同一个截止时间各往后推一个周期：
CPU0 触发，X = t+P；CPU1 触发，X = t+2P；……每个 CPU 都在 N 个周期后才再次触发。

bring-up 窗口更糟：AP 在 :666 写了全局之后、`setup_periodic` 装好处理函数之前，要走
GCED Mutex、configure、rebind 一整段；这时 BSP 或更早的 AP 来一个 LAPIC 中断，看到的是空的
`m_event_handler`（什么都不做）或还没进 Oneshot 的设备（`handle_periodic_event` 在 :296 直接返回），
那个 CPU 的 oneshot LAPIC 就再也没人重新编程，tick 永久停掉。AP 写 `std::function` 的同时别的
CPU 在读，也是一个没同步的跨 CPU 数据竞争。

**照谁改**：Linux `local_apic_timer_interrupt()`（`arch/x86/kernel/apic/apic.c`）里是
`struct clock_event_device *evt = this_cpu_ptr(&lapic_events);`——在中断里按当前 CPU 取。
这里对应在 `apic_handle_timer_irq()` 里用 `CpuLocal::access(CLKDEV_STORAGE).m_clkevt`
（中断上下文、中断关着，取本 CPU 那份是安全的），删掉全局；`init_apic_timer()` 里
`ClockEventDevice::enroll(*LAPIC_TIMER_EVENT, ...)`（:687）改成用 `self.m_clkevt`。

**来源**：2026-09-26 per-CPU 访问审计（抢占/迁移专题），已对照代码复核。
