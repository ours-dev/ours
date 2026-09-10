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

## 5. 跨 CPU 唤醒总是发 IPI，丢掉了刚算出来的抢占结论

**严重程度**：中，性能。

`Scheduler::do_unpark_thread()` 会调 `unpark_preempt()` 判断被唤醒的线程要不要抢占
目标 CPU 正在跑的那个，然后 `unpark_preempt()` **返回 void**，
`unpark_thread()` 无条件 `to_resched.set(target_cpu)`。

于是每一次"不该抢占"的跨 CPU 唤醒都要付：唤醒方一次 ICR 写加 delivery-status 自旋，
目标一次中断进入和 EOI，而且因为 `x86_handle_ipi_resched()`
（`kernel/arch/x86/kernel/mp.cpp`）无条件调 `add_pending()`，
目标还会跑一整趟 `reschedule_common()`，可能白白切一次上下文。

**怎么修**：把结论从 `unpark_preempt()` / `do_unpark_thread()` 传出来，只有为真时
才 `to_resched.set(target_cpu)`。`unpark_preempt()` 在目标当前线程上置的 pending 位
仍然是对的机制，IPI 只是通知，应该跟同一个条件走。

**参照**：Zircon `Scheduler::Unblock()` —— `if (target->IsPreemptRequired(...)) mp_reschedule(...)`。

**没有顺手改的原因**：这是唤醒协议的行为改动（少发 IPI 意味着"谁负责最终把目标叫醒"
的责任转移），错了会表现成偶发的唤醒丢失，不适合在一次以质量为目的的清理里做。

## 6. `Runtime` 的可变字段没有锁注解

**严重程度**：中。

`kernel/include/ours/task/sched/runtime.hpp` 里的 `curr_thread`、`prev_thread`、
`next_thread`、`num_queued_threads`、`total_weight`、`curr_started_time`、`driver`
都由 `rqlock` 保护，但没有 `TA_GUARDED(rqlock)`；`Scheduler::unpark_preempt()` 也没有
`TA_REQUIRES(get_lock())`——它读的是**远程** CPU 的 `m_runtime.curr_thread`，
安全性完全依赖唯一的调用者恰好持着那把锁。

删掉的那两条 `which_cpu == Cpu::id()` 断言，本来想表达的就是"这里有个不变量"，
只是表达错了对象。正确的不变量是"调用者持有**目标**的运行队列锁"，
而这个不变量是可以被静态检查的。

**怎么修**：给 `Runtime` 的可变字段加 `TA_GUARDED(rqlock)`，给 `unpark_preempt()` 加
`TA_REQUIRES(get_lock())`。规则写在数据上一次，胜过在每个函数上写一遍。
副作用是 `select_target_cpu()` 里那次**故意不加锁**的 `num_runnable()` 读会被标出来，
需要显式写成一次带注解的豁免——那正是它应该有的样子，而不是靠一段注释。

**没有顺手改的原因**：会连锁到 `Scheduler` 的一大片方法和两个纪律，超出这次清理的范围。
