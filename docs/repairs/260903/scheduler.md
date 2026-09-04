# 调度器 — 待修

按严重程度排。已修的从这里删掉，改动记录在 `docs/changes/260903/scheduler.md`（第 1–19 条）。

---

## 1. 没有中断出口的抢占兑现，也没有中断嵌套计数（arch/x86 的 lane，交叉索引）

`PreemptionState::IrqNestedCounter` 全树无人增减；`x86_dispatch_exception()` 出口不检查
pending 抢占。后果：changes 第 15 条把 `reschedule_mask()` 的本地抢占改成只记 pending 之后，
一个正在运行的线程只会在自己 `enable_preemption()` 或空闲循环时被换下——tick 里
`RoundRobin::update_thread()` 标的 pending 不会在中断返回时兑现，**时间片轮转事实上不生效**，
一个忙循环线程会一直占着 CPU。Zircon：`x86_exception_handler` 出口对 `preempts_pending` 调
`Thread::Current::Preempt()`，进入中断时 `arch_set_blocking_disallowed(true)`。


## 2. `Scheduler::init()` 把每个调度器都记成 BSP

`m_runtime.which_cpu = Cpu::id()`，而 `task::init()` 在 BSP 上替每颗 CPU 调它。应接收 cpunum。

## 3. `Driver::update_clock()` 的基准写错了

`m_prev_real_clock = m_sched_clock;` 应为 `= now;`。

## 4. `find_active_scheduler_for()` 无出口自旋

没有任何调度器 active 时永远转。

## 5. `Thread::Current::reschedule()` 的门与 `reschedule_mask()` 的 pending 之间没有兑现点

changes 第 16 条把"不可抢占就 `add_pending()`"放进了 `reschedule()`；pending 的兑现点只有
`enable_preemption()` 和空闲循环。等第 1 条落地后要一并核对。

## Fair 自身的问题（作者自留，不由助手改）

- `RunQueue::avg_vruntime()` 对空队列除零；Linux `avg_vruntime()` 有 `if (load)`。
- `RunQueue::current_` 全代码没有写入点。
- `calc_weighted_time()` 的 `weight / MID_WEIGHT` 是整数除法，低于中权重的线程结果恒为 0。
- `Fair::on_thread_init()` / `on_thread_dead()` 丢弃 `Runtime &`，取的是运行 CPU 的调度器。
