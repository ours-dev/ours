# kernel/object 改动记录（2026-09-25）

## 进程在最后一个线程离开时进入 Dead，并在锁外关掉它的全部句柄（2026-09-25）

**文件**
- `kernel/object/process_dispatcher.cpp` — `detach_thread()` 在线程表清空时置 Dead；`kill()` 与 `detach_thread()` 都在放锁之后调新增的 `finish_dead_transition()`
- `kernel/object/include/ours/object/process_dispatcher.hpp` — 声明 `finish_dead_transition()`
- `kernel/object/handle_table.cpp`、`include/ours/object/handle_table.hpp` — 新增 `HandleTable::clean()`，析构函数改为调它

**症状**
- 一个进程的线程全部正常退出后，进程停在 Running，永远不会 Dead。只有"kill 时恰好没有线程"这一条路能到 Dead。
- 进程的句柄只在进程对象析构时（`~HandleTable`）才关。只要有人还持有这个进程对象的引用——包括它自己句柄表里
  一个指向自己的句柄——它持有的所有句柄就一直开着：channel 的对端看不到 PEER_CLOSED，等它的人一直等。

今天没有触发：fault_probe 被杀后，进程对象恰好很快被销毁，句柄随之关掉。文件系统计划后面的步骤（K5 的
`process_exit`、K7 的 TERMINATED、K24 的 get_info 拿进程句柄）都建立在"进程死了就是死了"之上。

**原因**
"进程结束"和"进程对象被释放"是两件事，前者由线程决定，后者由引用计数决定。Zircon 把两者分开：最后一个线程离开时
`RemoveThread()` 置 DEAD，随后 `FinishDeadTransition()` 立即关句柄、拆地址空间、发 TERMINATED；进程对象本身
可以因为别人还拿着它的句柄而活得更久。ours 只有后一半，于是进程的"死"被推迟到最后一个引用消失——而这个时刻
可能永远不来。

**改法**
改前：

```cpp
auto ProcessDispatcher::detach_thread(ThreadDispatcher &thread) -> Status {
    verify_canary();

    Guard guard{get_lock()};
    thread.mark_dead();
    static_cast<void>(m_thread_list.erase(m_thread_list.iterator_to(thread)));
    return Status::Ok;
}
```

```cpp
    if (m_thread_list.empty()) {
        m_state = State::Dead;
        return;
    }
```

改后：

```cpp
auto ProcessDispatcher::detach_thread(ThreadDispatcher &thread) -> Status {
    verify_canary();

    auto became_dead = false;
    {
        Guard guard{get_lock()};
        thread.mark_dead();
        static_cast<void>(m_thread_list.erase(m_thread_list.iterator_to(thread)));
        if (m_thread_list.empty()) {
            DEBUG_ASSERT(m_state == State::Running || m_state == State::Dying, ...);
            m_state = State::Dead;
            became_dead = true;
        }
    }

    if (became_dead) {
        finish_dead_transition();
    }
    return Status::Ok;
}

auto ProcessDispatcher::finish_dead_transition() -> void {
    auto const closed = m_handle_table->clean();
    log::debug("object: process '{}' (koid {}) dead, {} handles closed", name(), koid(), closed);
}
```

`kill()` 在没有线程时同样置 `became_dead`，放锁后调 `finish_dead_transition()`。

- **只调一次**：两条路都只在 Running/Dying → Dead 这一条边上、持锁时置位，所以 `finish_dead_transition()` 恰好
  运行一次，不需要 Zircon 的 `completely_dead_` 标志。
- **锁外**：关句柄可能跑到对象的 `on_zero_handles()`，它要拿那个对象的锁，不能嵌在进程锁里。
- **`HandleTable::clean()`**：就是原析构函数的循环，抽出来并返回关了几个。逐个在锁内摘、锁外销毁；Zircon 的
  `Clean()` 是锁内整表换出、锁外一次销毁，这里做不到，因为 Handle 的侵入式钩子只能挂在一个链表类型上，没有第二个
  链表可换。代价是每个句柄一次锁往返，每个进程只走一次。
- 句柄表对象本身保留（空的），随进程对象一起走，`handle_table()` 对仍持有进程的人始终有效。

**参照**
- Zircon `ProcessDispatcher::RemoveThread()`（`zircon/kernel/object/process_dispatcher.cc:482`）：线程表清空时
  `SetStateLocked(State::DEAD)`，放锁后调 `FinishDeadTransition()`。
- Zircon `ProcessDispatcher::FinishDeadTransition()`（同文件 :547）：`shareable_state_->DecrementShareCount()`，
  计数归零时调 `HandleTable::Clean()`（`zircon/kernel/object/include/object/shareable_process_state.h:64`）。
- Zircon `HandleTable::Clean()`（`zircon/kernel/object/handle_table.cc`）：锁内摘空，锁外经 `HandleOwner` 析构逐个关。

**验证**
- 构建通过；clang-format 干净，clang-tidy 在改动行上无新告警。
- dbg_1（默认配置两次、`SMP=1` 一次）：10 个 fault_probe 各有一行
  `object: process 'fault_probe' (koid K) dead, 1 handles closed`，且按 koid 逐一核对，每一行都早于同一 koid 的
  `process … destroyed`；`probes done (10 killed, 0 survived)`。
- 副作用可见：shell 现在在进程 Dead、句柄关闭的那一刻就看到 PEER_CLOSED，进下一个探针，不再等进程对象析构——
  日志里上一个探针的 destroyed 行会出现在下一个探针开始之后。
- dev：init 收到 "pong"，到 `--- end ---`。

**故意没做**
- Zircon `FinishDeadTransition()` 的另外几件：拆地址空间（计划 K6）、发 TERMINATED（K7）、从 job 摘除（job 还没有
  子进程列表）、critical-to-job 处理（没有这个概念）。
- `create_shared()` 的共享句柄表：它还没有实现；将来两个进程共用一张表时，`clean()` 必须改成 Zircon 那样按共享计数，
  最后一个进程死时才清。在 `finish_dead_transition()` 的注释之外不做预留。
