# kernel/task 改动记录（2026-09-25）

## 线程、等待队列、IPI、CPU stopper 的自旋锁都写明选项（2026-09-25）

**文件**
- `kernel/kernel/task/thread.cpp` — `do_suspend()` 与 `exit()` 的 PI 锁改为 `IrqSaveAndPreemptDisable`；`detach()`、
  `bind_user_thread()`、`destroy()` 的 PI 锁和资源锁改为 `IrqSave`
- `kernel/kernel/task/wait_queue.cpp` — 五处队列锁改为 `IrqSave`
- `kernel/kernel/task/owned_wait_queue.cpp` — `disown_all_queues()` 两把锁 `IrqSave`；`override_thread_base_profile()` 的
  PI 锁 `IrqSaveAndPreemptDisable`
- `kernel/kernel/task/ipi_service.cpp` — mailbox 锁两处 `IrqSave`，`Mailbox` 的注释补上"为什么中断论证不够"
- `kernel/kernel/task/cpu_stopper.cpp` — `worker_thread()` 的锁 `IrqSave`，与 `queue_work()` 一致
- `kernel/kernel/task/idle_thread.cpp` — `IdleThread::init()` 的 PI 锁 `IrqSave`
- `kernel/include/ours/task/thread.hpp`、`wait_queue.hpp` — `m_pi_lock`、`m_resource_lock`、`WaitQueue::m_lock` 声明处写明
  持有规则；注释里的示例写法改成带选项的

**症状**
都是潜伏的，当前启动里没有观察到。能触发的几条：

- `Thread::Current::do_suspend()`：持着自己的 PI 锁（中断和抢占都开着）调 `Scheduler::Current::reschedule()`。dbg_1 在
  `reschedule_common()` 入口的 `DEBUG_ASSERT(arch::irqs_disabled())` 停下；release 版在持锁期间来一个 tick，中断出口的
  `Thread::Current::preempt()` 在同一颗 CPU 上再拿这把 PI 锁，自锁死。今天没有代码设置 Suspend 信号，所以走不到
  （原 `docs/repairs/260924/kernel-task.md` 第 1 条）。
- `IpiService::sync_exec()`：发送方持着目标 CPU 的 mailbox 锁时被抢占，同 CPU 的下一个线程再向同一个目标 `sync_exec()`，
  自旋在"本 CPU 持有"的锁上。今天唯一的调用者是 prof 的 PMU 启停。
- 等待队列锁的五处：持锁时同 CPU 上一个等待者超时，定时器回调 `handle_timeout()` 经 `dequeue_waiter()` 再拿同一把锁。
- `detach()`、`bind_user_thread()`、`destroy()`、`disown_all_queues()`：持 PI 锁时来 tick，`preempt()` 若拿的是同一个
  线程的 PI 锁（持有者就是当前线程时），自锁死。

**原因**
`Guard guard{lock}` 经推导指引 `Guard(LockType &, Option...) -> Guard<LockType, Option...>` 成了 `Guard<SpinLock>`，
`lockdep::Guard` 为每个选项构造一个 saver，没有选项就一个都没有；`SpinLock::acquire()` 只做 CAS。于是：

```cpp
auto Thread::Current::do_suspend() -> void {
    ...
    {
        Guard guard{current_thread->get_pi_lock()};
        ...
            current_thread->set_state_suspend();

            Scheduler::Current::reschedule(current_thread);
```

这把锁在中断和抢占都开着的情况下持有，还被带进了 `reschedule_common()`。而同一文件里其他进调度器的入口都是：

```cpp
    Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{current_thread->get_pi_lock()};
    Scheduler::Current::reschedule(current_thread);
```

这几把锁的共同点是**都有中断上下文的持有者**：PI 锁由 tick 的 `Thread::Current::preempt()`（中断出口）和睡眠超时的定时器
回调拿；等待队列锁由 `WaitQueue::handle_timeout()` 拿；IPI mailbox 锁由 `IpiService::handle_generic()`（IPI 处理函数）拿；
CPU stopper 的锁由 `queue_work()` 从任意上下文拿。这样的自旋锁，只要有一个线程上下文的持有点开着中断，就会被本 CPU 的中断
撞上——自旋锁的前提"持有者不会在持锁期间让出 CPU"被打破，等锁的人等的是一个不会再跑的持有者。

`Mailbox` 原来的注释论证了"同一颗 CPU 不会同时以发送方和处理方身份拿同一个 mailbox 锁，所以中断撞不上"，这对中断成立，
但没有覆盖抢占：发送方持着 B 的锁被抢占，同 CPU 的下一个发送方照样撞上。

**改法**
规则只有两条，和树里已有的写法一致：

- 自旋锁一律关中断持有：`Guard<SpinLock, IrqSave>`。
- 持锁范围会进 `reschedule_common()`、或会唤醒线程（`unpark_thread()` 可能要求抢占）的，再加关抢占：
  `Guard<SpinLock, IrqSaveAndPreemptDisable>`。这让唤醒要求的重调度推迟到放锁之后，而不是在持着 PI 锁时发生。

改后：

```cpp
    auto need_exit = false;
    {
        // Interrupts and preemption both off: the guard is carried into
        // reschedule_common() through Scheduler::Current::reschedule() below,
        // ...
        Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> guard{current_thread->get_pi_lock()};
```

```cpp
        auto &mailbox = CurrentMailbox::get(cpu);
        Guard<sync::SpinLock, IrqSave> guard{mailbox.lock};
        mailbox.calls.push_back(slots[cpu]);
```

逐处的选择：

| 位置 | 选项 | 理由 |
| --- | --- | --- |
| `do_suspend()` | IrqSaveAndPreemptDisable | 进 `reschedule_common()`；与 `Current::reschedule()`、`yield()` 同形 |
| `exit()` 两处 | IrqSaveAndPreemptDisable | 进 `reschedule_common()`；外层 disabler 已关，guard 自己也说明 |
| `override_thread_base_profile()` | IrqSaveAndPreemptDisable | 改 profile 会（将来）要求重调度；Zircon 用 `EagerReschedDisableAndIrqSaveOption` |
| `detach()`、`bind_user_thread()`、`destroy()`、`disown_all_queues()` | IrqSave | 只改数据，不唤醒、不调度 |
| 等待队列锁 5 处、`disown_all_queues()` 的队列锁 | IrqSave | 超时回调在中断里拿 |
| IPI mailbox 2 处 | IrqSave | 处理函数在中断里拿；发送方防抢占 |
| `CpuStopper::worker_thread()` | IrqSave | 与 `queue_work()` 一致 |
| `IdleThread::init()` | IrqSave | PI 锁的规则 |

`do_suspend()` 的"`reschedule` 返回后锁与中断的状态约定"（原待修项要求核对的）：`reschedule_common()` 切走时把 PI 锁交给
下一个线程，本线程被恢复时 `reschedule()` 返回、PI 锁重新由本线程持有——`Thread::Current::reschedule()`、`yield()` 依赖的
是同一约定。作用域结束时 guard 放锁，saver 依次恢复进入时的抢占与中断状态；`process_pending_signals()` 在进
`do_suspend()` 前已 `local_irq_enable()`，所以恢复后两者都开，正是其后 `Thread::Current::exit()` 断言的状态。

资源锁（`m_resource_lock`）没有照它的原型 Linux `task_lock()` 只关抢占：它被 `PmNode::Current::alloc_frames()` 包在分配
外面，缺页路径在关中断下到达那里；统一用 IrqSave，少一条要单独论证的例外。

嵌套在已关中断区域里的 IrqSave（如 `handle_generic()`、`exit()` 里）代价是一次 pushf/cli/popf，换来每个持有点自己说明规则。

**参照**
- Linux `p->pi_lock`：`raw_spin_lock_irqsave()`（`kernel/sched/core.c` 的 `task_rq_lock()`、`try_to_wake_up()`）。
- Linux `cpu_stop_queue_work()` 用 `raw_spin_lock_irqsave(&stopper->lock)`，`cpu_stop_get_work()` 用
  `raw_spin_lock_irq()`（`kernel/stop_machine.c`）。
- Zircon `mp_sync_exec()`（`zircon/kernel/kernel/mp.cc`）：`arch_interrupt_save()` 之后才 `ipi_task_lock.Acquire()`；
  `mp_mbx_generic_irq()` 断言关中断后拿同一把锁。
- Zircon `Thread::Resume()`（`zircon/kernel/kernel/thread.cc`）在 `PreemptDisableAndIrqSaveOption` 事务里唤醒；
  `OwnedWaitQueue::SetThreadBaseProfileAndPropagate()` 用 `EagerReschedDisableAndIrqSaveOption`。

**验证**
- 构建通过；clang-format 干净（`ipi_service.cpp` 顺带被 clang-format 调整了 include 分组和一处 CAS 的换行，纯格式）；
  clang-tidy 在改动行上无新告警。
- QEMU：dev 到 `init: shell closed its channel`、`--- end ---`；dbg_1 到 `shell: probes done (11 killed, 0 survived)`、
  `--- end ---`，无断言。
- 上面列的竞争都没有专门复现；`do_suspend()` 今天走不到。

**故意没做**
- 没有给 lockdep 加 eager-resched 选项来完整对应 Zircon 的 `EagerReschedDisableAndIrqSaveOption`；本地 CPU 需要的是
  其中的关抢占部分。
- `docs/repairs/260924/kernel-task.md` 的第 2–4 条（`suspend_or_kill` 的双重加锁与竞争、`tid()` 读错地址）不是选项问题，
  不在此改。
