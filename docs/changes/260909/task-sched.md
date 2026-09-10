# task-sched 改动记录

## 调度器真正变成 per-CPU：身份、负载、跨 CPU 唤醒、运行队列归属（2026-09-09）

**文件**
- `kernel/include/ours/task/sched/scheduler.hpp` — `Scheduler::init()` 收 `CpuNum`
- `kernel/kernel/task/sched/scheduler.cpp` — 上述；负载指标；`do_unpark_thread`／`unpark_preempt` 允许远程
- `kernel/kernel/task/init.cpp` — 把 CPU 号传下去
- `kernel/kernel/task/sched/rr.cpp` — 运行队列从 `Runtime` 解析，不再从"当前 CPU"

**症状**
四颗 CPU 全部 online 之后，**所有线程仍然只在 CPU 0 上跑**，三颗 AP 空转。
一路往下修，症状依次变成：

1. AP 一进 `become_idle()` 就死在
   `Assertion failed on m_runtime.which_cpu == Cpu::id() with message: Invoke reschedule_common() on mismatched CPU[1]`；
2. 断言消失后线程仍然全在 CPU 0；
3. 线程终于被派到 CPU 1/2/3，BSP 却死在 `do_unpark_thread` 的
   `m_runtime.which_cpu == Cpu::id()`；
4. 断言放开后，账面上线程在 AP 的队列里（`nr: 1 1 1 0`），实际执行它们的还是 CPU 0。

四条是同一个病根的四种表现：**这套调度器在形状上是 per-CPU 的，在事实上处处假设
"当前 CPU 就是那个 CPU"**。

**原因**

**1. 每个 Scheduler 都以为自己是 CPU 0。**

```cpp
INIT_CODE auto Scheduler::init() -> void {
    ...
    m_runtime.which_cpu = Cpu::id();
}
```

而 `task::init()` 是**在 BSP 上**遍历 `cpu_possible_mask()` 给每颗 CPU 建调度器的，
`Cpu::id()` 一律答 0。旁边的 `IdleThread::init(CpuNum)` 早就为同一个理由收了参数，
注释也写着"这个 idler 属于哪颗 CPU，不一定是正在跑这段代码的那颗"——`Scheduler::init()`
漏了。

`which_cpu` 不是记账用的装饰。它是 `set_state()` 去翻的那一位：

```cpp
auto &word = STATES[usize(state)];
...
desired.set(m_runtime.which_cpu, on);
```

所以每颗 AP 的 `Scheduler::Current::set_active(true)` 其实都在给 **CPU 0** 置位，
自己那一位始终是 0。而 `select_target_cpu()` 第一顺位就查 Active 掩码——AP 永远不在
候选集里。它同时又是 `reschedule_common()` 拿来自检的东西，于是引导最终停在第 1 条症状上。

**2. 负载指标是一个没有人写的字段。**

```cpp
if (best_cpu == INVALID_CPU_NUM ||
    Scheduler::Current::get(best_cpu).m_runtime.total_expected_runtime >
        Scheduler::Current::get(cpunum).m_runtime.total_expected_runtime) {
```

全树 grep `total_expected_runtime`：**只有这两处读，没有一处写**。于是每次比较都是
`0 > 0`，`pick_least_loaded()` 退化成"永远返回候选集里编号最小的那颗"，也就是 CPU 0。

单核下这个退化答案恰好也是正确答案，所以它藏了很久。四核下它意味着内核创建的每一个线程
都去 CPU 0，三颗 CPU 空转。

**3. 跨 CPU 唤醒被断言挡住。** `unpark_thread()` 是这么写的：

```cpp
target_cpu = select_target_cpu(thread);
auto &self = Scheduler::Current::get(target_cpu);   // 远程调度器
self.do_unpark_thread(thread, control);
reschedule_mask(CpuMask::of(target_cpu));
```

——这是对的，也正是 Zircon `Scheduler::Unblock()` → `target->Insert()` 的形状：插入
发生在**唤醒方**的 CPU 上，目标 CPU 事后靠 IPI 得知。可 `do_unpark_thread()` 开头写着
`DEBUG_ASSERT(m_runtime.which_cpu == Cpu::id())`，`unpark_preempt()` 开头也有一条。
这两条断言之所以一直成立，只是因为第 2 条让 `select_target_cpu()` 除了 0 什么都答不出来。

`unpark_preempt()` 里还有一处更隐蔽的：

```cpp
preemption_state.add_pending();     // 默认参数 = CpuMask::of(Cpu::id())
```

要求抢占的那个线程跑在**目标** CPU 上，而默认掩码是**执行这行代码的** CPU。
跨 CPU 唤醒于是把"该抢占了"记在唤醒方头上，目标什么也没被告知。

**4. 运行队列从"当前 CPU"取，而不是从 Runtime 取。** 这是最后一条，也是最要命的一条：

```cpp
// rr.cpp
auto rq() -> RrRunQueue & {
    return Scheduler::Current::get().rr_run_queue();
}
```

注释写着"每个 ISchedule 入口都跑在它被交付的那个 Runtime 所属的 CPU 上"——这个前提
不成立，也不可能成立，因为第 3 条说的跨 CPU 插入正是设计要的。后果是：
`Scheduler::enqueue_thread()` 把**目标** CPU 的 `num_queued_threads` 加了一，
`RoundRobin::enqueue_thread()` 却把线程挂到了**唤醒方**的 FIFO 上。目标 CPU 看见
自己有活但队列是空的，唤醒方跑着一个自己刚送走的线程。日志里就是
`nr: 1 1 1 0` 配上 `userboot: hello from ring 3` 打在 cpu0。

**改法**
```cpp
auto Scheduler::init(CpuNum cpunum) -> void {   // 不再问 Cpu::id()
    ...
    m_runtime.which_cpu = cpunum;
}
```
```cpp
auto const pick_least_loaded = [](CpuMask const &candidates) -> CpuNum {
    ...
    Scheduler::Current::get(best_cpu).num_runnable() >
        Scheduler::Current::get(cpunum).num_runnable()
    ...
};
```
```cpp
// do_unpark_thread / unpark_preempt：去掉 "which_cpu == Cpu::id()" 断言
preemption_state.add_pending(CpuMask::of(target_cpu));   // 目标，不是本地
```
```cpp
// rr.cpp
auto rq(Runtime &runtime) -> RrRunQueue & {
    return Scheduler::Current::get(runtime.which_cpu).rr_run_queue();
}
```

`num_runnable()`（即 `m_runtime.num_queued_threads`）是 enqueue／dequeue 一直在维护的，
读远程那份不加锁——这是有意的，和 Linux `find_idlest_cpu()` 读
`cpu_rq(cpu)->nr_running` 一样：这个数字本来就是一个下一刻就可能失效的提示，为了放一个
线程去抢三把远程运行队列锁，代价比偶尔放错一次大得多。

`total_expected_runtime` 字段留着没删——按本仓库的惯例，没有调用者的东西可能是还没长出来的
脚手架，不是死代码。它现在没有读者了，记在 `docs/repairs/260909/task-sched.md`。

`RoundRobin::num_queued() const` 仍然取当前 CPU 的队列：`ISchedule::num_queued()` 的
签名里没有 `Runtime`，而它唯一的调用者 `Scheduler::pick_next_thread()` 问的就是自己这颗，
两边一致。哪天有人要问远程的深度，得先让接口把 `Runtime` 带上——这一点写进注释了。

**参照**
- Zircon `Scheduler::Unblock()` → `target->Insert()` / `RescheduleMask()`：跨 CPU 插入
  在唤醒方完成，目标靠 IPI 得知。ours 没有 Zircon 的全局 `thread_lock`，靠的是目标运行
  队列自己的锁（`do_unpark_thread()` 里的 `Guard{get_lock()}`）。
- Linux `find_idlest_cpu()`：用 `cpu_rq(cpu)->nr_running` 比负载，不加锁。
- Linux `select_task_rq()` / `ttwu_queue()`：目标 CPU 由唤醒方选定并入队。

**验证**
`SMP=4` 实测（`kernel.dev.dbg_1.obi`），30 秒窗口内的日志行数：

| CPU | 修改前 | 修改后 |
|-----|-------|-------|
| 0   | 8600  | 8589  |
| 1   | 1     | 648（userboot） |
| 2   | 1     | 98552（Sample1） |
| 3   | 1     | 98500（Sample2） |

`select_target_cpu()` 的分派也能在日志里直接看到：`userboot -> CPU[1]`、
`Sample1 -> CPU[2]`、`Sample2 -> CPU[3]`。`SMP=1` 行为不变，
`kernel.dev.obi`（断言编译掉）同样起到 ring 3。

**故意没做**
- 只改了 `rr.cpp`。`fair.cpp` 有 20 多处同样的 `Scheduler::Current::get().fair_run_queue()`，
  病一模一样，但它不在构建里（见 `kernel/kernel/task/sched/BUILD.gn`），而且 Fair 是
  owner 自己的设计，不由这一改动去动。记在 `docs/repairs/260909/task-sched.md`。
- 没有做负载均衡：线程在 unpark 那一刻被放到某颗 CPU 上之后，不会再迁移。
  Linux 的 `load_balance()`／`nohz idle balance` 这一层还不存在。
- `Scheduler::reschedule(CpuNum)` 仍然是个空函数体（`if (cpu == Cpu::id()) {}`），
  没有调用者，没动。

## 纪律从 `Runtime` 反查 `Scheduler`，不再绕一次 per-CPU 表（2026-09-10）

**文件**
- `kernel/include/ours/task/sched/scheduler.hpp` — 新增 `Scheduler::of(Runtime &)`
- `kernel/kernel/task/sched/rr.cpp` — `rq()` 改用它

**症状**
不是 bug。`rq()` 昨天刚从"当前 CPU"改成"Runtime 所属的 CPU"，但实现方式是绕回去：

```cpp
auto rq(Runtime &runtime) -> RrRunQueue & {
    return Scheduler::Current::get(runtime.which_cpu).rr_run_queue();
}
```

**原因**
`Runtime` 是 `Scheduler` 的**成员**（`scheduler.hpp` 的 `Runtime m_runtime;`）。
从成员回到宿主是一次编译期的常量减法，而上面这行做的是：读 `runtime.which_cpu`，
拿它去索引全局的 `CPU_OFFSETS`，再加上 CpuLocal 的 handle。

代价有两层：

1. **性能**：`RoundRobin` 的八个入口全走这一步，其中 `put_prev_thread` /
   `set_next_thread` 每次上下文切换都跑，`update_thread` 每个 tick 都跑。
   每次都要碰一条和马上要访问的运行队列**毫不相干**的 cacheline（`CPU_OFFSETS`），
   外加一条 `DEBUG_ASSERT(cpunum < MAX_CPU)`。
2. **正确性的依赖面**：运行队列取哪一份，被绑在了 `which_cpu` 这个字段对不对上——
   而 `which_cpu` 恰恰是昨天刚修好的那个字段（`Scheduler::init()` 以前给每颗 CPU
   都写 0）。指针算术本来就知道答案，不需要再问一遍。

而且这个 helper 是 rr.cpp 文件内的 `static`，`fair.cpp` 里同样的 20 处一点好处也拿不到。
放到 `Scheduler` 上，将来 Fair 回到构建里时是一次机械替换。

**改法**
```cpp
FORCE_INLINE
static auto of(Runtime &runtime) -> Self & {
    return *ustl::mem::container_of(&runtime, &Self::m_runtime);
}
```
```cpp
auto rq(Runtime &runtime) -> RrRunQueue & {
    return Scheduler::of(runtime).rr_run_queue();
}
```

`ustl::mem::container_of` 是本仓库既有的 idiom，`Thread::of(sched::Context *)`
（`kernel/include/ours/task/thread.hpp:378`）就是这么写的；`runtime.hpp` 甚至早就
`#include <ustl/mem/container_of.hpp>` 了，只是一直没人用。

顺带把 `scheduler.hpp` 上 `init(CpuNum)` 的注释收短了——它和定义处那段一字不差，
而且自己结尾还写着"see the note on the definition"。

**参照**
Linux 的 `struct rq` 和 `cfs_rq` 之间用的是 `container_of`（`task_of()`、`rq_of()`），
不是每次去查 `cpu_rq(cpu)`。同一个理由。

**验证**
四核引导：cpu0 8589 行 / cpu1 599 / cpu2 89435 / cpu3 89399，无断言失败。

**故意没做**
`fair.cpp` 那 20 处没动——它不在构建里，而且 Fair 是 owner 自己的设计。
`Scheduler::of()` 就位之后那是一次机械替换，仍记在 `docs/repairs/260909/task-sched.md`。

## `ISchedule::num_queued()` 补上 `Runtime` 参数（2026-09-10）

**文件**
- `kernel/include/ours/task/sched/schedule.hpp` — 纯虚函数签名
- `kernel/include/ours/task/sched/rr.hpp` / `kernel/kernel/task/sched/rr.cpp` — 实现
- `kernel/kernel/task/sched/fair.cpp` — 同步签名（该文件不在构建里）
- `kernel/kernel/task/sched/scheduler.cpp` — 唯一的调用点

**症状**
不是 bug，是接口上留下的一个洞。`ISchedule` 的每一个虚函数都收 `Runtime &`——
它就是"问的是哪颗 CPU"的凭证——只有 `num_queued()` 不收：

```cpp
[[nodiscard]]
virtual auto num_queued() const -> usize = 0;
```

**原因**
签名里没有 `Runtime`，实现能写出来的就只有一种：**当前 CPU 的**。

```cpp
auto RoundRobin::num_queued() const -> usize {
    return Scheduler::Current::get().rr_run_queue().num_queued();   // 当前 CPU
}
```

而"当前 CPU 就是那个 CPU"正是昨天整个多核修复要拆掉的假设——
`Scheduler::do_unpark_thread()` 明确地跑在唤醒方、操作目标 CPU 的队列。
昨天在这里留了一段五行注释解释"为什么这一处例外"，而不是把洞堵上；
注释本身还写错了调用者的名字（写的是 `Scheduler::pick_next_thread()`，
全树没有这个函数，真正的调用者是 `Scheduler::evaluate_next_thread()`）。

一个只能靠注释维持的例外，比它想避免的改动更贵：读的人得一直记着
"这个文件里所有查询都按 Runtime 走，除了这一个"。

**改法**
```cpp
virtual auto num_queued(Runtime &runtime) const -> usize = 0;
```
```cpp
auto RoundRobin::num_queued(Runtime &runtime) const -> usize {
    return rq(runtime).num_queued();
}
```

`fair.cpp` 的那份也一并跟上（`Scheduler::of(runtime).fair_run_queue()`）——
它不在构建里，但把一个 override 留在旧签名上等于埋一颗编译错误。
这只是签名对齐，没有碰 Fair/EEVDF 的任何策略。

**被否掉的方案**：把 `num_queued()` 从接口里删掉，理由是
`Scheduler::enqueue_thread`/`dequeue_thread` 已经在维护 `num_queued_threads`。
否掉是因为唯一的调用点用的是两者**相等**这个条件：

```cpp
if (ctx.discipline() == Discipline::Normal &&
    ctx.schedule()->num_queued(m_runtime) == m_runtime.num_queued_threads)
```

意思是"队列里所有线程都属于这个纪律，那就直接问它要下一个，不用遍历所有纪律"。
删掉其中一边，这条快路径就没了。

**参照**
无，本项目自有设计。形状上对应 Linux 的 `sched_class` 方法一律以 `struct rq *rq`
开头——同一个理由：方法本身无状态，rq 才是"哪颗 CPU"。

**验证**
四核引导：cpu0 8589 / cpu1 601 / cpu2 90043 / cpu3 90062，无断言失败。
