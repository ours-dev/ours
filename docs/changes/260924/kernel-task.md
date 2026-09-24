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

## `process_pending_signals` 的 Kill 分支让线程退出（2026-09-24）

**文件**
- `kernel/kernel/task/thread.cpp` — `process_pending_signals()` 重写；`exit()` 入口加两条断言；`m_signals` 的三处写入改 `ktl::write_once`
- `kernel/include/ours/task/thread.hpp` — `signals()` 改 `ktl::read_once`
- `docs/repairs/260924/kernel-task.md` — 新增 `do_suspend` 一项

**症状**
潜伏。一个线程一旦被置上 Kill，回 ring 3 的路上进入 `process_pending_signals()`，Kill 分支是空的，
信号永远不清，`while` 在关中断的状态下永远转下去：这颗 CPU 死锁，没有任何输出。

**原因**
这个函数的循环以"信号清空"为出口，每个分支都得让自己的那一位消失：Suspend 分支在 `do_suspend()`
里清；Kill 分支什么都没做；未知位同样没人清。一个分支不处理自己的位，循环就没有出口。

**改法**
改前：

```cpp
    ThreadSignals signals;
    while ((signals = current_thread->signals()) != ThreadSignals::None) {
        if (!!(signals & ThreadSignals::Kill)) {
        }

        if (!!(signals & ThreadSignals::Suspend)) {
            arch::local_irq_enable();
            Thread::Current::do_suspend();
            arch::local_irq_disable();
        }

        if (!!(signals & ThreadSignals::None)) {
        }
    }
```

改后：

```cpp
    DEBUG_ASSERT(arch::irqs_disabled(), "{}() entered with interrupts on", __func__);

    ThreadSignals signals;
    while ((signals = current_thread->signals()) != ThreadSignals::None) {
        if (!!(signals & ThreadSignals::Kill)) {
            arch::local_irq_enable();
            Thread::Current::exit(0);
        }

        if (!!(signals & ThreadSignals::Suspend)) {
            arch::local_irq_enable();
            Thread::Current::do_suspend();
            arch::local_irq_disable();
            continue;
        }

        panic("thread {} has signals {:#x} nobody handles", ...);
    }
```

- Kill 先于 Suspend：一个既被挂起又被杀的线程没有可以恢复进去的东西。
- 退出前开中断：`exit()` 里 ThreadDispatcher 的 `on_exit()` 要拿会睡眠的锁。
- 未知位直接 panic，而不是再写一个空分支让循环空转。
- 删掉 `signals & ThreadSignals::None` 那个空分支（与 None 按位与恒为零，条件永假）——主人同意删除。

`exit()` 入口断言中断开、抢占开：它的调用者（线程入口返回后的 trampoline、上面两条信号路径）都
承诺能阻塞，断言把这个承诺写死。没有断言"不持自旋锁"：`SpinLock::num_held_cpu()` 存在，但 PI 锁会
在上下文切换时交接，这个每 CPU 计数是否平衡没有核实过。

`m_signals` 由别的 CPU 在 PI 锁下写，由线程自己在回用户态的路上不加锁地读，所以读写都改成
`ktl::read_once`/`ktl::write_once`：不撕裂、不被编译器缓存或推迟。

**参照**
- Zircon `Thread::Current::ProcessPendingSignals()`（`kernel/kernel/thread.cc`）：先看 Kill，
  `arch_enable_ints(); Thread::Current::Exit(0);`；Suspend 分支挂起后再关中断。
- Zircon `Thread::Current::Exit()`：用户线程在 `ExitingCurrent()` 前 `DEBUG_ASSERT(!arch_ints_disabled())`。
- Zircon 的 `signals_` 是 relaxed 原子量。

**验证**
- 构建通过；两个镜像在 `SMP=4` 与 `SMP=1` 下都跑到 `shell: bad-pointer checks passed` 与 `--- end ---`。
- dbg_1 日志里内核线程 `laborer` 走 trampoline 退出一次，`exit()` 的两条新断言成立。
- Kill 分支本身这一条还走不到：没有代码置 Kill。第 26 条（用户态异常杀进程）之后实测。

**故意没做**
`do_suspend()` 的锁（见待修项），P1-A 不会置 Suspend。

## 勘误：P1-A 第 1–16 条对抗审查查出的记录错误（2026-09-24）

本文件只增不改，更正写在这里。

- 「`suspend_or_kill` 关中断、关抢占后再拿 PI 锁」一节的**症状**写"CPU 停在自旋里，不 panic，也不再打任何日志"。
  这只对 dbg_0 成立。dbg_1（`KERNEL_DEBUG_LEVEL=1`）里，`Thread::Current::preempt()` 在中断出口对同一把锁的
  第二次获取会触发 `SpinLock::acquire()` 的 `DEBUG_ASSERT(!is_held())`（`kernel/kernel/sync/spin_lock.cpp`），
  内核 panic。
- 同一个函数还有两处早于 P1-A 的缺陷（Parked 分支重复加锁、Sleeping 分支与定时器竞争），以及
  `Thread::Current::tid()` 读错每 CPU 槽，已记入 `docs/repairs/260924/kernel-task.md` 第 2–4 条并交接。

## 勘误：P1-A 第 17–29 条对抗审查查出的记录错误（2026-09-24）

本文件只增不改，更正写在这里。

- 「`process_pending_signals` 的 Kill 分支让线程退出」一节写 `m_signals` "由别的 CPU 在 PI 锁下写"。不完全：
  `suspend_or_kill()` 里的 `gktl::defer` 动作会执行两次（`DeferredAction::invoke()` 不标记已执行），第二次在 PI 锁释放之后，
  是一次锁外的读改写。已交接给独立会话（与 `SPAWNED_THREADS_LOCK`、可 join 线程的退出循环、Kill 不发 IPI 一起）。
- 同一节写 `process_pending_signals()` "关中断，所以检查之后到来的信号不会在返回前被错过"。只对本 CPU 成立；另一颗 CPU
  在这个窗口里置的信号不会丢，但要等下一次进内核，因为还不发 IPI。
