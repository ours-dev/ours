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
