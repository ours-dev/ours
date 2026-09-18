# kernel/task/sched 框架重构：目标形状（2026-09-18）

前提由作者定下：**`Discipline` 会长大**。所以 `ISchedule` 挣得起它的抽象开销，
目标是把已经选定的那条路（Linux 的 `sched_class` 模型）走完，而不是退回 Zircon 的
单调度器形态。

本文档只谈**框架**。EEVDF 算术、字段语义、控制流属于 `fair.*`，由作者本人处理；
框架改动要求 `fair.cpp` 做的机械适配，逐处列在第 5 节。

---

## 1. 问题：今天每加一个 discipline 的代价

| 位置 | 加第三个 discipline 会发生什么 |
|---|---|
| `Context` 的侵入式钩子 | 再加一个成员。今天已有 `m_runqueue_hook`（Fair 的 set 节点）与 `m_rr_hook`（RR 的 list 节点） |
| `SchedParams` | 它的字段并进同一个**扁平结构体**。今天 7 个字段同型，其中 `runtime`/`prev_runtime` 是通用记账，`vruntime`/`vdeadline`/`vlag` 是 Fair 的虚拟时间代数 |
| `Scheduler` 持有队列 | 再加一个**具名成员** `m_xxx_runqueue`，并再加一个具名访问器 |
| discipline 取自己的队列 | 再写一遍 `Scheduler::of(runtime).xxx_run_queue()` —— 用 `container_of` 从 `Runtime&` 还原整个 `Scheduler` |

第二行已经造成了实害：`deadline`（零写入、三个读者）和 `vdeadline`（Fair 写四次、
零读者）能在同一个结构体里共存而编译器一言不发，因为它们同型、同名族、同一层。
只要两批字段还在一个结构体里，下一对 `x` / `vx` 就还会发生。

第四行是更根本的：**注册表的全部意义就是「加 discipline 不用改 `Scheduler`」，
而今天加一个 discipline 必须改 `Scheduler`。** 抽象没有兑现它承诺的那件事。

## 2. 目标：每 discipline 一个实体，每 discipline 一个子队列

**（a）`Context` 按 discipline 切分，纯组合，不引入基类。**

```cpp
/// 所有 discipline 共享的通用记账。框架拥有它。
struct SchedEntity {
    SchedDuration runtime;
    SchedDuration prev_runtime;
    Timeslice     timeslice;
};

/// Fair 自己的每线程状态。框架不读它，只负责它存在。
struct FairEntity {
    ustl::collections::intrusive::SetMemberHook<> hook;
    SchedDuration vruntime;
    SchedDuration vdeadline;
    SchedDuration vlag;
    SchedTime     deadline;
};

struct RrEntity {
    ustl::collections::intrusive::ListMemberHook<> hook;
};
```

**要诚实说明的一点**：这不省内存——Linux 的 `task_struct` 同样把 `se`/`rt`/`dl`
三个实体都内联着。买到的是**归属清晰**：每个 discipline 的字段装在它自己的结构体里，
所以「谁该写 `deadline`」在类型上就有答案，而加一个 discipline 是加一个成员，
不是把一个共享的扁平结构体再撑宽一圈。

**（b）子运行队列住进每 CPU 对象，并且由框架递给 discipline。**

今天 `Scheduler` 用具名成员持有各队列，discipline 靠 `Scheduler::of()` 反查。
目标是 discipline 的入口点**直接收到自己的队列**——解析在 `Schedule<Derived,
Discipline>` CRTP 基类里做一次，每个 discipline 的实现里不再出现 `Scheduler::of()`，
而 `Scheduler::of()` 转为 private。

对照：Linux 的 `struct rq` 内联 `cfs_rq cfs; rt_rq rt; dl_rq dl;`，而
`sched_class` 的钩子收 `struct rq *`；ours 多走一步，把「取出我自己那一份」也从
调用方挪进框架。

**（c）`Runtime` 解散，并入 `Scheduler`。**

`Scheduler::of()` 用 `container_of` 从 `Runtime&` 还原 `Scheduler`——这就是
`Runtime` 没有独立身份的证据，它是 `Scheduler` 的字段换了个名字。它之所以存在，
只是因为 `ISchedule` 是无状态单例、每次调用都得被告知「你在为哪个 CPU 干活」。
（b）解决了这个需求之后，`Runtime` 就没有存在理由了。

## 3. 不动的东西

- `ISchedule` / `Schedule<Derived, Discipline>` / `ScheduleEntry` / `.task.schedules`
  注册机制：前提是 discipline 会变多，这套机制正是为此存在的。
- 所有存根：`SchedulingState`（空类）、`on_thread_tick`（零调用点纯虚）、
  `override_thread_base_profile`、`Timeslice::m_extension`、
  `EffectiveProfile::initial_timeslice_ns`、`Runtime::last_updated_time` /
  `total_expected_runtime`、`Discipline::Deadline`。按作者的规矩，不删也不改形。
- `Profile` 三件套：`Base` / `Effective` / `Inherited` 是三件事，与 Zircon 同构。
- `fair.*` 的算法、字段语义与控制流。

## 4. 提交序列（框架侧）

每一步都构建通过并在四核 QEMU 启动。`fair.cpp` 不在构建里，用
`-fsyntax-only` 单独查（做法见 `docs/changes/260917/kernel-task-sched.md`）。

| # | 改动 | 会要求 fair.cpp 适配吗 |
|---|---|---|
| F1 | `SchedParams` 拆成 `SchedEntity`（通用）+ `FairEntity`（EEVDF）+ `RrEntity`，`Context` 按组合持有 | 是 |
| F2 | 两条队列按值进入每 CPU 对象；`Schedule<>` CRTP 基类解析「我自己的队列」并转发给 discipline 实现 | 是 |
| F3 | 删掉 `Scheduler::fair_run_queue()` / `rr_run_queue()`，`Scheduler::of()` 转 private | 是 |
| F4 | `Runtime` 解散并入 `Scheduler` | 是 |

## 5. 交给作者的 fair.cpp 适配清单

每完成一步，本节追加一份清单：**行号 + 现在是什么 + 该改成什么**，不含任何关于
EEVDF 语义的判断。框架侧改完后 `fair.cpp` 会编不过——它本来就不在构建里，
由作者在把 Fair 放回构建时一并处理。

（待第一步完成后填入。）
