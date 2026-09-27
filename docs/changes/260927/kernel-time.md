## Timer 和 ClockEventDevice 的处理函数改用 ustl::function::Function（2026-09-27）

**文件**
- `kernel/include/ours/time/timer.hpp`：`Timer::Handler` 改为 `Function`
- `kernel/include/ours/time/clock_event_device.hpp`：`Handler` 改为 `Function`；
  `get_event_handler()` 改为 `take_event_handler()`
- `kernel/kernel/time/tick_device.cpp`：`rebind_device()` 改用 `take_event_handler()`

**症状**
潜伏，当前触发不到。两个 Handler 都是 `std::function`，都在中断上下文里被调用。
目前所有 handler 都放得进 `std::function` 的小缓冲区：`TickSched::handle_event`
是函数指针，wait_queue 和 thread 的超时 lambda 只捕获一个指针。一旦哪个
handler 捕获的东西超过缓冲区，`std::function` 就会在 `activate()` 或
`set_event_handler()` 里悄悄调用 `operator new`。`activate()` 往往在持有自旋锁
时被调用，而且这个内核的全局 `operator new` 只是一个 panic 桩。

**原因**
`std::function` 把"放不放得下"当成实现细节，调用方看不到，编译器也不会报。
中断上下文里的回调需要的恰恰是编译期保证"不会分配"。
`ustl::function::Function` 把 24 字节的上限写进了类型约束，超出就编译不过。

**改法**

之前：

```cpp
using Handler = ustl::function::Fn<auto(Timer *)->HandlerResult>;      // = std::function

auto set_event_handler(Handler handler) -> void { m_event_handler = handler; }
auto get_event_handler() -> Handler { return m_event_handler; }

// rebind_device()
handler = m_clkdev->get_event_handler();
next_event = m_clkdev->next_event();
m_clkdev->set_event_handler(nullptr);
```

之后：

```cpp
using Handler = ustl::function::Function<auto(Timer *)->HandlerResult>;

auto set_event_handler(Handler handler) -> void { m_event_handler = ustl::move(handler); }
auto take_event_handler() -> Handler { return ustl::move(m_event_handler); }

// rebind_device()
handler = m_clkdev->take_event_handler();
next_event = m_clkdev->next_event();
```

`Function` 只能移动，不能拷贝，所以原来的 getter 写不成了。不过 getter 唯一的
调用方本来就是"拷出来，再置空"，这两步合起来就是一次 take。这样一来，
handler 在任何时刻都只有一个持有者，旧设备被取走 handler 后自然变成空的。

**参照**
Linux 的 `clockevents_exchange_device()`：它把旧设备的 handler 设成
`clockevents_handle_noop()`，然后通过 `tick_setup_device()` 把原 handler
交给新设备。这里的 take 对应"交出后旧设备不再持有"这一半。
Zircon 的 `Timer` 用的是 `timer_callback`，即函数指针加 `void *arg`，不涉及分配。

**验证**
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过。
- QEMU 实测：
  - dev 启动 45 s，`usercopy.test` 18/18 通过，无 panic；
  - dbg_1 启动 60 s，无 panic，时间持续推进，线程持续被调度，说明 tick 和
    定时器超时都正常。
- clang-tidy：改动行无新告警。`GCED` 和 `GCED_MUTEX` 的命名告警是原有的。

**故意没做**
`Timer::activate(deadline, f, args...)` 仍然用 `bind_front` 把参数绑进 handler。
如果绑定后超过 24 字节，现在会在调用处编译失败，这正是想要的效果，所以没有
为它另外开口子。
