# kernel/sync — 改动记录

格式见 `docs/changes/TEMPLATE.md`。

---

## 1. `Mutex` 在竞争时真的阻塞（2026-09-15）

**文件**
- `kernel/kernel/task/owned_wait_queue.cpp` — `park_and_assign_owner()` 真的去 park。
- `kernel/kernel/sync/mutex.cpp` — `acquire_slowpath()` 自旋失败后阻塞。
- `kernel/include/ours/sync/mutex.hpp` — `acquire()` 重试而不是丢掉结果。

**症状**

潜伏到多核起来为止。竞争路径上，**输掉的线程直接从 `acquire()` 返回并进入临界区**，
而 `m_state` 里存的是另一个线程的指针。

**原因**

两处，一处套着另一处。

其一，`Mutex::acquire_slowpath()` 自旋失败后什么都不做：

```cpp
    // Failed to wait a lock release. Make current thread sleep

    // Every exit above either returns false on a successful CAS or breaks
    // here: ...  None of those acquired the mutex.
    return true;
```

注释说「让当前线程睡下去」，后面**没有任何睡眠**。`m_waiters` 在 `mutex.cpp` 里只出现在
`release_slowpath()`——也就是**唤醒那一半写好了，入队那一半没写**，于是释放方每次都在一个
永远为空的队列里挑后继。

其二，`Mutex::acquire()` 是 `-> void`，把 `acquire_common()` 的 `bool` 丢掉：

```cpp
inline auto Mutex::acquire(Duration spining_duration) -> void {
    m_canary.verify<Self>();
    acquire_common(spining_duration, TimesliceExtension<false>{});
}
```

注意这个 `bool` 的极性和直觉相反：CAS **成功**时返回 `false`，返回 `true` 才是「你还没拿到」。
丢掉它，竞争失败就成了静默的空操作。

**改法**

`acquire_slowpath()` 末尾改成真的阻塞，并说明返回 `false` 是**交接**而不是再抢一次：

```cpp
auto const status = m_waiters.park_and_assign_owner(
    current_thread, time::Deadline::infinite(), task::Interruptible::No);
if (Status::Ok != status) {
    return true;   // 没能挂起，让调用方重试，而不是当作持锁
}
// release_slowpath() 在唤醒之前就 publish() 了新主人，所以醒来即持锁。
return false;
```

`acquire()` 改成重试到真的持锁：

```cpp
while (acquire_common(spining_duration, TimesliceExtension<false>{})) {
}
```

而它要调的 `OwnedWaitQueue::park_and_assign_owner()` 原本也是空壳
（`verify_owned(); return Status::Ok;`），现在走 `WaitQueue::park_thread()` 真的挂起。
PI 锁在它内部取、且连中断和抢占一起关——这个 guard 会被 `park_thread()` 带进
`reschedule_common()`，那正是后者要求的状态，`Event::wait_etc()` 出于同样理由这么写。

**参照**

`Linux __mutex_lock_common()`：自旋失败后 `__mutex_add_waiter()` 入队再
`schedule_preempt_disabled()`，醒来由 handoff 决定是否直接持锁。
`Zircon Mutex::AcquireContendedMutex()` 同样是「入队 + 阻塞 + 交接」。

**故意没做**

- **没有接上优先级继承**。`assign_owner()` 那条路要走 PI 链，而 `begin_propagate()` 断言
  属主的 PI 锁已被持有；在没有全局 thread_lock 的前提下取一串线程锁，需要
  `lock_for_park_and_assign_owner_or_backoff()` 这个占位符所预留的 ChainLock 式退避协议。
  **不带继承地阻塞，是一把可能倒置优先级的正确互斥锁；带着半成品的链遍历阻塞，是死锁。**
  已记入待修。
- 没有动 `try_release()`。它构造的期望值不带 `Contended`，而 `acquire` 存进去的总是带，
  所以那个 CAS 永远失败、释放永远走慢路径。不是正确性问题（空队列时 `publish(Available)`），
  但那条快路径是死的，另记待修。

**验证**

构建通过；QEMU dbg 四核实测无回归：无断言、15.0 万行线程输出、ring 3 的用户态 shell
照常列出命令注册表、QEMU 存活。
