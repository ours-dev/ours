# 调度器 — 待修

按严重程度排。已修的条目从这里删掉，改动记录在 `docs/changes/260903/scheduler.md`。

原先挂在这里的两条交叉索引（`context.schedule()` 为空、新线程在绑定纪律之前被问要哪颗
CPU）已经修完，见 `docs/repairs-20260830.md` 第三十九／四十／四十一节和
`docs/changes/260903/scheduler.md` 第 1、2 条，对应提交 `10cceaf`、`da0bb97`、
`d7d3492` 及其后一条。

---

## 1. `Scheduler::init()` 把每个调度器都记成 BSP

`kernel/kernel/task/sched/scheduler.cpp`，`Scheduler::init()` 末尾：

```cpp
m_runtime.which_cpu = Cpu::id();
```

而 `task::init()` 是**从 BSP 上**替每颗 possible CPU 调它的，所以每个 scheduler 记下的
都是 BSP 的号。`init()` 应当接收 cpunum 参数——`TimerQueue::init(cpunum)` 就是这么做的，
理由见 `docs/changes/260903/kernel-time.md`「绑定 CPU 号」那一段。

严重性：`m_runtime.which_cpu` 被 `unpark_preempt()` 的 `DEBUG_ASSERT(Cpu::id() ==
m_runtime.which_cpu)` 和 `Fair::on_thread_unpark()` 记录 `m_wake_cpu` 用到，多核起来后
两处都会给出错误答案。当前单核下恒等，触发不到。

## 2. `Fair::on_thread_init()` / `on_thread_dead()` 取的是错的调度器

`kernel/kernel/task/sched/fair.cpp:298` 与 `:304`：

```cpp
auto Fair::on_thread_init(Runtime &, Thread *thread) -> void {
    Scheduler::Current::get().fair_run_queue().verify();
```

`Runtime &` 形参就在手边却被丢弃，`Scheduler::Current::get()` 取的是**正在运行的**那颗
CPU 的调度器。在 `on_thread_init` 这条路上，线程被放到 `target_cpu` 上，而调用发生在
创建它的那颗 CPU 上——现在两者可以不同（见 changes 第 2 条，空闲线程钉在自己的 CPU 上）。

严重性：目前后果只是 `verify()` 校验了不相干的那条运行队列，不是崩溃。但同一个文件里
`on_thread_unpark()` 用的是 `runtime.which_cpu`，写法不一致，先修的应该是这个不一致。

## 3. `find_active_scheduler_for()` 在没有调度器 active 时空转

`kernel/kernel/task/sched/scheduler.cpp`：

```cpp
while (true) {
    auto const target_cpu = Self::select_target_cpu(thread);
    ...
    if (self.is_state_active()) { ...; return target_cpu; }
}
```

没有任何退出条件，也不 relax。一颗永远不会 active 的 CPU 会让它锁死在这个循环里。
改动前它同样出不来（`select_target_cpu()` 那时是直接断言），所以不是回归，但这个循环
本身缺一个「等不到就该说等不到」的出口。
