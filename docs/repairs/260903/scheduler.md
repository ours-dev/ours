# 调度器 — 待修

按严重程度排。已修的从这里删掉，改动记录在 `docs/changes/260903/scheduler.md`（第 1–27 条）。

---

## 1. 次级 CPU 没有自己的 `DpcRunner::init()`

`DpcRunner::init()` 按设计是「对当前 CPU」的（`Current::get()` / `mm::CpuLocal::current_cpu()`），
而 changes 第 27 条加的 `GKTL_INIT_HOOK` 只在发布该层的那颗 CPU（BSP）上跑。AP 起来之后
欠自己一次 `DpcRunner::init()`；不补的话它的 DPC 队列仍是 CpuLocal 的零填充存储，
第一次 enqueue 就是 changes 第 24 条那个空指针页故障。等 SMP 真正起来时必须处理。

同一形状的还有 `Scheduler::init()`（第 2 条）和 `TickSched::emulate()`——凡是「每 CPU 一次」
的初始化，目前都只覆盖了 BSP。

## 2. `Scheduler::init()` 把每个调度器都记成 BSP

`m_runtime.which_cpu = Cpu::id()`，而 `task::init()` 是在 BSP 上替每颗 possible CPU 调它的。
应当接收 cpunum，如 `TimerQueue::init(cpunum)`。单核下恒等，多核下
`unpark_preempt()` 的 `DEBUG_ASSERT(Cpu::id() == m_runtime.which_cpu)` 和
`Fair::on_thread_unpark()` 记的 `m_wake_cpu` 都会错。

## 3. 抢占延后的一般规则没有接上（现在可以接了）

`Zircon Scheduler::PreemptLocked()` 在 `arch_num_spinlocks_held() > 0` 时只记 pending 不切换。
ours 有这个计数，但我试的时候 `SpinLock::num_held_cpu()` 读出来是垃圾
（`288423890198200320/1/2`，只有低位是计数），只好退而求其次，把 DPC 队列锁单独改成
`IrqSave`（changes 第 25 条）。根因后来由 lib/concurrent 在 `06176e9` 修掉
（`kernel.ld` 缺 `KEEP`，所有 counter 的存储段被 `--gc-sections` 回收，counter 一直在改写
别人的 per-CPU 内存），我已验证（`COUNTER_STORAGE_` 符号 0 → 27）。

所以这条现在走得通了，而且比逐个给队列锁打 `IrqSave` 更普适：任何用普通 `Guard` 持自旋锁
的地方仍然会被抢占，并留下一把没人能释放的锁。

## 4. 中断里睡眠没有断言；`NmiNestedCounter` 同样无人增减

`IrqNestedCounter` / `in_interrupt()` 已删（`docs/changes/260924/kernel-task.md` 末条），
进中断的禁用计数由 `handle_irq_preamble()` 抬起。剩下两件：

- Zircon `int_handler_start()` 还会 `arch_set_blocking_disallowed(true)`，可能睡眠的锁
  （`Mutex::Acquire()`、`BrwLock`、user-copy）断言它为假。ours 没有等价断言；可以用
  `irq::current_frame() != nullptr` 当信号，但 ours 的 #PF 在前导/尾声之间、关中断关抢占地处理，
  得先像 `Zircon x86_pfe_handler()` 那样在缺页路径里放开，否则断言会打在缺页上。
- `PreemptionState::NmiNestedCounter` 只被 `is_preemption_enabled()` 读，从不写；NMI 路径按设计
  不做任何簿记（`arch_handle_exception()` 的注释）。删还是维护，待定。

## 5. `Driver::update_clock()` 的基准写错了

`kernel/kernel/task/sched/driver.cpp`：`m_prev_real_clock = m_sched_clock;` 应为 `= now;`。
现在 `delta = now - m_prev_real_clock` 里 prev 是调度时钟而不是真实时钟，两者一旦分叉
delta 就失真。

## 6. `find_active_scheduler_for()` 无出口自旋

`kernel/kernel/task/sched/scheduler.cpp`：没有任何调度器 active 时永远转，也不 relax。

## Fair 自身的问题（作者自留，不由助手改）

- `RunQueue::avg_vruntime()` 对空队列除零（`weight_sum_ == 0`）；Linux `avg_vruntime()`
  有 `if (load)` 守卫。
- `RunQueue::current_` 全代码没有写入点。
- `calc_weighted_time()` 的 `weight / MID_WEIGHT` 是整数除法，低于中权重的线程（含
  `IDLE_PRIORITY`）结果恒为 0。Linux 用 `calc_delta_fair()` 的定点逆权重。
- `Fair::on_thread_init()` / `on_thread_dead()` 丢弃 `Runtime &`，取的是**运行** CPU 的调度器。
