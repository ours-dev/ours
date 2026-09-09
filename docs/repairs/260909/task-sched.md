# task-sched —— 待修项（2026-09-09）

四核引导已经通了（见 `docs/changes/260909/task-sched.md`），下面是这一轮里看见但没有修的。

## 1. `fair.cpp` 里有二十多处和 rr 一样的"运行队列取当前 CPU"

**严重程度**：高——但**当前不触发**，因为 `fair.cpp` 不在构建里
（`kernel/kernel/task/sched/BUILD.gn`，注释写着"走 round-robin 期间先移出去"）。
一旦换回 Fair，四核会立刻退回到今天修之前的样子：线程记在目标 CPU 的账上、
挂在唤醒方的队列里。

**位置**

```
kernel/kernel/task/sched/fair.cpp:134,166,174,194,198,237,262,293,302,308,318,333,350,364,379,406,426,463
```

全是 `Scheduler::Current::get().fair_run_queue()`，而 fair.cpp:95 的注释还写着
"通过 `Scheduler::Current::get().fair_run_queue()` 拿调用 CPU 的 RunQueue"——
这个前提在 `Scheduler::do_unpark_thread()` 面前不成立。

**怎么修**：和 rr.cpp 一样，把查询改成从入参的 `Runtime &` 出发：

```cpp
auto rq(Runtime &runtime) -> FairRunQueue & {
    return Scheduler::Current::get(runtime.which_cpu).fair_run_queue();
}
```

**注意**：Fair/EEVDF 这套纪律是 owner 自己的设计，动之前先问。这一条改的是"从哪儿
取队列"，不是纪律本身，但它会碰到十八个函数。

## 2. `Runtime::total_expected_runtime` 现在一个读者都没有了

**严重程度**：低。

`kernel/include/ours/task/sched/runtime.hpp:77`。它本来只有
`Scheduler::select_target_cpu()` 里的两处读、零处写；今天负载指标换成
`num_queued_threads` 之后，它读也没有了。

按本仓库的惯例（没有调用者的东西可能是还没长出来的脚手架），没有删。
要么让某个纪律真的去维护它、然后把负载指标换回来（那比队列深度更准），
要么确认它是废弃的再删。

## 3. 线程一旦落地就不会再迁移

**严重程度**：中。

`Scheduler::select_target_cpu()` 只在 `on_thread_init()` 和 `unpark_thread()`
被调用，也就是"线程被创建"和"线程被唤醒"两个时刻。之后没有任何东西会把一个长期运行的
线程从忙 CPU 挪到闲 CPU：Linux 的 `load_balance()` / `nohz_idle_balance()` 这一层不存在。

今天的样例负载（两个死循环线程）恰好各占一颗 CPU，看不出问题。三个 CPU 绑定线程 + 一个
空闲 CPU 的负载会一直歪着。

`kernel/kernel/task/sched/scheduler_migrator.cpp` 已经在构建里，是这件事的落脚点。

## 4. `Scheduler::reschedule(CpuNum)` 是个空壳

**严重程度**：低（无调用者）。

```cpp
auto Scheduler::reschedule(CpuNum cpu) -> void {
    if (cpu == Cpu::id()) {
    }
}
```

`kernel/kernel/task/sched/scheduler.cpp:276`。函数体是一个空的 if。
要么补成真的（本地就置 pending、远程就走 `IpiService::send_resched`，
也就是 `reschedule_mask()` 的单 CPU 版），要么删掉。

## 5. `ISchedule::num_queued()` 拿不到 `Runtime`

**严重程度**：低。

`kernel/include/ours/task/sched/schedule.hpp:127` 是纯虚，签名里没有 `Runtime`，
所以实现只能去问 `Scheduler::Current::get()`——当前 CPU。它唯一的调用者
`Scheduler::pick_next_thread()` 问的确实是自己这颗，所以现在是对的，
但这是这个文件里唯一一处"靠调用者恰好在对的 CPU 上"的地方，和第 1 条是同一个坑。

哪天要问远程 CPU 的队列深度，先把 `Runtime &` 加进签名。

## 6. `Cpu::is_online()` 读的是普通变量，写在另一颗 CPU 上

**严重程度**：中，竞态。

`Cpu::STATES` 是 `std::array<CpuMask, N>`，`CpuMask` 是普通 `BitSet`，不是原子的。
`start_nonboot_cpu()` 在 AP 上 `set()`，`x86_bring_aps()` 在 BSP 上
`while (!Cpu::is_online(cpu));` 里读——没有任何 acquire/release，
编译器完全可以把这个读提到循环外。今天能跑是因为循环体是空的、优化器留下了访存，
不是因为它对。

对照：`Scheduler::STATES` 是 `std::array<std::atomic<CpuMask>, N>`，同一件事那边做对了。
`Cpu::STATES` 该照着改。
