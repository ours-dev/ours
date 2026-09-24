# kernel-task 改动记录（2026-09-24）

## `Thread::Current::kill()` 的断言条件写反了（2026-09-24）

**文件**
- `kernel/kernel/task/thread.cpp` — `Thread::Current::kill()` 的 `DEBUG_ASSERT` 取反

**症状**
潜伏，当前没有调用者。一旦有线程调用它来杀自己（P1-A 的 fault-kill 会走到同一类路径），
dbg_1 镜像会对每一个普通线程断言失败；唯一能通过断言的却是 idle 线程，而它恰恰是
唯一不能被杀的。

**原因**
断言的消息写的是"试图杀 idle 线程"，条件却是 `is_idle_thread()` 为真。`DEBUG_ASSERT`
在条件为假时触发，所以它实际表达的是"当前线程必须是 idle"。消息和条件说的是相反的
两件事，读的人照消息理解，编译器照条件执行。

idle 线程不能死，是因为它是 CPU 在没有别的可跑时运行的东西：调度器在空队列上总能
挑到它，杀掉它之后，这颗 CPU 下一次无事可做时就挑不出任何线程。

**改法**
改前：

```cpp
    DEBUG_ASSERT(current_thread->is_idle_thread(), "Try to kill the idle thread");
```

改后：

```cpp
    DEBUG_ASSERT(!current_thread->is_idle_thread(), "Try to kill the idle thread");
```

**参照**
`Zircon Thread::Current::Kill()`：`DEBUG_ASSERT(!current_thread->IsIdle())`。
`Thread::Current::Exit()` 同样断言 `!IsIdle()`。

**验证**
构建通过；两个镜像启动正常。没有调用者，所以这条断言本身未被执行到。

**故意没做**
没有给 `Thread::Current::exit()` 补同样的断言：P1-A 计划第 20 条要在 `exit` 入口加
中断与抢占状态的断言，届时一起加，避免同一个函数被改两次。
