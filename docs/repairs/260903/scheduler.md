# 调度器 — 待修

**这两条都已经有别的 agent 在处理，本文件只做交叉索引，不要重复认领。**

它们是 2026-09-03 验证 kernel/time 的 tick 时撞上的（见
`x86-clock.changes.md` 的「在 QEMU 上实测」一节），不是专门审查调度器得到的。
写下来之后发现共享的 `docs/repairs-20260830.md` 里已经有人分析得更深：

| 现象 | 别人的记录 |
|---|---|
| `context.schedule()` 为空，`scheduler.cpp:120` 跳到空指针 | `docs/repairs-20260830.md` 第三十九节「调度纪律注册表是空的：一个通配符吃掉了整个节」 |
| 新线程在绑定纪律之前就被问要哪颗 CPU | 同文件第四十节 |

他们的结论比我的更靠前一层：不是 `m_schedules[]` 填错，而是
`.init.rodata.schedules` 这个 linker section 被 `--gc-sections` 回收了——
`task.ld` 里写的是裸的 `*(.init.rodata.schedules)`，没有 `KEEP`，而
`kernel.ld` 里同类的三个注册表都包在 `KEEP()` 里。

---

## 只有这一条是我这边补充的

`Scheduler::init()`（`kernel/kernel/task/sched/scheduler.cpp:214` 起）末尾：

```cpp
m_runtime.which_cpu = Cpu::id();
```

而 `task::init()` 是**从 BSP** 上替每颗 possible CPU 调它的，所以每个 scheduler 记下的都是
BSP 的号。`init()` 应当接收 cpunum 参数——`TimerQueue::init(cpunum)` 就是这么做的，
理由见 `kernel-time.changes.md` 第二十七节「绑定 CPU 号」那一段。

（如果第四十节已经把这条一起修了，本节可删。）
