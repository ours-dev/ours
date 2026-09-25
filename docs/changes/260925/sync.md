# kernel/sync 改动记录（2026-09-25）

## Mutex、Event 持有等待队列锁时都写明 IrqSave（2026-09-25）

**文件**
- `kernel/kernel/sync/event.cpp` — `unsignal()` 原来裸拿等待队列锁，改为 IrqSave；`wait_etc()`、`signal()` 的队列锁写明 IrqSave
- `kernel/kernel/sync/mutex.cpp` — `acquire_slowpath()` 的队列锁写明 IrqSave

**症状**
`Event::unsignal()`：潜伏。条件：线程在 `unsignal()` 里持着事件等待队列的锁时，同 CPU 上这个队列里某个等待者的超时到期，
定时器回调 `WaitQueue::handle_timeout()` 在中断里拿同一把锁——持有者是这颗 CPU 自己，dbg_1 断言
`SpinLock::acquire()` 的 `!is_held()`，release 版永远自旋。持锁线程被抢占、同 CPU 下一个线程 `signal()` 同一个事件，
也是一样。

另外三处当前没有问题：它们外面已经有关中断的东西（PI 锁的 `IrqSaveAndPreemptDisable` guard，或 `signal()` 里显式的
`arch::AutoInterruptDisabler`）。

**原因**
`Guard guard{lock}` 经推导指引变成 `Guard<SpinLock>`，saver 元组为空，什么状态都不保存；`SpinLock::acquire()` 本身也不碰
中断和抢占。改之前：

```cpp
auto Event::unsignal() -> void {
    Guard guard{m_wq.get_lock()};
    m_result.reset();
}
```

等待队列的锁两种上下文都会拿：线程上下文（park、signal、unsignal）和中断上下文（超时回调 `handle_timeout()` 摘下并唤醒
等待者）。这样的自旋锁只要有一个持有点开着中断，就会被同 CPU 的中断撞上。

另外三处是"碰巧安全"：

```cpp
    Guard<SpinLock, IrqSaveAndPreemptDisable> pi_guard{current_thread->get_pi_lock()};
    {
        Guard queue_guard{m_waiters.get_lock()};
```

队列锁本身什么都没关，全靠外面那一层。外层一旦调整（比如把 PI 锁的范围收窄），里面就悄悄变成裸锁，而写法上看不出任何区别。

**改法**
四处都写成 `Guard<SpinLock, IrqSave>`：

```cpp
auto Event::unsignal() -> void {
    Guard<SpinLock, IrqSave> guard{m_wq.get_lock()};
    m_result.reset();
}
```

```cpp
    Guard<SpinLock, IrqSaveAndPreemptDisable> pi_guard{current_thread->get_pi_lock()};
    {
        Guard<SpinLock, IrqSave> queue_guard{m_waiters.get_lock()};
```

嵌套在已关中断的区域里时，`AutoInterruptDisabler` 保存的是"已关"，析构时恢复的也是"已关"，代价是一次 pushf/cli/popf。
换来的是每个持有点自己说明它在关中断下持锁，不依赖周围。

`Event::signal()` 里原有的 `arch::AutoInterruptDisabler` **保留**，队列锁同时写 IrqSave。它们不重复：`signal()` 在
`guard.unlock()` 之后还要调 `Scheduler::unpark_thread()`，那一段仍需关中断（要拿 PI 锁和运行队列锁）。现在 Guard 的 saver
是成员，活到 Guard 析构而不是 `unlock()`，所以只写 IrqSave 在今天也能覆盖；但这不是承诺——Zircon 的
`Guard::Release()` 经 `IrqSavePolicy::Release()` 在放锁的同时恢复中断，`lockdep/docs/interface.md` 也要把 Guard 改成
LockPolicy 的形状。到那时只剩 Guard 的话，`unpark_thread()` 就会在开中断下运行。显式的 disabler 把这一段的要求写在它自己身上。

**参照**
- Zircon `Event::WaitWorker()` / `Event::Signal()`（`zircon/kernel/kernel/event.cc`）：等待队列的锁在
  `ChainLockTransaction::UntilDone(IrqSaveOption, ...)` / `(EagerReschedDisableAndIrqSaveOption, ...)` 里拿，即整段关中断。
- Zircon `IrqSavePolicy<LockType>::Release()`（`zircon/kernel/include/kernel/spinlock.h`）：`lock->Release()` 之后紧接
  `arch_interrupt_restore()`，放锁即恢复中断——保留 `signal()` 里那个 disabler 的理由。

**验证**
- 构建通过；clang-format 干净；clang-tidy 对两个文件无告警。
- QEMU：dev 到 `init: shell closed its channel`、`--- end ---`；dbg_1 到 `shell: probes done (11 killed, 0 survived)`、
  `--- end ---`，无断言。
- `unsignal()` 与超时撞上的竞争没有专门复现。

**故意没做**
- 没有把 `signal()` 的显式 disabler 改成依赖 Guard saver 的生命期，理由见上。
