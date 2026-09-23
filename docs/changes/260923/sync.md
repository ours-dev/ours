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
