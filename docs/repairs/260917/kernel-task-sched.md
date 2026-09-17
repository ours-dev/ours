# kernel/task/sched 待修项（2026-09-17）

## 1. 调度周期是 64ms，因为粒度常量取错了值

`kernel/include/ours/task/sched/common.hpp`

```cpp
constexpr auto MIN_SCHED_GRANULARITY = Milliseconds(1);   // 定义了，没人用
constexpr auto TARGETED_LATENCY = Milliseconds(8);
constexpr SchedDuration SCHED_GRANULARITY = to_sched_duration(TARGETED_LATENCY);  // ← 取了 8ms
constexpr usize SCHEDULING_PERIOD_GRANS = TARGETED_LATENCY / MIN_SCHED_GRANULARITY;  // 8
```

于是 `scheduling_period()` = 8ms × 8 = **64ms**，而它本该是 1ms × 8 = 8ms。
这个值是 `RoundRobin::on_thread_init()` 给每个线程的时间片基线
（`timeslice.m_base`），所以**每个线程的时间片都大了 8 倍**。

原来的字段叫 `minimal_granularity`，注释写着「a thread 在单轮里拿到的最小时间片」，
却用 `DEFAULT_MIN_TARGETED_LATENCY` 初始化，而没人用的 1ms 常量就在它上面两行。
常量上移那一轮刻意原样保留了这个值，因为改它是行为变更。

**为什么要单独一个提交**：修掉它会把上下文切换频率提高 8 倍。那是可观测的性能与
时延变化，需要自己的改动记录和自己的实测数字（切换计数、tick 间隔），不能混在
重构里，否则以后没人说得清是哪一下改变了调度行为。

**建议顺序**：排在 Runtime 收敛的注解工作之后。切换频率提高 8 倍会放大任何已存在的
上下文切换竞态，先把那一块钉稳再动它。

## 2. `Fair::unpark_preempt()` 的两个 Thread* 参数写反了

`kernel/kernel/task/sched/fair.cpp:439`

接口是 `unpark_preempt(Runtime &, Thread *curr, Thread *thread, UnparkControl)`，
Fair 的实现把 `curr` 和 `thread` 当成了对方。两个形参同类型，所以这是合法的 override，
`-Werror` 全程沉默。

`fair.cpp` 目前不在构建里，所以现在触发不到；Fair 一旦放回构建就是活的缺陷。

**建议修法**：改参数类型让编译器能分辨，而不是只改实现——
`struct UnparkPair { Thread *curr; Thread *waking; };` 具名字段，零运行时成本，
且对所有纪律一次性生效。单独提交。
