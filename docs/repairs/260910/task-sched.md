# task-sched —— 待修项（2026-09-10）

## `Thread` 的 `m_curr_cpu` / `m_last_cpu` / `m_wake_cpu` 从来不被维护

**严重程度**：中偏高。这三个字段是"这个线程在哪个 CPU 上"的公开答案
（`Thread::curr_cpu()` / `last_cpu()`，`kernel/include/ours/task/thread.hpp:251-259`），
查亲和性、迁移、跑错核这类问题的人第一个会去看它们，而它们**全是错的**。

**复现**
四核启动，四个线程分别跑在 CPU 0/1/2/3 上，三个字段全是 0：

```
(lldb) kthread 0
'Idle/0' tid=3 @ 0xffffff88000125c0
  cpu      curr=0 last=0 wake=0
(lldb) kthread 1
'userboot' tid=18 @ 0xffffff8000105d48
  cpu      curr=0 last=0 wake=0        ← 实际在 CPU 1 上跑
(lldb) kthread 2      → 'Sample1'  curr=0 last=0 wake=0   ← 实际在 CPU 2
(lldb) kthread 3      → 'Sample2'  curr=0 last=0 wake=0   ← 实际在 CPU 3
```

"实际在哪个 CPU"是通过该 CPU 的 per-CPU `Current<Thread*>` 拿到的，两者本该一致。

**证据**
全仓只有三处赋值，而且都不在上下文切换路径上：

```
$ grep -rn 'm_curr_cpu *=\|m_last_cpu *=\|m_wake_cpu *=' kernel/ --include='*.cpp'
kernel/kernel/task/sched/scheduler.cpp:190:    context.m_curr_cpu = cpunum;
kernel/kernel/task/sched/scheduler.cpp:191:    context.m_last_cpu = cpunum;
kernel/kernel/task/sched/fair.cpp:323:        thread->scheduling_context().m_wake_cpu = runtime.which_cpu;
```

前两处在 `Scheduler::init_first_thread()` 里——那是**一次性的引导路径**，只处理"这个 CPU
启动时就在跑的那个线程"。此后每一次换上/换下线程都没有人再写它们。第三处在 Fair 里，
而 bring-up 期跑的是 RR，所以那一行也到不了。

于是：没经过 `init_first_thread()` 的线程，这三个字段一直是默认构造的 0。

**该怎么修**
在换上线程的地方写 `m_curr_cpu`，换下时写 `m_last_cpu`——也就是 `Scheduler` 里真正
完成切换的那一处，和 `Runtime::curr_thread` 的更新放在一起最不容易再次走散。
`m_wake_cpu` 应当在唤醒选核时写，而不是只在 Fair 里写。

对标 Linux：`task_struct::on_cpu` / `wake_cpu` / `recent_used_cpu`，都在
`__schedule()` / `try_to_wake_up()` 这些主路径上更新。

**影响到的东西**
- `Thread::curr_cpu()` / `last_cpu()` 的每一个调用者。
- lldb 的 `kthread` 命令会打出 `curr/last/wake_cpu all 0 -- nothing maintains them`
  并指回这份文档；修好之后它会自动变成真正的"跑错核"告警
  （`scripts/lldb_ours.py` 里那处判断已经写好了两条分支）。
