# dpc 改动记录

## 每颗 CPU 都要有自己的 DpcRunner —— 存储与工作线程分开建（2026-09-10）

**文件**
- `kernel/include/ours/dpc.hpp` — `init()` 拆成 `init(CpuNum)` + `start_workers(CpuNum)`
- `kernel/kernel/task/dpc.cpp` — 两个 init 钩子，分别在 Threading 和 Platform

**症状**
潜伏，但一碰就死：**任何线程在非 0 号 CPU 上退出**都会页错误。

```
[... cpu2 error] Unhandled page fault: vector=14, error_code=0x2
[... cpu2 error]   rip=... <ours::DpcRunner::Queue::enqueue(...)>   rcx=0x0
```

`Thread::Current::exit()` 会把销毁自己的那个 Dpc 入队到**当前 CPU**的队列上。

**原因**
`DpcRunner::init()` 只在**运行它的那颗 CPU** 上建：

```cpp
auto DpcRunner::init() -> void {
    auto &runner = Current::get();      // 当前 CPU
    new (&runner) Self();
    ...
}
GKTL_INIT_HOOK(DpcRunnerInit, start_dpc_runner, gktl::InitLevel::Threading);
```

而钩子跑在发布这一级的那颗 CPU 上，也就是 BSP。文件里的注释**自己承认了这个洞**：

> This covers the boot CPU only, which is what a hook can do ... A secondary
> CPU still owes itself this call from its own bring-up path.

而"secondary CPU 自己的 bring-up 路径"从来没有还过这笔账——在这轮之前，
根本就没有一条能用的 secondary bring-up 路径。

于是 AP 的 `DpcRunner` 一直是 `CpuLocal` 发下来的**零填充**存储。
零着的侵入式链表不是空链表：它的表头是 `{null, null}` 而不是自指，
`List::insert()` 又不查 `empty()`、直接对着表头链，所以 `link_before()`
顺着表头那个空的 prev 指针写了出去。（同一个坑今天在 `IpiService` 的信箱上也踩过一次。）

**为什么不能简单地"给每颗 CPU 都调一次 init()"**
因为 `init()` 干了两件时机要求**相反**的事：

| 做什么 | 什么时候必须完成 | 什么时候才能做 |
|--------|-----------------|---------------|
| 建存储（placement-new） | 任何人入队**之前** | 随时，不需要目标 CPU 在线 |
| 起工作线程 | 队列需要被排空之前 | 目标 CPU **在线之后** |

工作线程是用 `set_cpu_affinity_hard(CpuMask::of(cpu))` 钉到自己那颗 CPU 上的，
而 `resume()` 要经过 `select_target_cpu()`，它的候选集全是 `Cpu::online_mask()` 的子集——
把线程钉到一颗还没上线的 CPU 上，选择的结果是空集。
而 `InitLevel::Threading` 比 `init_platform()`（AP 在那里被唤醒）早两级。

**改法**
拆成两个函数、两个钩子：

```cpp
/// 只建存储，不起线程
auto DpcRunner::init(CpuNum cpu) -> void {
    new (&Current::get(cpu)) Self();
}

auto DpcRunner::start_workers(CpuNum cpu) -> void { ... }
```
```cpp
// InitLevel::Threading：所有可能的 CPU 的存储 + BSP 自己的工作线程
cpu_possible_mask().for_each([](CpuNum cpu) { DpcRunner::init(cpu); });
DpcRunner::start_workers(Cpu::id());

// InitLevel::Platform：AP 上线之后，补它们的工作线程
auto secondaries = Cpu::online_mask();
secondaries.set(Cpu::id(), false);
secondaries.for_each([](CpuNum cpu) { DpcRunner::start_workers(cpu); });
```

BSP 的时序**一点没变**（存储和工作线程仍然都在 Threading 就绪），
所以 Threading 到 Platform 之间在 CPU 0 上入队的 Dpc 行为和以前一致。

**参照**
本仓库自己的同类拆分：`task::init()` 在 BSP 上给每颗可能的 CPU 建调度器和 idler，
`time::init()` 建每颗 CPU 的 TimerQueue，`IpiService::init()` 建每颗 CPU 的信箱——
"能替别人建的就在 BSP 上一次建完"。Linux 的 `softirq`/`tasklet` 每 CPU 结构同样是
`per_cpu` 静态初始化，工作线程（`ksoftirqd/N`）则在 CPU 上线时才 spawn
（`smpboot_register_percpu_thread`）。

**验证**
临时探针：起一个线程、让它在 AP 上**退出**（`return 0` → `Thread::Current::exit()`
→ 入队 teardown Dpc）：

```
[4157651000 cpu2 info] exit probe running on CPU[2], about to exit
```

改动前这里是 `Unhandled page fault ... DpcRunner::Queue::enqueue`；改动后无故障。
另一个临时探针确认四颗 CPU 的工作线程都起来了：

```
[4061179000 cpu0 info] starting Dpc workers for CPU[0]
[4136874000 cpu0 info] starting Dpc workers for CPU[1]
[4138374000 cpu0 info] starting Dpc workers for CPU[2]
[4139717000 cpu0 info] starting Dpc workers for CPU[3]
```

（前者在 Threading，后三者在 Platform，都由 BSP 发起，符合设计。）
撤掉探针后：四核零断言零故障（cpu0 8609 / cpu1 542 / cpu2 76979 / cpu3 76869），
单核零故障。

**故意没做**
`DpcRunner::enqueue()` 仍然只能"排到当前 CPU"——`dpc.hpp` 的注释写着
"there is deliberately no 'run this on CPU N' option yet"，没有动。
