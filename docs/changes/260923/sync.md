## `Event::wait_etc()` 从检查到入队一直持有队列锁，不再丢唤醒（2026-09-23）

**文件**
- `kernel/include/ours/task/wait_queue.hpp`、`kernel/kernel/task/wait_queue.cpp` — 新增公开的 `park_thread_preamble_locked()`、`enqueue_waiter_locked()`；`park_thread_postamble()` 改为公开
- `kernel/kernel/sync/event.cpp` — `wait_etc()` 在队列锁下检查 `m_result` 并入队，放锁后再 park

**症状**
对抗式审查发现（高）：`Event::wait_etc()` 在队列锁下检查 `m_result`，**放掉锁**，再调 `park_thread()`，
后者重新拿锁入队。另一个 CPU 在这个缝隙里 `signal()`：看到结果未置、队列为空——置上结果、谁也不唤醒；
等待者随后入队睡下，永远醒不来。之后的每次 `signal()` 都因为「结果已置」直接返回，同样救不了它。
`object::wait_one` 整个建在这个 Event 上，第一个跨 CPU 的 channel 等待就可能中招。

**原因**
检查条件和进入等待不是在同一把锁下原子完成的——经典的丢唤醒。

**改法**
改前：

```cpp
    {
        Guard queue_guard{m_wq.get_lock()};
        if (m_result.is_some()) {
            ...
            return result;
        }
    }

    return m_wq.park_thread(current_thread, deadline, sigmask, interruptible);
```

改后：

```cpp
    {
        Guard queue_guard{m_wq.get_lock()};
        if (m_result.is_some()) {
            ...
            return result;
        }

        auto const status =
            m_wq.park_thread_preamble_locked(current_thread, deadline, sigmask, interruptible);
        if (Status::Ok != status) {
            return status;
        }
    }

    return m_wq.park_thread_postamble(current_thread, deadline);
```

`signal()` 于是要么落在检查之前（检查看得到），要么落在入队之后（找得到要唤醒的线程）。放锁之后、
真正 park 之前来的唤醒会等等待者的 PI 锁，而等待者一直持有它直到真正睡下，所以那里也不会丢。

`WaitQueue` 为此把 `park_thread()` 的两半公开给这种「在锁下检查、再等待」的用法，并在头文件里写明了
正确的调用顺序。

**参照**
Zircon `Event::WaitWorker()`（`kernel/lib/syncing/event.cc`）：它用同一把锁同时覆盖检查与 `BlockEtc()`。

**验证**
构建通过；两个镜像各启动两次，DPC 与 cpu_stopper（Event 的现有使用者）工作正常、无断言。跨 CPU 的
竞争窗口本身没有复现测试。

**故意没做**
无。

## `Event::signal()` 全程关中断（2026-09-23）

**文件**
- `kernel/kernel/sync/event.cpp` — `signal()` 开头 `arch::AutoInterruptDisabler`

**症状**
潜伏（对抗式审查，中）：`signal()` 用普通 `Guard` 拿等待队列的自旋锁，随后 `Scheduler::unpark_thread()`
也用普通 `Guard` 拿各线程的 PI 锁和运行队列锁。这些锁在持有期间中断是开着的；时钟中断要拿运行队列锁，
一旦恰好打在持锁期间，这个 CPU 就在自己拿着的锁上永远自旋。

**原因**
以前碰巧安全：`signal()` 的调用者（DPC、cpu_stopper）都在关中断的上下文里调它。是 `f34e49b` 让系统调用
开着中断运行，而 `object::wait_one` 的观察者正是在系统调用上下文里 `signal()`——这个组合把它暴露了出来。

**改法**
改前：

```cpp
auto Event::signal(Status status) -> void {
    Guard guard{m_wq.get_lock()};
```

改后：

```cpp
auto Event::signal(Status status) -> void {
    arch::AutoInterruptDisabler const irq_disabler;
    Guard guard{m_wq.get_lock()};
```

放在函数最前面、覆盖整个函数，包括锁放掉之后的 `Scheduler::unpark_thread()`——那条路径上的 PI 锁和
运行队列锁同样需要关中断。

**参照**
Zircon `Event::SignalEtc()`：`Guard<..., IrqSave>`，唤醒全程关中断。

**验证**
构建通过；两个镜像各启动两次（与上一条一起），DPC 与 cpu_stopper 正常、无断言。

**故意没做**
没有逐个把 `Scheduler::unpark_thread(UnparkList)` 里的普通 `Guard` 改成 `IrqSaveAndPreemptDisable`：
那是调度器内部的约定，从调用者这一侧整体关中断已经覆盖了现有的唯一入口。
