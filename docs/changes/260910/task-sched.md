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
