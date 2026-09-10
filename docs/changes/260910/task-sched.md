# task-sched 改动记录

## 从上下文切换里出来时，问的必须是"我现在这颗 CPU"（2026-09-10）

**文件**
- `kernel/kernel/task/sched/scheduler.cpp` — `switch_context()` 尾部

**症状**
四核下解空指针：

```
[... cpu0 error] Unhandled page fault: vector=14, error_code=0x0
[... cpu0 error]   rip=0xffffff8840020520 (+0x20520)   rdi=0x88
```

`rdi=0x88` 是一个空指针加上 `Thread::m_pi_lock` 的偏移。lldb 的调用栈：

```
frame #5: Scheduler::finish_switch_context() at scheduler.cpp:541
frame #6: Scheduler::switch_context(...)     at scheduler.cpp:580
frame #7: Thread::Current::sleep_etc(...)    at thread.cpp:558
frame #10: kshell::shell_main()              at shell.cpp:77
```

也就是 kshell 线程睡了 10 ms、被唤醒、从切换里回来的那一刻。

**原因**
`switch_context()` 的尾部是这样的：

```cpp
    next->get_pi_lock().acquire();
    get_lock().release();
    Thread::switch_thread(curr, next);
    // ——— 这条线以下，是这个线程**将来某个时刻**被切回来才会执行 ———
    finish_switch_context();          // 隐式的 this->
```

`this` 是**进入切换时**那颗 CPU 的调度器。可是从切换里回来的时候，这个线程未必还在
那颗 CPU 上：它睡下去之后是由 `select_target_cpu()` 重新安置的，而那个函数完全可以
答出另一颗 CPU。于是 `this` 变成了**别人家的调度器**，它的 `prev_thread` 早就被
那颗 CPU 自己消费掉（`std::exchange(m_runtime.prev_thread, nullptr)`）：

```cpp
auto Scheduler::finish_switch_context() -> void {
    auto const prev_thread = std::exchange(m_runtime.prev_thread, nullptr);
    prev_thread->get_pi_lock().release();     // prev_thread == nullptr
    ...
}
```

同一次切换有**两个出口**，而它们的做法早就不一致：

| 出口 | 谁走 | 怎么拿调度器 |
|------|------|-------------|
| `trampoline_handoff()` | 从没跑过的新线程 | `Scheduler::Current::get()` ✅ |
| `switch_context()` 尾部 | 被切回来的线程 | 切换前捕获的 `this` ❌ |

`trampoline_handoff()` 一直是对的，注释里也写明了它在释放"跨切换传递过来的两把锁"。
错的是另一条。

单核下两者等价，所以这个错一直是隐形的。四核下它需要"线程睡在一颗 CPU、醒在另一颗"
才会显形——这在多核上是常态，只是在此之前树里还没有足够多的可运行线程把它逼出来
（kshell 的控制台线程和用户态 shell 进来之后就有了）。

**改法**
```cpp
    //  3. |curr| before context switch was cached in |...prev_thread|.
    //
    // Current::get(), not `this`. ...
    Scheduler::Current::get().finish_switch_context();
```

**参照**
本仓库自己的 `Scheduler::trampoline_handoff()`——同一次切换的另一个出口，
一直就是 `Scheduler::Current::get()`。Zircon 的
`Scheduler::RescheduleCommon()` 在切换之后同样重新取 `Scheduler::Get()`，
不复用切换前的那个指针。

**验证**
`kernel.dev.dbg_1.obi` 四核：页错误消失。
这一条单独提交时，故障集合与纯 HEAD 完全一致（只剩下 `ctx.curr_cpu() == m_runtime.which_cpu`
那条断言，它是另一个缺陷，下一节）——也就是说本改动没有引入也没有掩盖任何东西。
两条一起修完之后，四核引导零断言、零故障，用户态 shell 在 ring 3 打印。

**故意没做**
`switch_context()` 里切换**之前**用的 `this`（`m_runtime.prev_thread = curr`、
`m_runtime.curr_thread = next`、`get_lock().release()`）没有动——那些都发生在切换发生
之前，跑在正确的 CPU 上，`this` 是对的。只有那条线以下才是新世界。

## 线程终于会记下自己在哪颗 CPU 上跑（2026-09-10）

**文件**
- `kernel/include/ours/task/sched/scheduler_inline.hpp` — `set_next_thread()` / `put_prev_thread()`
- `kernel/kernel/task/sched/scheduler.cpp` — `enqueue_thread()`
- `kernel/include/ours/task/sched/context.hpp` — 只加注释

**症状**
四核下 cpu1 必死：

```
[3890776000 cpu1 error] Assertion failed on ctx.curr_cpu() == m_runtime.which_cpu
```

出自 `Scheduler::reschedule_common()` 里的 `assert_thread_in_scheduler(curr)`：
正在这颗 CPU 上跑的线程，自己记的 CPU 号却不是这颗。

**原因**
全树 grep `m_curr_cpu`，**只有一处写入**：

```cpp
// scheduler.cpp:191，Scheduler::init_first_thread()
context.m_curr_cpu = cpunum;
```

而 `init_first_thread()` 只对两种线程调用——boot 线程，和每颗 CPU 的 idler。
**其余每一个线程，`m_curr_cpu` 从构造之后就再没被写过**：

```cpp
Context(BaseProfile profile)
    : ...
    , m_last_cpu()
    , m_curr_cpu()      // = 0
```

值初始化成 0。单核下 0 永远是对的，所以断言永远成立；四核下，第一个在非 0 号 CPU 上
真正走一次 `reschedule_common()` 的普通线程就把它打爆。

为什么以前没爆？因为在 kshell 的控制台线程和用户态 shell 进来之前，每颗 AP 上通常
只有一个可运行线程，RR 的 `update_thread()` 里那句 `if (queue.num_queued() && ...)`
就不会请求抢占，`reschedule_common()` 根本不会被调到。可运行线程一多，它立刻显形。

`m_last_cpu` 是同一个病：`select_target_cpu()` 的兜底写着

```cpp
auto const last_cpu = se.last_cpu();
auto const starting_cpu = last_cpu == INVALID_CPU_NUM ? Cpu::id() : last_cpu;
```

——"倾向于放回它上次跑过的 CPU"。而 `m_last_cpu` 同样只有 `init_first_thread()` 写，
所以对每个普通线程它恒等于 0，这条启发式一直在说"它上次在 CPU 0"。

**改法**
在"线程与运行队列发生关系"的两个咽喉处写：

```cpp
// scheduler_inline.hpp，set_next_thread()：即将在这颗 CPU 上运行
ctx.m_curr_cpu = m_runtime.which_cpu;

// scheduler_inline.hpp，put_prev_thread()：刚在这颗 CPU 上跑过
ctx.m_last_cpu = m_runtime.which_cpu;

// scheduler.cpp，enqueue_thread()：进了这颗 CPU 的队列
so.m_curr_cpu = m_runtime.which_cpu;
```

`enqueue_thread()` 和 `set_next_thread()` **两处都要**：纪律会把即将运行的线程从队列里
摘掉（`RoundRobin::set_next_thread()` 就是），所以"正在这颗 CPU 上跑"是一个
`enqueue_thread()` 永远看不到的状态。

三处都取 `m_runtime.which_cpu` 而不是 `Cpu::id()`——`enqueue_thread()` 的 `this`
未必是正在执行的那颗 CPU，`do_unpark_thread()` 就是从唤醒方伸进目标队列的。

**参照**
Linux 的 `task_struct::on_cpu` / `wake_cpu` 由 `__schedule()` 和 `ttwu` 路径维护；
Zircon 的 `Thread::scheduler_state().curr_cpu_` 在 `Scheduler::Insert()` 和
`RescheduleCommon()` 里写，同样是"入队"和"选中运行"两处。

**验证**
四核 `kernel.dev.dbg_1.obi`：**零断言、零故障**，
cpu0 8591 / cpu1 603 / cpu2 86845 / cpu3 86851，用户态 shell 在 ring 3 打印。
`kernel.dev.obi` 四核同样起到 ring 3；单核零故障、329510 行。
（对比：改之前 cpu1 只有 5 行就死了。）

**故意没做**
- **没有把 `m_curr_cpu` / `m_last_cpu` / `m_wake_cpu` 的初值改成 `INVALID_CPU_NUM`**，
  尽管每一个读者都拿它和 `INVALID_CPU_NUM` 比、显然那才是"还没分配"的本意。
  试过：它会**打开三条从来没有执行过的分支**（`Scheduler::Pi::common()` 的 else 臂、
  `set_cpu_affinity()` 的迁移检查、`select_target_cpu()` 的"从没跑过"臂），
  其中第一条当场解空指针。那是一次独立的、需要连着那三条分支一起做的改动，
  单独记在 `docs/repairs/260910/task-sched.md`。
- **没有在出队时清掉 `m_curr_cpu`**。按现在的语义它是"哪颗 CPU 的调度器拥有我"，
  一个阻塞在等待队列上的线程其实不属于任何队列，但清掉它同样会打开上面那些分支。
  同一条待修项。
