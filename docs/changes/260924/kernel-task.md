# kernel-task 改动记录（2026-09-24）

## `Thread::Current::kill()` 的断言条件写反了（2026-09-24）

**文件**
- `kernel/kernel/task/thread.cpp` — `Thread::Current::kill()` 的 `DEBUG_ASSERT` 取反

**症状**
潜伏，当前没有调用者。一旦有线程调用它来杀自己（P1-A 的 fault-kill 会走到同一类路径），
dbg_1 镜像会对每一个普通线程断言失败；唯一能通过断言的却是 idle 线程，而它恰恰是
唯一不能被杀的。

**原因**
断言的消息写的是"试图杀 idle 线程"，条件却是 `is_idle_thread()` 为真。`DEBUG_ASSERT`
在条件为假时触发，所以它实际表达的是"当前线程必须是 idle"。消息和条件说的是相反的
两件事，读的人照消息理解，编译器照条件执行。

idle 线程不能死，是因为它是 CPU 在没有别的可跑时运行的东西：调度器在空队列上总能
挑到它，杀掉它之后，这颗 CPU 下一次无事可做时就挑不出任何线程。

**改法**
改前：

```cpp
    DEBUG_ASSERT(current_thread->is_idle_thread(), "Try to kill the idle thread");
```

改后：

```cpp
    DEBUG_ASSERT(!current_thread->is_idle_thread(), "Try to kill the idle thread");
```

**参照**
`Zircon Thread::Current::Kill()`：`DEBUG_ASSERT(!current_thread->IsIdle())`。
`Thread::Current::Exit()` 同样断言 `!IsIdle()`。

**验证**
构建通过；两个镜像启动正常。没有调用者，所以这条断言本身未被执行到。

**故意没做**
没有给 `Thread::Current::exit()` 补同样的断言：P1-A 计划第 20 条要在 `exit` 入口加
中断与抢占状态的断言，届时一起加，避免同一个函数被改两次。

## `suspend_or_kill` 关中断、关抢占后再拿 PI 锁（2026-09-24）

**文件**
- `kernel/kernel/task/thread.cpp` — `Thread::suspend_or_kill()` 的 Guard 加上 `IrqSaveAndPreemptDisable`

**症状**
潜伏，当前没有调用者。P1-A 的 fault-kill 让线程杀自己之后，这里会在一次时钟中断上自锁死：
CPU 停在自旋里，不 panic，也不再打任何日志。

**原因**
`Guard thread_guard{get_pi_lock()}` 由推导指引变成 `Guard<SpinLock>`，不带任何选项——
它只拿锁，不关中断也不关抢占。

线程对自己调用 `suspend_or_kill` 时，持有的是**自己的** PI 锁。这时来一个时钟中断，
中断出口走 `Thread::Current::preempt()`，它要拿的正是当前线程的 PI 锁。同一颗 CPU、
同一把锁、没有第二个参与者，自旋永远等不到释放。只要持锁期间可能被中断，而中断路径
又可能拿同一把锁，这把锁就必须在关中断下持有——这是自旋锁最基本的规则，与锁是谁的无关。

旁边的 `resume()` 已经用 `Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable>`；
`suspend_or_kill` 是漏掉的那个。关抢占也需要：持锁期间被抢占，下一个线程在同一颗 CPU
上去拿这把锁，结局一样。

**改法**
改前：

```cpp
    Guard thread_guard{get_pi_lock()};
```

改后：

```cpp
    Guard<SpinLock, lockdep::IrqSaveAndPreemptDisable> thread_guard{get_pi_lock()};
```

**参照**
- `Linux try_to_wake_up()`：`raw_spin_lock_irqsave(&p->pi_lock, flags)`。本项目 Thread 的两把自旋锁照 Linux 建模。
- `Zircon Thread::SuspendOrKillInternal()`：整个事务在 `ChainLockTransaction::UntilDone(PreemptDisableAndIrqSaveOption, ...)` 下执行。

**验证**
构建通过；两个镜像启动正常。没有调用者，改动本身未被执行到。

**故意没做**
同文件里还有几处 `Guard guard{get_pi_lock()}`（`do_suspend`、`exit` 等），没有一并改。
P1-A 计划第 20 条会逐个看：有的调用点已经在外层关了中断，只加 Guard 选项不一定对，
要按各自的上下文判断，不适合在这一条里批量改。
