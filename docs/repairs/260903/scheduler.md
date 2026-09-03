# 调度器 — 待修

按严重程度排。已修的从这里删掉，改动记录在 `docs/changes/260903/scheduler.md`（第 1–18 条）。

---

## 1. 启动的下一个停点：`reclaim_init_area()` 在 init 代码里释放 init 段（mm / top 的 lane，交叉索引）

`kernel/top/main.cpp` 的 `labour_routine()` 是 `INIT_CODE`，它调用 `mm::EarlyMem::reclaim_init_area()`
（也是 `INIT_CODE`）把 `.init*` 段的物理页交还 buddy 分配器——而调用者本身就在 `.init*` 里执行。
页表映射还在，所以不会当场缺页；紧接着的两次 `Thread::spawn()` 一旦从这些页分配到内核栈或对象，
正在执行的代码就被覆盖，CPU 静默死亡。dbg 镜像现在正好停在这里：`sleep_etc` 返回后进入
`reclaim_init_area()`，打印一串 `Free frame(...)` 之后再无输出，也没有 "I'm Thread 1/2"。

Linux 的 `free_initmem()` 由 `kernel_init()` 在**所有** `__init` 代码返回之后调用，且它自身不是
`__init`。改法在 `main.cpp`（调用顺序）或 mm，不在 task。

## 2. 没有中断出口的抢占兑现，也没有中断嵌套计数（arch/x86 的 lane，交叉索引）

`PreemptionState::IrqNestedCounter` 全树无人增减；`x86_dispatch_exception()` 出口不检查
pending 抢占。后果：changes 第 15 条把 `reschedule_mask()` 的本地抢占改成只记 pending 之后，
一个正在运行的线程只会在自己 `enable_preemption()` 或空闲循环时被换下——tick 里
`RoundRobin::update_thread()` 标的 pending 不会在中断返回时兑现，**时间片轮转事实上不生效**，
一个忙循环线程会一直占着 CPU。Zircon：`x86_exception_handler` 出口对 `preempts_pending` 调
`Thread::Current::Preempt()`，进入中断时 `arch_set_blocking_disallowed(true)`。

## 3. `IpiService::send_resched(CpuMask)` 发的是原始 mask

`kernel/kernel/task/ipi_service.cpp`：算好了 `targets`（去掉本 CPU、与 active 求交），
`if (!mask.count())` 和 `arch_mp_reschedule_mask(mask)` 用的却是 `mask`。启动日志里的
`IPI-Resched on CPU[0]` 就是它给自己发的。

## 4. `Scheduler::init()` 把每个调度器都记成 BSP

`m_runtime.which_cpu = Cpu::id()`，而 `task::init()` 在 BSP 上替每颗 CPU 调它。应接收 cpunum。

## 5. `Driver::update_clock()` 的基准写错了

`m_prev_real_clock = m_sched_clock;` 应为 `= now;`。

## 6. `find_active_scheduler_for()` 无出口自旋

没有任何调度器 active 时永远转。

## 7. `Thread::Current::reschedule()` 的门与 `reschedule_mask()` 的 pending 之间没有兑现点

changes 第 16 条把"不可抢占就 `add_pending()`"放进了 `reschedule()`；pending 的兑现点只有
`enable_preemption()` 和空闲循环。等第 2 条落地后要一并核对。

## Fair 自身的问题（作者自留，不由助手改）

- `RunQueue::avg_vruntime()` 对空队列除零；Linux `avg_vruntime()` 有 `if (load)`。
- `RunQueue::current_` 全代码没有写入点。
- `calc_weighted_time()` 的 `weight / MID_WEIGHT` 是整数除法，低于中权重的线程结果恒为 0。
- `Fair::on_thread_init()` / `on_thread_dead()` 丢弃 `Runtime &`，取的是运行 CPU 的调度器。
