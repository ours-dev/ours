# 地址空间归属 — 待修

---

## 1. 线程的地址空间有两处真相来源

`Thread::Current::fault()`（`kernel/kernel/task/thread.cpp`）：

```cpp
if (!self->m_user_thread) { aspace = self->m_aspace; }
else { aspace = self->m_user_thread->process().aspace(); }
```

而 `Scheduler::switch_context()`（`kernel/kernel/task/sched/scheduler.cpp`）读的是
`curr->aspace()`，即 `m_aspace`。**缺页按进程的地址空间解析，CR3 按 `m_aspace` 切换。**
两者一旦不一致，就会在 A 空间里解析 B 空间的缺页。

现在碰不到，因为还没有真正的用户线程（`m_user_thread` 恒空）。做法应当是进程在建线程时把
aspace **推**给线程，`fault()` 之后只读 `m_aspace`，不再回头问进程。
