# driver/clock 改动记录（2026-09-25）

## i8253 的锁改为关中断持有（2026-09-25）

**文件**
- `kernel/driver/clock/i8253.cpp` — 四处 `Guard guard{m_lock}` 改为 `Guard<sync::SpinLock, IrqSave>`，`m_lock` 上写明为什么

**症状**
潜伏，当前没有观察到。条件：PIT 作为 tick 设备时，线程上下文里正在给通道 0 写两字节的 reload 值（arm 一个定时器、或
切换模式），同一颗 CPU 上来了 tick 中断，中断经定时器队列调 `set_next_event_tick()` 再拿 `m_lock`——自旋锁的持有者就是
这颗 CPU 自己：dbg_1 在 `SpinLock::acquire()` 的 `DEBUG_ASSERT(!is_held())` 停下，release 版永远自旋（中断里不会被抢占，
没有人能放锁）。即使没有中断，持锁线程被抢占、同 CPU 下一个线程再 arm 定时器，也是同一个结局。

**原因**
`lockdep::Guard` 为每个选项标签构造一个 state saver，放在 `std::tuple<StateSaver<Lock, Options>...>` 里；推导指引
`Guard(LockType &, Option...) -> Guard<LockType, Option...>` 把最自然的写法 `Guard guard{m_lock}` 推成
`Guard<SpinLock>`——saver 元组是空的，既不关中断也不关抢占。`SpinLock::acquire()` 本身只做 CAS，不碰任何 CPU 状态
（`spin_lock.hpp` 的 `DEFAULT_OPTIONS` 注释自己承认 "holding this lock disables nothing at all"）。所以原来这把锁是一把
裸锁：

```cpp
    auto set_next_event_tick(Ticks delta) -> Status {
        Guard guard{m_lock};
        arch::outb(PIT_CH0, static_cast<u8>(delta & 0xffu));        // LSB
        arch::outb(PIT_CH0, static_cast<u8>((delta >> 8) & 0xffu)); // MSB
        return Status::Ok;
    }
```

自旋锁的正确性建立在"持有者不会在持锁期间让出 CPU"上。持锁的线程一旦被抢占或被中断，而下一个在这颗 CPU 上运行的
代码要同一把锁，它等的是一个不会再跑的持有者。这把锁恰好两种上下文都有：`set_next_event_tick()` 既在 tick 中断里被调
（定时器队列重编程下一次 one-shot 事件），也在线程上下文里被调（arm 定时器），模式切换在 tick 设备初始化时的线程上下文里。

**改法**
四处都改为 IrqSave：

```cpp
    auto set_next_event_tick(Ticks delta) -> Status {
        Guard<sync::SpinLock, IrqSave> guard{m_lock};
        arch::outb(PIT_CH0, static_cast<u8>(delta & 0xffu));        // LSB
        arch::outb(PIT_CH0, static_cast<u8>((delta >> 8) & 0xffu)); // MSB
        return Status::Ok;
    }
```

`m_lock` 的声明处写明两字节写序列为什么要串行化、为什么必须 IrqSave。

没有用 PreemptDisable：它只挡住抢占，挡不住同 CPU 的 tick 中断再来拿锁。

**参照**
Linux `drivers/clocksource/i8253.c`：`i8253_lock` 在 `pit_read()` 里用 `raw_spin_lock_irqsave()`；clockevents 回调
（`pit_set_periodic()`、`pit_next_event()`）本身在关中断下被调用，所以用 `raw_spin_lock()`。两者合起来就是"只在关中断时持有"。

**验证**
- 构建通过；clang-format 干净；clang-tidy 在改动行上无新告警。
- QEMU：dev 到 `init: shell closed its channel` 和 `--- end ---`；dbg_1 到 `shell: probes done (11 killed, 0 survived)`
  和 `--- end ---`，无断言。
- 这只说明改动没有破坏启动。原来的竞争没有复现过，也没有核对当前启动里 PIT 是否一直是 tick 设备（per-CPU 的 LAPIC
  定时器会顶替它，见 `TickDevice::check_preferred()`），所以不能说这条路径被充分地走过。

**故意没做**
- 框架层让 `Guard<SpinLock>`（不带选项）编译失败，是 sync/lockdep 的一条单独改动，等全树的调用点都写明选项之后再落。
