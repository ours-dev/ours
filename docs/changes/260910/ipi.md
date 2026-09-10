# ipi 改动记录

## `IpiService::sync_exec()`：让"通用 IPI"真的通用（2026-09-10）

**文件**
- `kernel/include/ours/ipi_event.hpp` — 补上 `IpiEvent::Generic`
- `kernel/include/ours/ipi_service.hpp` — `SyncTask` / `sync_exec()` / `handle_generic()`
- `kernel/kernel/task/ipi_service.cpp` — 实现；`init()` 从空壳补成真的
- `kernel/arch/x86/kernel/mp.cpp` — `x86_handle_ipi_generic()` 接上；事件映射改成全覆盖
- `kernel/top/main.cpp` — 在 Irq 级之后调 `IpiService::init()`

**症状**
不是崩溃，是框架缺了中间那一层。想在别的 CPU 上"做一件事"的客户，只能这么写：

```cpp
// kernel/arch/x86/kernel/prof_pmu.cpp
g_ipi_action.store(static_cast<std::uint32_t>(IpiAction::Start), MemoryOrder::Release);
ours::apic_send_ipi_mulitcast(ours::Cpu::online_mask(), ::arch::IrqVec::IpiGeneric,
                              ::arch::ApicDeliveryMode::Fixed);
```

——越过 `IpiService`，直接调 arch 的 APIC 层；再靠 `faults.cpp` 里硬写一行接住：

```cpp
case IrqVec::IpiGeneric:
    x86_handle_ipi_generic();   // 空函数
    x86_handle_prof_ipi();      // 唯一的真实客户，硬编码在这里
    apic_issue_eoi();
```

**原因**
**一发 IPI 除了向量号什么都不携带。** 所以"通用 IPI"这个向量本身说不出要做什么——
第二个客户来了，只能再往 `faults.cpp` 的那个 `case` 里加一行，并且和第一个客户
共享同一个向量、各自靠全局变量猜是不是在说自己。这不是可扩展的形状，
是"目前只有一个客户所以还没塌"。

缺的那一层是：**载荷放在目标的信箱里，向量只负责说"去看信箱"**。
Linux 是 `smp_call_function_many()`，Zircon 是 `mp_sync_exec()`。

顺带两处：

- `IpiService::send_event()` **声明了但没有定义**，谁调谁链接失败——一个只有一半的 API。
- `ipi_evnet_to_irqvec()`（拼写也错了）的 `switch` 后面还有一句
  `return IrqVec::IpiGeneric`，意思是"任何漏掉的事件都当成 Generic 投递"——
  静默误投，而不是编译错误。

**改法**

三个类型，各管一件事：

```cpp
/// 请求本身：一次 sync_exec 一个，被所有目标共享
struct SyncCall {
    IpiService::SyncTask task;
    void *context;
    std::atomic<CpuMask> outstanding;   // 还没应答的 CPU
};

/// 某一个目标在它自己队列里的位置
struct SyncSlot {
    SyncCall *call;
    ListMemberHook<> queue_hook;
};

/// 一颗 CPU 被要求做的事
struct Mailbox {
    sync::SpinLock lock;
    SyncSlotList calls;
};
```

`SyncCall` 和 `SyncSlot` **必须分开**，这是实测撞出来的：一个侵入式钩子同时只能挂在
一条链表上，而一个请求要同时进好几颗 CPU 的信箱。把同一个节点 push 三次，
链表在第二次就 `ud2` 了（`#UD Invalid opcode, rip=+0x2205b`，正落在 `sync_exec` 里）。
Zircon 是同样的拆法：一个 `mp_sync_context`，外加每颗 CPU 一个 `mp_ipi_task` 指向它。

```cpp
auto IpiService::sync_exec(CpuMask targets, SyncTask task, void *context) -> void {
    auto const self = Cpu::id();
    auto const run_here = targets.test(self);
    targets &= Cpu::online_mask();
    targets.set(self, false);
    ...
    SyncCall call{.task = task, .context = context, .outstanding = targets};
    SyncSlot slots[MAX_CPU]{};                 // 都在调用者栈上

    targets.for_each([&](CpuNum cpu) {
        slots[cpu].call = &call;
        auto &mailbox = CurrentMailbox::get(cpu);
        Guard guard{mailbox.lock};             // 一次只持一把，不成环
        mailbox.calls.push_back(slots[cpu]);
    });

    send_event(targets, Event::Generic);
    if (run_here) { task(context); }           // 自己那份就地跑，不能中断自己
    while (call.outstanding.load(std::memory_order_acquire).any()) {
        arch::spin_hint();
    }
}
```

**请求放在调用者栈上**，不分配。这是 Zircon 的做法，理由也一样：第一个客户可能远早于
堆存在。安全性来自 `sync_exec()` 在 `outstanding` 清空之前不返回，而目标对这块内存的
**最后一次触碰**就是清掉自己那一位——所以栈帧不会在还有人读它的时候消失。
应答端的注释把这条写死了。

**为什么 `init()` 不能是空的**
`Mailbox` 是 `CPU_LOCAL` 对象，里面有自旋锁和一条侵入式链表。CPU_LOCAL 的模板内存是
**零**，而一条侵入式链表零着的时候不是"空链表"——它的哨兵得指向自己。第一次
`push_back()` 就顺着空的 next 指针写了出去：

```
[3946694000 cpu3 error] Unhandled page fault: vector=14, error_code=0x2
[3947223000 cpu3 error]   rip=0xffffff8840022017 (+0x22017)   <- sync_exec 里
```

所以 `IpiService::init()` 要 placement-new 每颗 CPU 的信箱，和
`Scheduler::init()`、`IdleThread::init()`、`DpcRunner::init()` 解决同一个问题的方式一致。
它原来是个空函数**而且全树没有调用者**——这正是那种"看着像死代码、其实是脚手架"的东西。

调用点放在 `set_init_level(InitLevel::Irq)` **之后**：填 `cpu_possible_mask()` 的 MADT
钩子挂在那一级上；而 AP 是 `init_platform()`（两级之后）才起来的，所以"信箱先于
第一发 generic IPI 存在"是自动成立的。

**两条前置条件，都用断言写死了**
- **必须开着中断调用。** 它自旋等目标应答，而目标是从**中断处理**里应答的；
  关着中断自旋的调用者服务不了目标正在等的那个 sync_exec，两边会互相干等。
- **task 跑在中断上下文**：不能阻塞、不能拿中断外持有的锁、不能再调 `sync_exec()`。

第一条一上来就抓到了一个真问题：`init_platform()` 返回时**中断是关着的**——
`x86_bring_aps()` 里那句 `arch::local_irq_disable()` 之后没人打开，
于是整个第二阶段（`reclaim_init_area()`、`userboot_init()`、spawn）都在关中断下跑，
直到第一次上下文切换才恢复。这条单独记在 `docs/repairs/260909/x86-mp.md`。

**参照**
- Zircon `mp_sync_exec()` / `mp_sync_context` / `mp_ipi_task`（`kernel/kernel/mp.cc`）——
  形状直接照搬：请求在调用者栈上，每 CPU 一个链表节点，目标清位，调用者自旋。
- Linux `smp_call_function_many()`：同一个操作，但用的是每 CPU 的 llist 加
  `call_single_data`，并且支持异步。这里先做同步版，因为它不需要分配、
  在早期引导可用，也正好覆盖 profiler 那个"在每颗 CPU 上开/关计数器并等确认"的用例。

**验证**
临时探针从一个线程（中断开着）里对所有在线 CPU 发起 `sync_exec`：

```
[3995676000 cpu3 info] probe: irqs_disabled=false online=4
[3996315000 cpu3 info] sync_exec ran on CPU[3]     <- 调用者自己就地跑
[3996835000 cpu0 info] sync_exec ran on CPU[0]     <- 经 generic IPI 应答
[3996814000 cpu2 info] sync_exec ran on CPU[2]
[3997795000 cpu3 info] sync_exec returned, ran on 3 of 4 CPUs
```

探针里排掉了 CPU[1]：它在这次引导里更早就死于一条**与本改动无关**的断言
（`ctx.curr_cpu() == m_runtime.which_cpu`，`reschedule_common`），
死在 `vpanic` 的关中断 halt 里，永远不会应答。已用纯 HEAD（不带本改动）复现确认。
探针撤掉后再启动一次，故障集合与纯 HEAD 完全一致。

**故意没做**
- **profiler 还没迁过来**。它仍然自己拼动作字、自己调 APIC，`faults.cpp` 里
  `x86_handle_prof_ipi()` 那一行也还在。那是下一步，迁完才能把绕行和硬编码一起删掉。
- **没有异步版**。Linux 的 `smp_call_function_single_async()` 那种"发了就走"需要
  节点的所有权模型（不能放调用者栈上），等有客户再说。
- `IpiEvent::Interrupt` 仍然没有发送方，`x86_handle_ipi_interrupt()` 仍然是空的；
  `arch_mp_suspend()` 也仍然是空壳。收尾那一步再处理。

## profiler 迁到 `sync_exec`，顺手修掉"只有一颗 CPU 真的动了"（2026-09-10）

**文件**
- `kernel/arch/x86/kernel/prof_pmu.cpp` — 删掉自建的那一套，改调 `sync_exec()`
- `kernel/arch/x86/kernel/faults.cpp` — 删掉硬编码的 `x86_handle_prof_ipi()`
- `kernel/arch/x86/kernel/include/ours/arch/x86/faults.hpp` — 删掉它的声明

**症状**
四核下 **PMU 采样只在一颗 CPU 上生效**，而停止路径必定超时并打印
"prof: N CPU(s) did not acknowledge the stop"。

**原因**
IPI 携带不了载荷，所以 profiler 用一个全局动作字来传：

```cpp
g_ipi_action.store(static_cast<std::uint32_t>(IpiAction::Start), MemoryOrder::Release);
ours::apic_send_ipi_mulitcast(ours::Cpu::online_mask(), ::arch::IrqVec::IpiGeneric, ...);
```

接收端：

```cpp
auto const action = static_cast<IpiAction>(g_ipi_action.exchange(
    static_cast<std::uint32_t>(IpiAction::None), MemoryOrder::Acquire));
```

`exchange`。**第一颗跑到这里的 CPU 把动作取走并写回 `None`**，其余每一颗读到的都是
`None`，于是什么也不做。四核下"给所有 CPU 开计数器"实际只开了一个。

停止路径因此必定失败：它等 `g_quiesce_count` 涨到 `num_online_cpus()`，
而只有一颗 CPU 会去 `fetch_add`。于是空转一亿次然后告警。

单核下 `exchange` 恰好是对的（只有一个消费者），所以这个错和这轮里其它几个一样，
是"单核下碰巧成立"。

这不是把 `exchange` 换成 `load` 就能了事的：那样又没人清理动作字，下一发无关的
generic IPI 会把上次的动作再执行一遍。真正缺的是"这一发 IPI 是发给谁的、要做什么"
这件事本身——也就是 `sync_exec()`。

**改法**
```cpp
auto prof::arch::start_pmu_sampling(std::uint64_t period) -> ours::Status {
    if (period == 0) {
        return ours::Status::BadState;
    }
    ours::IpiService::sync_exec(
        ours::Cpu::online_mask(),
        [](void *context) { program_this_cpu(*static_cast<std::uint64_t *>(context)); }, &period);
    return ours::Status::Ok;
}

auto prof::arch::stop_pmu_sampling() -> void {
    ours::IpiService::sync_exec(
        ours::Cpu::online_mask(), [](void *) { teardown_this_cpu(); }, nullptr);
}
```

`&period` 是**栈上局部变量的地址**，而这正当——`sync_exec()` 在所有目标都跑完之前不返回，
局部变量的生命期恰好覆盖整个调用。这也是这个 API 先做同步版的理由之一。

一起删掉的：`IpiAction` 枚举、`g_ipi_action`、`g_ipi_period`、`g_quiesce_count`、
`teardown_this_cpu()` 里那次 `fetch_add`、整个 `x86_handle_prof_ipi()`、
它在 `faults.hpp` 的声明，以及 `faults.cpp` 里那行硬编码调用。
`IrqVec::IpiGeneric` 的分发现在只有一行 `x86_handle_ipi_generic()`，
第二个客户不需要再动 `faults.cpp` 一个字。

**参照**
Linux 的 perf 用 `smp_call_function()` 做同一件事（`perf_event_enable`／
`perf_event_disable` 一路下到 `cpu_function_call`），不是全局动作字。

**验证**
四核引导零断言、零故障，用户态 shell 在 ring 3 打印。

**未实测**：PMU 这条路径在 QEMU 上跑不到——引导日志明说
`prof: no architectural PMU; use 'prof.start tick'`，TCG 不模拟架构 PMU，
`prof.start pmu` 根本不会走到 `start_pmu_sampling()`。
所以本节验证的是"迁移没有把引导弄坏"，加上**新增部分的单独实测**：用一个临时探针，
按和 `start_pmu_sampling()` 一模一样的形状（栈上局部变量按地址传入）发起 `sync_exec`：

```
[4043173000 cpu2 info] cpu[2] got context DEADBEEF
[4043191000 cpu3 info] cpu[3] got context DEADBEEF
[4044498000 cpu0 info] cpu[0] got context DEADBEEF
[4043731000 cpu1 info] cpu[1] got context DEADBEEF
[4045239000 cpu2 info] sync_exec with context returned
```

四颗全部收到，等待正常收尾。

**故意没做**
`program_this_cpu()` / `teardown_this_cpu()` 的内容一个字没动——它们写的是 PMU MSR 和
LVT PMI，和这次改的"怎么把它们送到每颗 CPU"是两件事。

## `send_resched` 的两个重载不再各说各话（2026-09-10）

**文件**
- `kernel/kernel/task/ipi_service.cpp`

**症状**
潜伏，而且是"给下一个人挖的坑"——`send_resched(CpuNum)` **今天没有任何调用者**
（`PreemptionState::flush_pending_remote()` 和 `Scheduler::reschedule_mask()`
用的都是掩码版）。

**原因**
两个重载对"哪些目标该被排除"的答案不一样：

```cpp
auto IpiService::send_resched(CpuNum cpu) -> void {
    if (!task::Scheduler::peek_active_mask().test(cpu)) {   // 只过滤 inactive
        return;
    }
    arch_mp_reschedule(cpu);
}
```

掩码版还多做一件事，而且注释写得很清楚为什么必须做：

```cpp
    auto targets = mask;
    targets.set(local_cpu, 0);        // 本地 CPU 永远不是目标
    targets &= task::Scheduler::peek_active_mask();
```

「给自己发重调度 IPI」在掩码版里被明确当成 bug 处理（注释里记着它曾经导致
引导日志里满屏的 "IPI-Resched on CPU[0]"），在单 CPU 版里却是允许的。
两者的差异没有任何理由，只是没人走过那条路所以没付出代价。

**改法**
```cpp
auto IpiService::send_resched(CpuNum cpu) -> void {
    send_resched(CpuMask::of(cpu));
}
```

规则只有一处。

**参照**
无，本项目自有设计。

**验证**
四核引导零断言零故障。行为上是空操作——没有调用者——所以这一条修的是
"第一个调用它的人会踩到什么"。

**故意没做**
`arch_mp_reschedule(CpuNum)` 这条 arch 接缝现在也没有调用者了，没有删：
它和 `arch_mp_reschedule_mask()` 是对称的一对，属于给 arch 层留的形状。
