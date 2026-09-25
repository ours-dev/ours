# sync 改动记录（2026-09-24）

## `Mutex::release_slowpath()` 真的唤醒等待者（2026-09-24）

**文件**
- `kernel/kernel/task/owned_wait_queue.cpp` — `lock_for_notify_operation()` 从空桩改成真正出队；
  `notify_threads_locked()` 改名 `notify_threads()` 并真正唤醒
- `kernel/include/ours/task/owned_wait_queue.hpp` — 两个函数的锁约定写进声明；公开 `get_lock()`
- `kernel/kernel/sync/mutex.cpp` — `release_slowpath()` 在队列锁下出队并发布新状态，放锁后再唤醒

**症状**
任何一次 `sync::Mutex` 争用都让输掉的那个线程永远睡下去。dbg_1 镜像上 P1-A 的探针测试
约一半的启动停在 "DBG detaching from koid 31"：shell 在 `Dispatcher::detach_observer()` 里
等通道 dispatcher 的锁，QEMU monitor 看它不在任何运行队列上。

**原因**
唤醒的那一半根本没写。`OwnedWaitQueue::lock_for_notify_operation()` 是个桩，无条件返回 none：

```cpp
template <typename UnparkThreads>
auto OwnedWaitQueue::lock_for_notify_operation(usize max_notify, INotifyRequeueHook &)
    -> ustl::Option<UnparkThreads> {
    return ustl::NONE;
}

template <typename UnparkThreads>
auto OwnedWaitQueue::notify_threads_locked(UnparkThreads, INotifyRequeueHook &, NotifyOptions)
    -> void {}
```

它写于 `WaitQueue::lock_for_notify_one/all` 也还是桩的时候。后来那两个为 `Event` 补上了，这个没有。
于是 `release_slowpath()` 永远"找不到人交接"，把锁放成 `Available` 就走；而 `acquire_slowpath()`
自 `e0cbf29a` 起是真的 park 了——park 进去的线程再没有人叫醒。

为什么是"任何争用"而不只是某个竞态窗口：快路径存进 `m_state` 的总是 `thread | Contended`
（见下一条要修的），所以慢路径里 CAS 失败后看到的旧值必然带 `Contended`，自旋循环第一圈就 break，
直奔 park。争用一次，丢一个线程。

用户描述里那个竞态（等待者 CAS 失败后、入队前，持有者发布 `Available`）是真的，但它叠在这个
更大的洞上面：在这一条之前，即便等待者赶在释放之前入了队，也照样没人叫它。那一条是下一节。

**改法**
照 Zircon 把唤醒拆成两半，但锁的放法按本树的锁序改：

```cpp
// release_slowpath()
task::AutoPreemptDisabler const preempt_disabler;
ustl::Option<Thread::UnparkList> batch;
{
    Guard<SpinLock, IrqSave> guard{m_waiters.get_lock()};
    batch = m_waiters.lock_for_notify_operation<Thread::UnparkList>(1, default_hook);
    if (batch) {
        publish(new_owner | State::Contended);
    } else {
        publish(State::Available);
    }
}
if (batch) {
    m_waiters.notify_threads(ustl::move(*batch), default_hook, NotifyOptions::None);
}
```

- **出队和发布在同一把队列锁下**。等待者入队要拿这把锁，所以"队列为空"这件事在 `Available`
  写进 `m_state` 之前不会变。这是下一节等待者复查能成立的前提。
- **唤醒在放锁之后**。唤醒要拿被唤醒线程的 PI 锁，本树里 PI 锁在外、队列锁在内（见
  `WaitQueue::enqueue_waiter` 的注释），在队列锁下拿 PI 锁就是反序。Zircon 的
  `WakeThreadsLocked()` 能在队列锁下唤醒，是因为它的 ChainLock 事务已经把所有相关锁都拿着了；
  本树没有那个协议。所以名字去掉 `_locked`——留着就是在说谎。
- **从发布到唤醒关抢占**。这段时间里锁的主人是一个还没跑起来的线程；若释放者在中间被抢占，
  锁就挂在一个睡着的主人名下，其他竞争者全排在后面。Zircon `ReleaseContendedMutex()` 同样关抢占。
- 队列锁 `IrqSave`：`WaitQueue::handle_timeout` 会在中断上下文里拿队列锁。今天没人带 deadline
  在 mutex 上 park，但队列本身并不知道这一点。
- `notify_threads()` 复用 `WaitQueue::wake_dequeued_thread()`，它顺带做 PI 边的
  `REMOVE_SINGLE_EDGE` 传播，和 `notify_one()` 同一条路。

被否掉的方案：让 `lock_for_notify_operation()` 自己拿队列锁、`notify_threads_locked()` 放锁
（Zircon 的形状）。那样发布 `m_state` 就要夹在两个函数之间、持着一把别人拿的锁，读代码时看不出
锁的边界；而且和 `WaitQueue::lock_for_notify_one()`"调用者持锁"的约定相反。

**参照**
- Zircon `Mutex::ReleaseContendedMutex()`（`kernel/kernel/mutex.cc`）：
  `LockForWakeOperationOrBackoff(1u, ...)` → CAS 发布新状态 → `WakeThreadsLocked(...)`。
- Zircon `OwnedWaitQueue::LockForWakeOperationOrBackoff()`、`WakeThreadsLocked()`。
- 本树 `Event::signal()`：同样是"队列锁下 `lock_for_notify_*`、放锁后 `unpark`"。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过；clang-format 无残留；clang-tidy 在改动行上无新诊断。
- dev 镜像启动：`init: shell says "pong"`、`shell: channel checks passed`、
  `shell: bad-pointer checks passed` 都在。
- dbg_1 镜像启动：`shell: channel checks passed`、`shell: bad-pointer checks passed` 都在。
- P1-A 的探针测试不在树里（暂存在会话外），这一步没有跑它。这一步只补了唤醒，竞态窗口还在，
  下一节补。

**故意没做**
- 竞态窗口（等待者在队列锁外判断要不要 park）——下一节，单独一步。
- 优先级继承：`notify_threads()` 不把队列的 owner 转给新主人。PI 的加锁协议还是占位符，
  见 `docs/repairs/260915/sync-mutex.md` 第 1 条。
- `~Mutex` 不等队列锁放掉：发布 `Available` 之后、放队列锁之前，别的线程可以走快路径拿锁、放锁、
  销毁 mutex，放锁那一下就写到了已释放的内存。Zircon 在 `Mutex::~Mutex()` 里自旋等 OWQ 锁放开来
  挡这个。本树的 `Mutex` 没有析构函数，另记待修。
