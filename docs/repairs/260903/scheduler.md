# 调度器 — 待修

按严重程度排。已修的从这里删掉，改动记录在 `docs/changes/260903/scheduler.md`。

---

## 1. `lockdep::Guard` 从不执行 `IrqSave`/`PreemptDisable`（lockdep 的 lane，交叉索引）

`system/lib/lockdep/include/lockdep/guard.hpp:21-72`：`Options...` 只被存进
`std::tuple<bool, Options...> m_states`，`guard()` 只做 `m_lock.acquire()`。
`state_saver.hpp:22` 的 `StateSaver` 别名括号位置写成了逗号表达式，结果类型永远是
`SavedStateContainer<Option>*`，且无人使用。全树没有任何 `Guard` 构造
`AutoInterruptDisabler`/`AutoPreemptDisabler`。

后果：所有 `Guard<SpinLock, IrqSave>` 都是裸自旋锁。dbg 镜像因此停在
`reschedule_common()` 的 `irqs_disabled()` 断言（changes 第 9 条）。已通知 lib/concurrent 会话。

## 2. `IdleThread::run()` 是一个静默死循环

`kernel/kernel/task/idle_thread.cpp`：`m_states` 构造成 `{Booting, Running}` 后没有任何写入，
`current != target` 永远为真；每圈在 `AutoInterruptDisabler` + `AutoPreemptDisabler` 下调
`Thread::Current::reschedule()`，而 `Scheduler::Current::reschedule()` 在 `!is_preemptible`
（抢占关或中断关）时 `add_pending(); return;`。结果：idler 一旦运行就永远转圈、中断关闭、无输出。
`m_arch.idle()` 也是空函数（`kernel/arch/x86/kernel/task/idle_thread.cpp`），没有 `hlt`。
Zircon `IdlePowerThread::Run()`：状态机初始就是 `kActive`，只在 target 变为 Active/Wakeup 时
`Reschedule()`，否则 `arch_idle()`。

这是 dev 镜像当前的停点（changes 第 9 条）。

## 3. `set_next_thread()` 断言 `curr_thread == 0`，但没人清它

`kernel/include/ours/task/sched/scheduler_inline.hpp`：`set_next_thread` 断言
`0 == m_runtime.curr_thread`，而 `put_prev_thread` 不清、`switch_context` 在其后才写。
第 8 条播种之后，dbg 镜像的第一次真实重调度会撞它。

## 4. `IpiService::send_resched(CpuMask)` 发的是原始 mask

`kernel/kernel/task/ipi_service.cpp`：算好了 `targets`（去掉本 CPU、与 active 求交），
`if (!mask.count())` 和 `arch_mp_reschedule_mask(mask)` 用的却是 `mask`。启动日志里
`IPI-Resched on CPU[0]` 就是它给自己发的。

## 5. `Scheduler::init()` 把每个调度器都记成 BSP

`m_runtime.which_cpu = Cpu::id()`，而 `task::init()` 在 BSP 上替每颗 CPU 调它。
应接收 cpunum，如 `TimerQueue::init(cpunum)`。单核下恒等。

## 6. `Driver::update_clock()` 的基准写错了

`kernel/kernel/task/sched/driver.cpp`：`m_prev_real_clock = m_sched_clock;` 应为 `= now;`。
现在 `delta = now - prev_real` 里 prev 是调度时钟而非真实时钟，两者一旦分叉 delta 就失真。

## 7. `Fair::on_thread_init()` / `on_thread_dead()` 取的是运行 CPU 的调度器

`fair.cpp`：`Runtime &` 形参被丢弃，`Scheduler::Current::get()` 取当前 CPU。Fair 当前不在构建里。

## 8. `find_active_scheduler_for()` 无出口自旋

`scheduler.cpp`：没有任何调度器 active 时永远转。

## Fair 自身的问题（作者自留，不由助手改）

- `RunQueue::avg_vruntime()` 对空队列除零（`weight_sum_ == 0`）；Linux `avg_vruntime()` 有 `if (load)`。
- `RunQueue::current_` 全代码没有写入点。
- `calc_weighted_time()` 的 `weight / MID_WEIGHT` 是整数除法，低于中权重的线程（含 `IDLE_PRIORITY`）
  结果恒为 0。Linux 用 `calc_delta_fair()` 的定点逆权重。
