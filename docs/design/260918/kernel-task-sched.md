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

### R1 —— `Context` → `SchedEntity`（已完成）

`fair.hpp` 与 `fair.cpp` **一个字没改**，靠两个具名迁移垫片继续编译：

- `kernel/include/ours/task/sched/context.hpp` 现在是一个 6 行转发头，
  内含 `using Context = SchedEntity;`
- `Thread::scheduling_context()` 保留为转发到 `scheduling_entity()` 的内联

你自己动 Fair 的时候要做的：

| 位置 | 现在 | 改成 |
|---|---|---|
| `fair.hpp:7`、`fair.cpp:2` | `#include <ours/task/sched/context.hpp>` | `#include <ours/task/sched/sched_entity.hpp>` |
| `fair.hpp` 8 处、`fair.cpp` 22 处 | `Context` | `SchedEntity` |
| `fair.cpp` 11 处 | `->scheduling_context()` | `->scheduling_entity()` |

改完之后**删掉 `sched/context.hpp` 这个文件和 `Thread::scheduling_context()`**——
届时不会再有任何东西引用它们。

### F1 —— `SchedParams` 拆成框架记账 + `FairState`（已完成）

`fair.hpp` 仍然编译通过（它只用访问器和钩子 traits），**`fair.cpp` 从这一步起语法检查
不再通过**——这是设计如此，不是意外：那 11 处直接读写 `m_params` 的地方，正是所有权
从共享扁平结构体收归 Fair 自己的那条线。

`SchedEntity` 上的新形状：

```cpp
SchedDuration m_exec_runtime;       // 框架记账（原 m_params.runtime）
SchedDuration m_prev_exec_runtime;  // 原 m_params.prev_runtime
FairState     m_fair;               // deadline / vruntime / vdeadline / vlag / generations
```

你自己动 Fair 的时候要做的：

| fair.cpp | 现在 | 改成 |
|---|---|---|
| 176, 195 | `m_params.vlag` | `m_fair.vlag` |
| 209 | `m_params.runtime` | `m_exec_runtime` |
| 210, 217, 374 | `m_params.vruntime` | `m_fair.vruntime` |
| 216, 373, 419 | `m_params.vdeadline` | `m_fair.vdeadline` |
| 372 | `m_params.runtime` | `m_exec_runtime` |
| 387 | `m_params.runtime`、`m_params.prev_runtime` | `m_exec_runtime`、`m_prev_exec_runtime` |

访问器 `deadline()` 和 `runtime()` **名字没变**，所以 `fair.hpp:45/113` 与
`fair.cpp:51/78/79/171/212` 不用动。

**顺带一件要你决定的事**：`runtime()` 这个访问器名和 `sched::Runtime`（每 CPU 运行队列
对象）撞名，两者毫无关系。字段已经改叫 `m_exec_runtime` 了，访问器改不改叫
`exec_runtime()` 由你定——改它要动 `fair.hpp` 和 `fair.cpp`，所以我没动。

### 改名：`EnqueueControl` / `DequeueControl` → `*Options`（已完成）

`fair.hpp` 不引用这两个类型，所以不需要垫片。`fair.cpp` 有 8 处：

| fair.cpp | 现在 | 改成 |
|---|---|---|
| 8 处 | `EnqueueControl` / `DequeueControl` | `EnqueueOptions` / `DequeueOptions` |

`UnparkControl` 同批改成了 `UnparkOptions`，`fair.cpp` 另有 2 处。

若其中有形参名叫 `control` 的，一并改成 `options`——框架侧已经这么改了。

### F2 —— 队列搬进 `Runtime`，按 `Discipline` 选取（已完成）

```cpp
template <Discipline Disci>
FORCE_INLINE auto Runtime::queue() -> auto & {
    if constexpr (Disci == Discipline::Normal) {
        return m_fair_queue;
    } else {
        static_assert(Disci != Disci, "no run queue is registered for this discipline");
    }
}
```

你自己动 Fair 的时候要做的（共 19 处）：

| fair.cpp | 现在 | 改成 |
|---|---|---|
| 135 | `Scheduler::of(runtime).fair_run_queue()` | `runtime.queue<Discipline::Normal>()` |
| 17 处 | `Scheduler::Current::get().fair_run_queue()` | `runtime.queue<Discipline::Normal>()`（该函数手里有 `runtime` 时） |
| 96 | 注释里同样的拼法 | 一并改 |

**注意**：那 17 处里凡是所在函数没有 `runtime` 形参的（例如 `RunQueue` 自己的成员函数），
改不成参数形式——那类地方要么从调用方把队列传进来，要么保留 `Current::get()` 并接受它
只对本 CPU 正确。这正是 rr.cpp:9-26 记录过的那类跨 CPU 事故，值得逐处看一眼。

**`Scheduler::of()` 现在在构建里零调用者**，只剩 fair.cpp:135。F3 把它转私有时，
那一处会变成硬编译错误——这是有意的。

### 适配清单不再由我手工维护

框架每改一次，清单就重新生成一次；下面这 76 条是用当前 HEAD 跑出来的，不是我抄的。

**先修第一条**：`fair.cpp:5` 的 include 是 *fatal* 错误，不修的话编译在那里就停，
下面 75 条一条都报不出来。（生成这张表时我用的是一份改好 include 的副本，没有动 fair.cpp。）

| 文件 | 行 | 编译器说 |
|---|---|---|
| fair.cpp | 5 | #include <ours/task/sched/runtime.hpp> -> sched/run_queue.hpp (fatal; hides everything below until fixed) |
| fair.cpp | 33 | unknown type name 'Runtime' |
| fair.cpp | 51 | no member named 'runtime' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 78 | no member named 'runtime' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 79 | no member named 'runtime' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 97 | abstract class is marked 'final' [-Werror,-Wabstract-final-class] |
| fair.cpp | 101 | unknown type name 'Runtime' |
| fair.cpp | 102 | unknown type name 'Runtime' |
| fair.cpp | 104 | unknown type name 'Runtime' |
| fair.cpp | 105 | unknown type name 'Runtime' |
| fair.cpp | 107 | unknown type name 'Runtime' |
| fair.cpp | 108 | unknown type name 'Runtime' |
| fair.cpp | 109 | unknown type name 'Runtime' |
| fair.cpp | 111 | unknown type name 'Runtime' |
| fair.cpp | 113 | unknown type name 'Runtime' |
| fair.cpp | 114 | unknown type name 'Runtime' |
| fair.cpp | 115 | unknown type name 'Runtime' |
| fair.cpp | 116 | unknown type name 'Runtime' |
| fair.cpp | 117 | unknown type name 'Runtime' |
| fair.cpp | 128 | unknown type name 'Runtime' |
| fair.cpp | 129 | unknown type name 'Runtime' |
| fair.cpp | 131 | unknown type name 'Runtime' |
| fair.cpp | 134 | unknown type name 'Runtime' |
| fair.cpp | 135 | no member named 'of' in 'ours::task::sched::Scheduler' |
| fair.cpp | 139 | unknown type name 'Runtime' |
| fair.cpp | 143 | unknown type name 'Runtime' |
| fair.cpp | 167 | no member named 'fair_run_queue' in 'ours::task::sched::Scheduler' |
| fair.cpp | 171 | no member named 'runtime' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 174 | unknown type name 'Runtime' |
| fair.cpp | 175 | no member named 'fair_run_queue' in 'ours::task::sched::Scheduler' |
| fair.cpp | 176 | no member named 'm_params' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 183 | no member named 'm_params' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 195 | no member named 'm_params' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 198 | unknown type name 'Runtime' |
| fair.cpp | 199 | no member named 'fair_run_queue' in 'ours::task::sched::Scheduler' |
| fair.cpp | 209 | no member named 'm_params' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 210 | no member named 'm_params' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 212 | no member named 'deadline' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 216 | no member named 'm_params' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 217 | no member named 'm_params' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 221 | unknown type name 'Runtime' |
| fair.cpp | 222 | use of undeclared identifier 'DequeueControl' |
| fair.cpp | 227 | use of undeclared identifier 'DequeueControl' |
| fair.cpp | 235 | unknown type name 'Runtime' |
| fair.cpp | 236 | no member named 'fair_run_queue' in 'ours::task::sched::Scheduler' |
| fair.cpp | 260 | unknown type name 'Runtime' |
| fair.cpp | 261 | no member named 'fair_run_queue' in 'ours::task::sched::Scheduler' |
| fair.cpp | 291 | unknown type name 'Runtime' |
| fair.cpp | 292 | no member named 'fair_run_queue' in 'ours::task::sched::Scheduler' |
| fair.cpp | 300 | unknown type name 'Runtime' |
| fair.cpp | 301 | no member named 'fair_run_queue' in 'ours::task::sched::Scheduler' |
| fair.cpp | 306 | unknown type name 'Runtime' |
| fair.cpp | 307 | no member named 'fair_run_queue' in 'ours::task::sched::Scheduler' |
| fair.cpp | 316 | unknown type name 'Runtime' |
| fair.cpp | 317 | no member named 'fair_run_queue' in 'ours::task::sched::Scheduler' |
| fair.cpp | 331 | unknown type name 'Runtime' |
| fair.cpp | 332 | no member named 'fair_run_queue' in 'ours::task::sched::Scheduler' |
| fair.cpp | 348 | unknown type name 'Runtime' |
| fair.cpp | 349 | no member named 'fair_run_queue' in 'ours::task::sched::Scheduler' |
| fair.cpp | 363 | no member named 'fair_run_queue' in 'ours::task::sched::Scheduler' |
| fair.cpp | 372 | no member named 'm_params' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 373 | no member named 'm_params' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 374 | no member named 'm_params' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 377 | unknown type name 'Runtime' |
| fair.cpp | 378 | no member named 'fair_run_queue' in 'ours::task::sched::Scheduler' |
| fair.cpp | 387 | no member named 'm_params' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 404 | unknown type name 'Runtime' |
| fair.cpp | 405 | no member named 'fair_run_queue' in 'ours::task::sched::Scheduler' |
| fair.cpp | 419 | no member named 'm_params' in 'ours::task::sched::SchedEntity' |
| fair.cpp | 424 | unknown type name 'Runtime' |
| fair.cpp | 425 | no member named 'fair_run_queue' in 'ours::task::sched::Scheduler' |
| fair.cpp | 437 | unknown type name 'Runtime' |
| fair.cpp | 454 | unknown type name 'Runtime' |
| fair.cpp | 462 | no member named 'fair_run_queue' in 'ours::task::sched::Scheduler' |
| fair.hpp | 45 | no member named 'runtime' in 'ours::task::sched::SchedEntity' |
| fair.hpp | 113 | no member named 'deadline' in 'ours::task::sched::SchedEntity' |

重新生成的办法：把 `rr.cpp` 的真实 ninja 命令从 ninja 里拎出来指向 `fair.cpp` 的副本
（副本里先把那一行 include 改掉），加 `-fsyntax-only -ferror-limit=0`，输出去掉 ANSI 转义。

（清单随每次框架改动自动重生成，见上。）
