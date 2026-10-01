# kernel/task 待修项（2026-09-26）

按严重程度排。本文件只收 2026-09-26 per-CPU 访问审计新发现的条目；
`WaitQueue::unpark_thread` 对 PI 锁重复加锁、`Thread::Current::tid()` 读错地址两条早已记在
`docs/repairs/260924/kernel-task.md`（2、4 条），不在这里重复。

## 2. 启动时 BSP 切到 idler0 之后、`init_percpu()` 之前，BSP 是可抢占的

**现象**：潜伏，当前触发不到。

**原因**：中断在 `InitLevel::Irq`（`platform_init_apic`，`platform/pc/interrupt.cpp:110/113`）就打开了，
早于 `task::init()`（`top/main.cpp:177`）。`kernel/kernel/task/init.cpp:27` 让 idler0 成为 BSP 的
当前线程，而 idler0 刚被 `spawn_etc` 建出来，`PreemptionState` 计数是 0
（`preemption_state.hpp:25`；`IdleThread::init`，`idle_thread.cpp:54-69`，从不关抢占）；
计数要到整个循环之后的 `init_percpu()`（`init.cpp:74`）才加上。`create_first()` 的注释
（`thread.cpp:138-139`）说 idler「自带计数」，在这个窗口里不成立。

坏法：:27 到 :35 之间来一个中断，退出时如果本地有挂起的调度请求，`is_preemption_enabled()` 为真
（`irq/mod.cpp:92`），走 `Thread::Current::preempt()`，`Scheduler::Current::preempt` 里
`DEBUG_ASSERT(curr == m_curr_thread)` 因 `m_curr_thread` 还是空而触发（`scheduler.cpp:1011`）。
今天没有东西能在这个窗口里设挂起位：`TickSched` 到 `InitLevel::Threading` 才启动
（`tick_sched.cpp:50-52, 78-82`），还没有 AP 所以没有调度 IPI，也没有睡着的线程可唤醒。

**照谁改**：让这段在关抢占（或关中断）下完成，或者让 idler 从创建起就带着一次关抢占，
与注释的说法一致。Linux 的 idle 任务从 `init_idle()` 起就带 `preempt_count` 的
`PREEMPT_DISABLED`（`init_idle_preempt_count()`）。

## 3. 中断收尾不看被打断现场的 IF，只开 IrqSave 的自旋锁临界区里发生同步异常时可能被抢占

**现象**：潜伏。需要在 `Guard<SpinLock, IrqSave>` 的临界区里触发一个会返回的同步异常
（内核 `#PF` 被解决、`#BP`、`#DB`），并且该临界区里刚好设了本地调度请求。

**原因**：`irq::handle_irq_postamble()`（`kernel/kernel/irq/mod.cpp:85-93`）只看
`PreemptionState::is_preemption_enabled() && take_pending_local()`，不看被打断的 `frame` 里的 IF。
只用 `IrqSave`（不带 `PreemptDisable`）的临界区里抢占计数是 0、中断关着，可屏蔽中断进不来，
但同步异常能进来；如果临界区里的唤醒（`reschedule_mask()`，`scheduler.cpp:293-322`）设了本地
挂起位，异常收尾就会在持着自旋锁的情况下 `Thread::Current::preempt()`。

**照谁改**：Linux `irqentry_exit()` 只在 `!regs_irqs_disabled(regs)` 时才考虑
`preempt_schedule_irq()`（`kernel/entry/common.c`）。这里在 `arch_handle_exception()`
（`faults.cpp:433`）或 postamble 里加上「被打断时中断是开着的」这个条件。

## 4. `PreemptionState` 的标志位在线程上下文里做非原子读-改-写，会被中断里的更新覆盖

**现象**：只影响 `NoReschedPreempt` 这个提示位，计数和抢占的实际投递不受影响。

**原因**：中断的 preamble/postamble 改的是被打断线程的同一个 `m_counter`
（`irq/mod.cpp:62-64, 88-92`），而线程上下文里的读-改-写是开着中断做的：独立使用的
`AutoPreemptDisabler`（`object_cache.cpp:706`、`cpu_stopper.cpp:75/101/138`、`mutex.cpp:75/282`）、
`LocalLock`（`local_lock.cpp:15-19`）、`set_timeslice_extension`（`mutex.cpp:47/112/196`）、
`init_percpu`（`init.cpp:74`）。两个计数能保住，因为中断对它们的净改变是 0；标志位保不住：
线程在 `enable_preemption` 里读了 `m_counter`，时钟中断里 `mark_preempt_pending -> add_pending`
把 NRP 置 false，postamble 因为计数还是 1 不抢占，线程随后写回旧的 NRP=true。
`take_pending_local()` 看的是 `m_pendings` 而不是 NRP（`preemption_state.hpp:132-141`），
所以抢占照样送达；NRP 只被 `unpark_preempt`（提示）和 Mutex 自旋循环（`mutex.cpp:133`）读，
丢了只会让让出推迟到自旋超时或下一个 tick。

**照谁改**：Linux 的 `preempt_count` 是 per-CPU 的，用一条中断原子的 `incl %gs:__preempt_count`
修改。这里 `preemption_state.hpp:20` 已经有 `TODO: From per thread to per scheduler`；
在那之前，至少让线程上下文的读-改-写对中断原子（关中断，或把标志位移出与中断共享的字）。

## 5. `CpuStopper` 的 `stop_cpus` 只等到第一个 CPU 完成就返回，且不加锁重写别的 CPU 仍在队列里的节点

**现象**：潜伏，当前不可达：`CpuStopper` 没有任何外部调用者，per-CPU 的 `CpuStopper` 没人初始化，
`worker_thread` 也没被 spawn。

**原因**（`kernel/kernel/task/cpu_stopper.cpp`）：
- `num_todo = cpus.count()`（:38）从不递减，worker 每完成一个就 signal（:140-143），于是
  `stop_cpus` 在第一个 CPU 完成后就返回、放掉 `STOP_CPUS_MUTEX`；后面的 worker 往已经死掉的
  栈帧上写 `done->retval` 和 completion。
- :106 的 `std::construct_at(&stopper.m_stop_work, ...)` 不拿 `stopper.m_lock` 就重写一个可能还挂在
  那个 CPU `m_works` 链上的节点（连链表钩子一起）。
- `stop_cpus_nowait` 分配了 `works` 然后不用（:64-69，泄漏），也不拿互斥锁。

与抢占无关：`Current::get(cpu)` 显式指定 CPU，调用者被迁走也不影响；:101 的
`AutoPreemptDisabler` 其实可以去掉。

**照谁改**：Linux `stop_cpus()`/`__stop_cpus()`（`kernel/stop_machine.c`）用
`cpu_stop_done` 的 `nr_todo` 原子计数，每个 worker `cpu_stop_signal_done()` 递减到 0 才
`complete()`；入队在 `cpu_stopper.lock` 下做（`cpu_stop_queue_work()`）。

**来源**：以上各条均出自 2026-09-26 per-CPU 访问审计，已对照代码复核。
