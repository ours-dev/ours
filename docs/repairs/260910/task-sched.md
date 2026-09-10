# task-sched —— 待修项（2026-09-10）

## 1. `Context` 的三个 CPU 字段初值是 0，而所有读者都拿它和 `INVALID_CPU_NUM` 比

**严重程度**：中。它让"还没分配 CPU"和"在 CPU 0 上"变成同一个答案。

`kernel/include/ours/task/sched/context.hpp` 的构造函数：

```cpp
, m_last_cpu()      // = 0
, m_curr_cpu()      // = 0
, m_wake_cpu()      // = 0
```

而三个读者都是这么写的：

- `Scheduler::set_cpu_affinity()`：`if (auto const cpu_thread_on = so.curr_cpu(); cpu_thread_on != INVALID_CPU_NUM)`
- `Scheduler::Pi::common()`：`if (auto const which_cpu = context.curr_cpu(); which_cpu != INVALID_CPU_NUM)`
- `Scheduler::select_target_cpu()`：`last_cpu == INVALID_CPU_NUM ? Cpu::id() : last_cpu`

意图很清楚，初值写错了。

**为什么没有顺手改**：实测过，改成 `INVALID_CPU_NUM` 会**同时打开三条在这棵树里
从未执行过的分支**，而其中 `Pi::common()` 的 else 臂当场解空指针
（`wq->get_lock().assert_held()` 一线，四核引导上 `rdi=0x88`）。也就是说这三条分支
从来没有被验证过，改初值等于一次性把它们全部启用。这是一次独立的改动，
要连着那三条分支一起做、一起验证。

同一条里还有：`m_curr_cpu` 在线程**出队**（park/sleep/dead）时没有被清掉，
所以一个阻塞在等待队列上的线程仍然记着最后拥有它的那颗 CPU。按
"哪颗 CPU 的调度器拥有我"的语义那是错的，但清掉它会走进同样那几条分支。

## 2. `Scheduler::migrate_thread()` 是个空函数，但现在真的会被调到

`kernel/kernel/task/sched/scheduler.cpp`：

```cpp
auto Scheduler::migrate_thread(Thread *thread) -> void {
    if (thread->is_state_ready()) {
        // Dequeue from the scheduler which it is on and Enqueue
        // onto the selected scheudler's runqueue.
    } else if (thread->is_state_running()) {
        // Trigger the IPI for rescheduling
    } else {
    }
}
```

三个分支全空。以前 `curr_cpu` 恒为 0，`set_cpu_affinity()` 里那句
"线程可能正待在新亲和性不允许的 CPU 上"只在亲和性排除 CPU 0 时才成立；
`m_curr_cpu` 开始被维护之后（见 `docs/changes/260910/task-sched.md`），
任何把线程钉到它当前 CPU 之外的 `set_cpu_affinity_hard()` 都会走到这里，
然后**什么也不发生**——线程留在一个它的硬亲和性已经不允许的 CPU 上继续跑。

`Thread::Current::migrate_to()` 是现成的入口。今天树里没有调用者，所以还没咬人。
