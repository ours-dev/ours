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

## 2. 模拟周期 tick 逐次中断补拍，一拍的开销超过一个周期就活锁，dbg_1 卡在 fault_probe

**现象**：`kernel.dev.dbg_1.obi` 大约三分之二的启动卡在 shell 的 fault_probe 上：新 probe 的线程已经
`userabi: 'fault_probe' starts`，却再也没打出 `fault_probe: <name>`；cpu0 不停打
`Exception[apic timer] from user space`，shell 永远等不到结果。`kernel.dev.obi` 不受影响。
`faa117f7`（LAPIC 定时器按 CPU 取设备）把每个 CPU 的 tick 从 `HZ / N` 恢复到 `HZ` 之后才出现；
回退它就消失，但那只是回到每 CPU 25 Hz 的错误频率，把余量藏起来。

卡住时用 QEMU monitor 抓的寄存器：cpu0/2/3 的 RIP 都在 `SerialLogger::write()` 里等 UART，cpu1 在
`ArchIdleThread::idle()` 的 `hlt`；cpu0 的 CR3 是新 probe 的地址空间。卡住期间 cpu0 来自用户态的 tick
平均间隔 16.4 ms，比 10 ms 的周期还长。

**原因**：`TickDevice::handle_periodic_event()`（`tick_device.cpp`）从上一个截止时间往后排一拍：
`program_event(device->next_event() + NSEC_PER_TICK)`。截止时间已经过去时，`program_event()` 把它钳到设备的
最小间隔，注释里写的是"丢的 tick 一次中断补一拍"。这只在"处理一拍比一个周期便宜"时成立。一旦一拍的开销超过
周期，每次补拍都会让截止时间落得更远，中断一个接一个立刻到来，两次中断之间回到用户态的时间只有最小间隔那么长，
被打断的用户线程再也跑不动。这是正反馈，不会自己恢复。

dbg_1 里一拍变贵的原因：`faults.cpp:437` 对每个来自 ring 3 的中断都打一行 `log::trace`，cpu2/3 上的测试线程
同时在刷 `I'm Thread N`，这一行要排队等串口。频率是 `HZ / 4` 时余量够，恢复到 `HZ` 就不够了。

**照谁改**：Linux `tick_handle_periodic()`（`kernel/time/tick-common.c`）。对 oneshot 设备，它用
`clockevents_program_event(dev, next, false)`（`force = false`）重装；截止时间已过就返回 `-ETIME`，
于是在**同一次中断里**循环：调一次 `tick_periodic()` 补一拍，再往后排一拍，直到排到将来为止。补拍不经过
中断入口/出口，也不会再触发一次中断。这里需要一个不强制的 `program_event()` 变体（过去的截止时间报错，而不是
钳到最小间隔），`handle_periodic_event()` 照这个结构改成循环。`faults.cpp:437` 那行每中断一条的 trace
是诱因，可以另行降级，但去掉它只会把余量变大，不能消除活锁。

**来源**：2026-09-26 复核 `faa117f7` 时由 kernel/task 会话报告，本会话用 QEMU monitor 复核。
