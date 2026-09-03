# 调度器 — 改动记录

格式见 `docs/changes/TEMPLATE.md`。待修项在 `docs/repairs/260903/scheduler.md`。

这一支的前史在 `docs/repairs-20260830.md`（只读归档）第三十九、四十、四十一节：
纪律注册表被 `--gc-sections` 回收、新线程在绑定纪律之前就被问要哪颗 CPU、
`CpuAffinity` 的两个掩码指针从来没被初始化过。对应提交 `10cceaf`、`da0bb97`。
本文件从那之后接着写。

---

## 1. 没有调度器 active 时，CPU 选择要能给出答案（2026-09-03）

**文件**
- `kernel/kernel/task/sched/scheduler.cpp` — `select_target_cpu()` 的兜底路径补完。

**症状**

```
Assertion failed on target_cpu != INVALID_CPU_NUM with message: At least one CPU was assigned to us
```

`task::init()` 里逐 CPU 建空闲线程时必然触发，启动停在这里。`da0bb97` 修掉了
「读地址 0」那一层之后，它依然在原地报——那一节写明了是有意停在这里。

**原因**

兜底路径是半成品。它算出了 `starting_cpu`，然后给它挂上 `[[maybe_unused]]` 就再也没用过：

```cpp
[[maybe_unused]]
auto starting_cpu = last_cpu == INVALID_CPU_NUM ? curr_cpu : last_cpu;

auto const active_mask = peek_active_mask();
auto const available_mask = se.cpu_affinity().get_available_mask(active_mask);
available_mask.for_each(...);   // 掩码是空的，循环体一次都不执行
```

于是唯一的候选来源是 active 掩码，而 active 掩码此刻是空的。这不是巧合，是
`task::init()` 的结构决定的：

```cpp
cpu_possible_mask().for_each([](CpuNum cpu) {
    Scheduler::Current::get(cpu).init();
    init_idle_task_percpu(cpu);          // 这里面 spawn，spawn 就要挑 CPU
});
Scheduler::Current::set_active(true);     // 循环之后才置位
```

**整个早期线程创建阶段都发生在「没有任何调度器处于 active」的窗口里**。空掩码和任何
亲和性求交还是空，`for_each` 不执行，`target_cpu` 保持 `INVALID_CPU_NUM`，断言必炸。

关键在于分清两种「交集为空」：一种是「这个线程哪儿都不能跑」，是真错误；另一种是
「候选集合本身是空的」，是启动期的正常状态。旧代码把两者塌缩成同一个断言。

**改法**

把兜底拆成两级，并把公共的「挑负载最轻的一颗」提成 `pick_least_loaded` lambda：

1. 先在 active 掩码里挑。这一级优先不只是因为「非 active 的调度器还没宣布自己能收线程」——
   `find_active_scheduler_for()` 会**自旋等到**它拿到的 CPU 报告 active 为止，所以回答一颗
   永远不会 active 的 CPU 是死锁，不是放错位置。
2. active 全空时放宽到 online 掩码，并优先选「线程本来所在的那颗」——`last_cpu`，
   从没跑过的就是创建它的那颗 CPU。这正是 `starting_cpu` 当初被算出来的用途。

放宽候选不等于放宽亲和性：`starting_cpu` 只是偏好，不是无视硬绑定的许可，所以它要先
通过 `get_available_mask(Cpu::online_mask())` 的检验才会被采纳；不通过就在允许集合里
挑负载最轻的。走到最后仍然是 `INVALID_CPU_NUM` 才断言——那时它才真的是
「硬亲和性排除了所有 online CPU」，是货真价实的错误。

否掉的方案：把 `set_active(true)` 提到逐 CPU 循环之前。这会让调度器在它的空闲线程存在
之前就宣称可以收线程，`m_runtime.curr_thread` 还是空，抢占路径会解引用它。窗口是真实
存在的，应该由选择逻辑承认它，而不是靠调整初始化顺序把它藏起来。

**参照**

`Linux select_task_rq()`：`cpumask_test_cpu(cpu, p->cpus_ptr)` 失败才重新挑，否则留在原地；
「没有更好的主意就别动它」是同一条原则。两级兜底对应
`select_fallback_rq()` 从 `cpu_active_mask` 逐步放宽到 `cpu_possible_mask` 的做法。

**验证**

- `prebuilt/ninja/ninja -C out/bringup.x86` 通过。
- `clang-format` 无改动；`clang-tidy -p out/bringup.x86` 在本文件只剩 4 条既有告警
  （3 条 `SCHEDULE_*_PAST` 命名、1 条 `modernize-use-nullptr`），都不在改动区。
- QEMU 实测 `kernel.dev.dbg_1.obi`：断言不再出现，启动越过 `task::init()`，
  进到 `start_kernel()` 里 spawn "laborer" 线程。lldb 经 gdbstub 取到的栈证实了这一点：

  ```
  frame #5: operator new(40) at cpp_dependencies.cpp:17
  frame #8: Thread::spawn<std::function<int ()>>(name="laborer", ...) at thread.hpp:429
  frame #10: start_kernel(...) at main.cpp:131
  ```

  即 `task::init()` 已经整段跑完。新的停点是另一回事：`Thread::spawn` 的
  `std::function` 形参在堆上分配，而内核的全局 `operator new` 是一个 panic 桩。

**故意没做**

- **没有让空闲线程绕开通用选择**。当前配置下 QEMU 只有一颗 CPU，`starting_cpu` 恰好
  就是正确答案，所以问题被掩盖了；多核下 `Cpu::id()` 始终是 BSP，于是**每一颗 CPU 的
  空闲线程都会被登记到 0 号调度器上**。空闲线程的 CPU 在 `IdleThread::init(cpunum)`
  就已知，它根本不该进选择路径。这是下一条的内容。
- **没有碰 `find_active_scheduler_for()` 的自旋**。它在「所有调度器都不 active」时会
  空转，这在本次改动前后都成立（改动前是断言，也不算能返回），不是回归。

---

## 2. 空闲线程钉在自己那颗 CPU 上，不进通用选择（2026-09-03）

**文件**
- `kernel/include/ours/cpu.hpp` — 新增 `cpu_mask_of()`，Linux 的 `cpumask_of()`。
- `kernel/include/ours/task/thread.hpp` / `kernel/kernel/task/thread.cpp` —
  `spawn_etc()` 增加尾部默认参数 `CpuNum target_cpu`。
- `kernel/include/ours/task/sched/scheduler.hpp` / `sched/scheduler.cpp` —
  `on_thread_init()` 同样增加该参数，有值时跳过选择。
- `kernel/kernel/task/idle_thread.cpp` — `IdleThread::init()` 把 `m_cpu` 传下去。

**症状**

潜伏，当前配置下触发不到：QEMU 只起了一颗 CPU，所以「选出来的 CPU」和「本该属于的 CPU」
恰好都是 0。多核下必现——`task::init()` 是**在 BSP 上**替每颗 possible CPU 建空闲线程的，
第 1 条的兜底会给出 `starting_cpu = Cpu::id()`，也就是 BSP：

> 每一颗 CPU 的空闲线程都被登记到 0 号调度器上，而它们各自的调度器没有空闲线程。

一个没有空闲线程的调度器在无事可做时无处可切，`m_runtime.curr_thread` 会停在一个不属于
它的线程上。

**原因**

空闲线程的 CPU **不是一个待决策的放置问题**，它是这个对象的定义：一颗 CPU 一个 idler，
`IdleThread::init(cpunum)` 一进来就知道答案。把它送进通用选择，是拿一个已知量去问一个
猜测器。

而且这个问题**通用选择根本答不出来**，不只是答错：`select_target_cpu()` 的两级候选集
（active 掩码、online 掩码）都是 online 掩码的子集，而 `task::init()` 遍历的是
`cpu_possible_mask()`，此时次级 CPU 一颗都还没 online。对一颗尚未 online 的 CPU，
选择逻辑没有任何可读的状态能得出正确答案——只有调用方知道。

`Thread::spawn_etc()` 此前没有任何办法表达「这个线程属于 CPU N」。

**改法**

`spawn_etc()` 和 `on_thread_init()` 各加一个尾部默认参数 `CpuNum target_cpu`，
默认 `INVALID_CPU_NUM` 表示「没有意见」，普通线程走原路。有值时：

```cpp
context.m_affinity.set_affinity_hard(cpu_mask_of(target_cpu));
```

两点是刻意的：

- **钉住这件事记进亲和性，而不只是在这里被遵守一次。** 这样下游——
  `do_unpark_thread()` 的重新选择、将来的迁移——是从线程自己的状态得出同一个结论，
  而不是依赖一个只有本函数见过的决定。
- **选择被跳过，而不是「喂一个单 CPU 亲和性然后相信它会同意」。** 它不会同意，理由同上：
  `{cpunum}` 和一个还不含 cpunum 的 online 掩码求交是空，而空正是选择逻辑唯一不能给的答案。

`cpu_mask_of()` 必须是一张静态表，不能是临时对象：`CpuAffinity` 存的是非拥有的
`CpuMask const *`，装进去的东西必须活得比装它的人久。这和 `cpu_all_mask()` /
`cpu_none_mask()` 用静态对象是同一个理由。表按 `MAX_CPU`（内核编译进去的上限）而不是
firmware 报的数量定尺寸——超出 possible 的下标没人会问，而做成动态的要为省几个字换一次分配。

**参照**

`Linux cpumask_of()`（底层是静态的 `cpu_bit_bitmap[]`），出于完全相同的「调用方要拿指针」
的理由。空闲线程钉死在本 CPU 也是 Linux 的做法：`init_idle()` 里
`set_cpus_allowed_common(idle, cpumask_of(cpu))`，同样不经过 `select_task_rq()`。

**验证**

- `prebuilt/ninja/ninja -C out/bringup.x86` 通过，无新增编译告警。
- `clang-format` 已跑；`clang-tidy -p out/bringup.x86` 对三个 `.cpp` 全跑过，
  改动区和新增的 `cpu_mask_of()` 没有产生任何新告警（其余都是既有的命名 / nullptr 条目）。
- QEMU 实测两个 image，停点与第 1 条完全一致（`start_kernel()` 里 spawn "laborer" 时的
  `operator new` panic），无回归：
  - `kernel.dev.dbg_1.obi`（开断言）
  - `kernel.dev.obi`（关断言）—— 两者停在同一处，说明不是被断言掩盖的差异。
- **多核路径未实测**：当前 QEMU 只有一颗 CPU，`m_cpu == Cpu::id() == 0`，所以这条改动
  在实测里是恒等变换。它的正确性论证是静态的，等 SMP 起来才能实证。

**故意没做**

- **没有修 `Scheduler::init()` 里的 `m_runtime.which_cpu = Cpu::id()`**。它同样在 BSP 上
  被逐 CPU 调用，于是每个调度器都记下 BSP 的号；`init()` 应当像 `TimerQueue::init(cpunum)`
  一样接收 cpunum。这条在 `docs/repairs/260903/scheduler.md` 里已经独立列着，
  留给它自己的一次改动。
- **没有把 `Fair::on_thread_init()` 里的 `Scheduler::Current::get()` 改成用 `runtime`**。
  它取的是**正在运行的**那颗 CPU 的调度器，而不是线程被放上去的那颗——参数 `Runtime &`
  就在手边却没用。目前只影响一次 `verify()`，但方向是错的。已记进待修。
