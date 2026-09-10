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

## `Suspend` 补成真的，panic 先让机器安静下来（2026-09-10）

**文件**
- `kernel/arch/x86/kernel/mp.cpp` — `arch_mp_suspend()` 不再是空函数；处理器清掉自己的 Online 位
- `kernel/include/ours/ipi_service.hpp` / `kernel/kernel/task/ipi_service.cpp` — `suspend_others()`
- `kernel/top/panic.cpp` — panic 开口之前先停下别的 CPU

**症状**
四核下**读不了 panic**。一颗 CPU 挂掉之后，另外三颗照常跑，日志是这样的：

```
[3856492000 cpu1 error] Assertion failed on ctx.curr_cpu() == m_runtime.which_cpu
[3856501000 cpu2 debug] I'm Thread 1
[3856503000 cpu3 debug] I'm Thread 2
... 之后还有八万行 ...
```

而且 `vpanic()` 会 `dump_recent()` 把整个记录环重放一遍——那几百行是**逐行**和另外三颗
CPU 的实时输出交错着出来的。今天为了找一条断言，我反复 `grep -a "Assertion" | sort -u`，
就是因为这个。

**原因**
`vpanic()` 只做了 `arch::AutoInterruptDisabler`——那让**这一颗** CPU 闭嘴，
对其余的 CPU 毫无作用。单核下"关掉本地中断"和"让机器停下来"是同一件事，
多核下不是。

停别的 CPU 的机制其实**已经写好一半了**：

- `IrqVec::IpiSuspend` 的分发在 `faults.cpp` 里，
- `x86_handle_ipi_suspend()` 也在，而且做的正是"永远停住"，
- 只有发送端 `arch_mp_suspend(CpuMask targets)` 是个**空函数体**。

一直没人发过这个事件，所以那个处理器从写好那天起就没有被执行过。

**改法**

发送端就是它看起来的那一行：

```cpp
auto arch_mp_suspend(CpuMask targets) -> void {
    arch_mp_send_ipi_mask(targets, IpiEvent::Suspend);
}
```

接收端多做一件事——**停之前先说一声**：

```cpp
auto x86_handle_ipi_suspend() -> void {
    log::info("CPU[{}] suspended", this_cpu);
    Cpu::set_state(this_cpu, Cpu::State::Online, false);
    while (1) { arch::local_irq_disable(); arch::halt(); }
}
```

这样"机器安静了"就是一件**可以观测**的事，而不是靠一个拍脑袋的延时等出来的：

```cpp
auto IpiService::suspend_others() -> void {
    auto targets = Cpu::online_mask();
    targets.set(Cpu::id(), false);
    if (targets.none()) { return; }

    arch_mp_suspend(targets);

    constexpr auto MAX_SPINS = usize{10'000'000};
    for (auto spin = usize{0}; spin < MAX_SPINS; ++spin) {
        if (Cpu::num_online_cpus() <= 1) { return; }
        arch::spin_hint();
    }
}
```

**有意做成尽力而为**：它被调用的时候，事情已经出错了。一颗卡死的 CPU、
或者一颗中断关得比这个上界还久的 CPU，就是不应答——调用者不等它，
也不为此再报一个错（正在 panic 的路径上，报错只会把真正的消息再往下推一行）。
上界的存在就是为了不让一颗答不上来的 CPU 把 panic 一起拖死。

`targets.none()` 那个提前返回不只是优化：引导早期 `XAPIC` 还没初始化，
而那时在线的只有 BSP 自己，所以这条路径天然不会去碰一个空指针。

panic 侧：

```cpp
auto vpanic(...) -> void {
    arch::AutoInterruptDisabler local_irq_disabler{};
    quiesce_machine();          // <- 新增
    logz4::dump_recent();
    ...
}
```

**参照**
Linux `panic()` → `smp_send_stop()`，同样在自己开始打印之前、同样是尽力而为
（`smp_send_stop` 之后只等一小段固定时间）。这里等的是可观测的状态而不是时间，
因为 `Cpu::online_mask()` 今天已经是原子的（见 `docs/changes/260910/cpu.md`）。

**验证**
临时探针：起一个线程，睡 200 ms 之后 `panic("XXX deliberate test panic on CPU[{}]")`。
四核实测：

```
10655:[4410121000 cpu3 info] CPU[3] suspended
10656:[4410076000 cpu1 info] CPU[1] suspended
10657:[4410176000 cpu2 info] CPU[2] suspended
...（记录环重放，独占输出，无交错）...
10914:[4423053000 cpu0 error] XXX deliberate test panic on CPU[0]
```

panic 那一行之后**总共只剩 1 行**（QEMU 的终止信息）。改动前这里是几万行。

撤掉探针后：四核 debug 零断言零故障、四核 release 起到 ring 3、单核零故障。

**故意没做**
- **没有加"恢复"**。被 suspend 的 CPU 是永久停住的，`IpiEvent::Suspend` 的语义
  就是这个（处理器一直是这么写的）。真要做 CPU 热插拔或者挂起/恢复，
  需要的是另一套东西，不是给这个加个反操作。
- **`IpiEvent::Interrupt` 仍然没有发送方，`x86_handle_ipi_interrupt()` 仍然是空的。**
  和 Suspend 不一样——Suspend 的处理器写明了它要做什么，只缺一个发送方；
  Interrupt 两头都是空的，没有任何东西说明它本来打算表示什么。
  按本仓库的惯例，空的东西默认当脚手架，不删；但它需要的是**一个用途**，
  而不是我替它编一个。记在 `docs/repairs/260910/ipi.md`。

## 多播只进出一次中断禁用窗口，请求只构造一次（2026-09-10）

**文件**
- `kernel/arch/x86/lib/arch/include/arch/x86/apic/xapic.hpp` — 新增 `send_ipi_multicast()`
- `kernel/arch/x86/apic/ipi.cpp` — 请求构造抽出来；多播不再是 N 次单播
- 全树：拼写 `mulitcast` → `multicast`

**症状**
不是 bug，是每个目标都白付一遍固定开销。这条路径在多核打通之后才真正跑起来
（每一次跨 CPU 唤醒都走它）。

**原因**
```cpp
auto apic_send_ipi_multicast(CpuMask const &targets, ...) -> void {
    targets.for_each([...](CpuNum cpunum) { apic_send_ipi_unicast(cpunum, vector, mode); });
}
```

而每一次 `apic_send_ipi_unicast()`：

1. 从零构造一个 `IpiRequest`——六次 `set_field`，可是**只有目标那一个字段随目标变**。
   物理投递模式下一次只能寻址一个 APIC，所以"多播确实是 N 次发送"，
   但那不等于"N 次重建同一个请求"。
2. `XApic::send_ipi()` 里各自一次 `AutoInterruptDisabler`——每个目标一对
   `pushf/cli/popf`。两次 ICR 写之间必须关中断（否则中断处理里自己发一发 IPI
   会覆盖 `IcrHigh`），但那个窗口可以**罩住整个多播**，而不是每个目标开关一次。

**改法**
```cpp
// xapic.hpp
template <typename Dests>
auto send_ipi_multicast(IpiRequest request, Dests const &dests) noexcept -> void {
    AutoInterruptDisabler guard;                 // 整批一次
    for (auto const dest: dests) {
        wait_icr_idle();
        request.set_dest(dest);                  // 只改这一个字段
        write_reg(XApicRegType::IcrHigh, request.icr_high());
        write_reg(XApicRegType::IcrLow, request.icr_low());
    }
}
```
```cpp
// ipi.cpp
u32 dests[MAX_CPU];
auto count = usize{0};
targets.for_each([&](CpuNum cpunum) { dests[count++] = cpunum_to_apic_id(cpunum); });
...
XAPIC->send_ipi_multicast(make_ipi_request(vector, mode),
                          ustl::views::Span<u32 const>(dests, count));
```

CPU 号到 APIC ID 的翻译**放在关中断之前**：一来那是这一层才知道的映射
（`XApic` 不该认识 `CpuNum`），二来这样关中断的窗口里只剩寄存器写。

**这是一次取舍，不是纯赚**：中断禁用窗口从"每个目标一小段"变成"整批一整段"。
Linux 的 `default_send_IPI_mask_sequence_phys()` 是同一个选择——
`local_irq_save()` 取一次，循环在里面。N 次进出窗口的代价比窗口本身长这件事更贵。

顺带两处：

- `XApic::send_ipi(u32, IpiRequest const &)` 这个重载**第一个参数从来没被用过**，
  只是转调另一个。删掉，单播直接调 `send_ipi(request)`。
- 全树的 `mulitcast` 拼写改成 `multicast`（6 处）。

**参照**
Linux `default_send_IPI_mask_sequence_phys()`（`arch/x86/kernel/apic/ipi.c`）：
`local_irq_save()` 一次，然后循环里逐个 `__default_send_IPI_dest_field()`。

**验证**
四核引导零断言零故障：三颗 AP 正常上线（INIT/SIPI 走单播），
12 次 IPI-Resched 正常投递（走多播），
cpu0 8609 / cpu1 548 / cpu2 76894 / cpu3 76970。

**故意没做**
`wait_icr_idle()` 仍然在循环**里面**、每个目标一次——这是硬件要求的
（下一次写 ICR 之前它必须空闲），不是可以提出去的东西。

## 带上界的 ICR 等待，AP 起不来时引导不再挂死（2026-09-10）

**文件**
- `kernel/arch/x86/lib/arch/include/arch/x86/apic/xapic.hpp` — `try_wait_icr_idle()` / `try_send_ipi()`
- `kernel/arch/x86/apic/ipi.cpp` / `.../ours/arch/apic.hpp` — `apic_try_send_ipi_unicast()`
- `kernel/arch/x86/kernel/mp.cpp` — `x86_bring_aps()` 的两处无界自旋都加了上界

**症状**
一颗起不来的 CPU 会让引导**永久挂死，而且一个字都不打印**。两处：

```cpp
apic_send_ipi_unicast(cpu, vec, ApicDeliveryMode::Startup);   // 里面 wait_icr_idle() 无上界
...
while (!Cpu::is_online(cpu))                                   // 无上界
    ;
```

而且这段是关着中断跑的（`arch::local_irq_disable()` 就在上面两行），
也就是说挂在这里连时钟中断都不会再来。这是引导里最没人能替它报信的位置。

**原因**
两个自旋都在等**别人**：ICR 等目标 APIC 接收，`is_online` 等目标 CPU 走完自己的 bring-up。
两者都可能永远等不到——固件报了一颗机器上并不存在的 CPU、SIPI 没被接受、
AP 在跳板里死掉。而"等不到"在这里的表现是静默挂死，不是错误。

**改法**

ICR 侧加一个有上界的兄弟，Linux 也是这么成对的
（`__xapic_wait_icr_idle()` 旁边就是 `safe_apic_wait_icr_idle()`）：

```cpp
[[nodiscard]]
auto try_wait_icr_idle() const noexcept -> bool {
    for (auto spin = usize{0}; spin < MAX_ICR_SPINS; ++spin) {
        if ((read_reg(XApicRegType::IcrLow) & ICR_DELIVERY_STATUS_PENDING) == 0) {
            return true;
        }
        spin_hint();
    }
    return false;
}

[[nodiscard]]
auto try_send_ipi(IpiRequest const &request) noexcept -> bool {
    AutoInterruptDisabler guard;
    if (!try_wait_icr_idle()) { return false; }    // 没发出去，什么也没写
    ...
}
```

上界是**迭代次数而不是时长**，有意的：它的调用者正是那些"还没有时钟可问"的路径——
唤醒第一颗 AP 就在其中。

上线等待侧则用**时限**，因为到那一步时钟已经有了（同一个函数上面几行就在读它）：

```cpp
auto const started = time::SteadyClock::now();
while (!Cpu::is_online(cpu)) {
    if (time::SteadyClock::now() - started > STARTUP_TIMEOUT) {
        log::error("CPU[{}] did not come online within {}ms; carrying on without it", ...);
        return;
    }
    arch::spin_hint();
}
```

**放弃的代价是零**：下游的一切都以 `Cpu::online_mask()` 为准，
所以一颗永远不来的 CPU 就是一颗这台机器没有的 CPU。

**参照**
- Linux `safe_apic_wait_icr_idle()`（`arch/x86/kernel/apic/apic.c`）——
  和无界版成对存在，用在 NMI 投递和 CPU 上线这两条"挂了没人能报信"的路径上。
- Linux `do_boot_cpu()`：等不到就 `pr_err("CPU%d: didn't respond")` 然后继续。

**验证**
正常路径：四核零故障、三颗 AP 正常上线；`SMP=8` 也实测过，七颗 AP 全部上线、零故障。

**超时路径专门造了一次**——临时跳过发给 CPU[2] 的 SIPI：

```
[4116580000 cpu0 info] XXX deliberately not sending the SIPI to CPU[2]
[5130239000 cpu0 error] CPU[2] did not come online within 1000ms; carrying on without it
[5131282000 cpu3 info] CPU[3] is online
```

恰好 1013 ms 之后报错，然后**引导继续走完**（CPU[3] 照常上线，零故障）。
改动前这里是永久挂死且不打印任何东西。

（第一次尝试是把 `STARTUP_TIMEOUT` 临时设成 0，没触发——因为 AP 在那 1 ms 的
`sleep_for` 之后**已经在线**了，循环体压根没进。所以才改用"造一颗真起不来的 CPU"。）

**故意没做**
`x86_bring_aps()` 里那句 `arch::local_irq_disable()` 没有动。它之后没人重新打开中断，
于是整个第二阶段（`reclaim_init_area()`、`userboot_init()`、spawn）都在关中断下跑，
直到第一次上下文切换才恢复。那是另一件事，仍记在 `docs/repairs/260909/x86-mp.md` 第 2 条。

## 第二阶段不再整段关着中断跑（2026-09-10）

**文件**
- `kernel/arch/x86/kernel/mp.cpp` — 删掉 `x86_bring_aps()` 里那句 `arch::local_irq_disable()`

**症状**
`init_platform()` 返回时中断是**关着的**，而且此后再没人打开：

```
[3992891000 cpu0 info] after init_platform: irqs_disabled=true
```

于是整个第二阶段——`mm::EarlyMem::reclaim_init_area()`、`userabi::userboot_init()`、
`labour_routine()` 里 spawn 的每一个线程——全都在关中断下跑，
一直到第一次上下文切换才**顺带**恢复（切换会加载新线程保存的 RFLAGS）。

这不是猜的：`IpiService::sync_exec()` 的前置断言
（"必须开着中断调用，因为它自旋等目标从中断里应答"）第一次上线就把它逮住了。

**原因**
```cpp
        apic_send_ipi_unicast(cpu, vec, ApicDeliveryMode::Startup);

        arch::local_irq_disable();          // <- 没有配对的 enable

        Thread::Current::sleep_for(Milliseconds(1), task::Interruptible::Yes);
        while (!Cpu::is_online(cpu))
            ;
```

一句没有配对的 `local_irq_disable()`，写在一个 `for_each` 的 lambda 里，
效果泄漏出整个函数、泄漏出 `init_platform()`。

**它保护的东西不存在。** 从 SIPI 发出到 AP 置上 Online 位之间，
引导 CPU 什么也不为它做——AP 的 bring-up 是自足的（跳板 → CpuLocal → local APIC →
per-CPU → `start_nonboot_cpu()`）。而它下面那两件事都不需要关中断：

- `sleep_for()` 自己用 `Guard<SpinLock, IrqSaveAndPreemptDisable>` 管中断状态
  （所以关中断反而让它**返回时把"关着"这个状态恢复回来**，这正是泄漏的机制）；
- `while (!Cpu::is_online(cpu))` 读的是原子量，中途被抢占无害；
  超时用的 `time::SteadyClock::now()` 读的是计数器，不靠中断推进。

代价倒是实打实的：等一颗 AP 最长要一秒，这一秒里这颗 CPU 连时钟中断都收不到，
什么别的线程也跑不了。

**改法**
删掉那一行，把"为什么这里中断可以开着"写进注释。

**参照**
Linux `do_boot_cpu()` 等 AP 上线的整段（`while (!cpumask_test_cpu(...))` 加
`schedule()`）是开着中断跑的，正因为等待方在这段时间里没有什么要保护的。

**验证**
`init_platform()` 返回时 `irqs_disabled=false`（同一个临时探针，改动前是 `true`）。
四种配置全跑：

| 配置 | 结果 |
|------|------|
| 四核 debug | 零断言零故障，cpu0 8609 / cpu1 602 / cpu2 88928 / cpu3 88808 |
| 四核 release | 零故障，用户态 shell 进 ring 3 |
| 单核 debug | 零故障 |
| **八核 debug** | 七颗 AP 全部上线，零故障 |

**故意没做**
`sleep_for(Milliseconds(1))` 留着。有了带时限的上线等待之后它已经不是必需的，
但它让常见情形（AP 一毫秒内就绪）根本不进自旋循环，删掉只会把功耗换成延迟。
