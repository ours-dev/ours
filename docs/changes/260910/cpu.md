# cpu 改动记录

## `Cpu` 的 Online 掩码变成原子的（2026-09-10）

**文件**
- `kernel/include/ours/cpu.hpp` — Online 独立成 `std::atomic<CpuMask>`；三个访问器的语义
- `kernel/kernel/cpu.cpp` — 新静态成员的定义
- `kernel/include/ours/task/sched/context.hpp` — `CpuAffinity` 改持原子单元
- `kernel/kernel/task/sched/scheduler.cpp` — 那唯一一个"跟随"调用点

**症状**
潜伏，而且是这轮多核工作**自己造出来的依赖**：AP 上线的握手全靠这个掩码，
但它是个普通变量。

```cpp
// AP 侧，kernel/top/main.cpp
Cpu::set_state(Cpu::State::Online);

// BSP 侧，kernel/arch/x86/kernel/mp.cpp
while (!Cpu::is_online(cpu))
    ;
```

**原因**

**1. 空循环体 + 普通读 = 允许被提出循环。** 语言层面没有任何东西阻止编译器把
`is_online(cpu)` 的读提到循环外，然后拿一个陈旧的字永远转下去。
这次的构建碰巧没有那么干，但那是优化器的选择，不是代码的保证。

**2. 没有 release/acquire 配对。** BSP 看见这一位的时候，必须同时看见那颗 AP
在置位之前做完的所有事——per-CPU 区域、idle 线程、调度器被标成 active。
x86 的 store 是 TSO，硬件那层碰巧对，但编译器重排这一层没有任何约束。

**3. `set()` 是读-改-写。** 两颗 CPU 同时翻不同的位，各自会存回一个丢了对方那一位的字。
今天不会发生（BSP 一次只唤醒一颗），但"因为调用方碰巧是串行的所以安全"正是这轮多核修复
一路踩过来的东西，而并行唤醒已经在待修清单上了。旁边的
`Scheduler::set_state()` 早就用 CAS 循环做对了同一件事，注释里写着同样的理由。

**为什么不是三个掩码一起改**
三个状态的生命周期根本不同：

| 状态 | 谁写 | 什么时候 |
|------|------|---------|
| `Possible` | BSP | 解析 MADT 时，之后不再变 |
| `Present`  | BSP | 同上 |
| `Online`   | **每颗 AP 自己** | 引导之后持续变 |

只有 `Online` 是启动后还在变、而且被别的 CPU 写的。把三个一起塞进
`std::array<std::atomic<CpuMask>, 3>` 会连带毁掉两个正当的按引用用法——
`srat.cpp` 就地收窄 possible 集合、`ClockEventDevice` 拿 `cpu_possible_mask()`
的指针当"任意 CPU"——去换两个根本不需要的原子。把它们当成一个齐整的数组，
本身就是这个 race 能存在的原因之一。

**改法**
```cpp
// 之前
static auto is_online(CpuNum cpu) -> bool {
    return state_mask(State::Online).test(cpu);
}
static auto set_state(CpuNum cpu, State state, bool enabled = true) -> void {
    state_mask(state).set(cpu, enabled);
}
static auto online_mask() -> CpuMask & { return state_mask(State::Online); }
static std::array<CpuMask, NUM_STATES> STATES;
```
```cpp
// 之后
static auto is_online(CpuNum cpu) -> bool {
    return ONLINE.load(std::memory_order_acquire).test(cpu);
}

static auto set_state(CpuNum cpu, State state, bool enabled = true) -> void {
    if (state != State::Online) {
        immutable_mask(state).set(cpu, enabled);
        return;
    }
    auto expected = ONLINE.load(std::memory_order_relaxed);
    for (;;) {
        auto desired = expected;
        desired.set(cpu, enabled);
        if (ONLINE.compare_exchange_weak(expected, desired, std::memory_order_release,
                                         std::memory_order_relaxed)) {
            return;
        }
    }
}

/// 快照
static auto online_mask() -> CpuMask { return ONLINE.load(std::memory_order_acquire); }
/// 需要「持续跟随」的那一个调用者用这个
static auto online_mask_live() -> std::atomic<CpuMask> const & { return ONLINE; }

static std::array<CpuMask, NUM_STATES> STATES;   // 只住 Possible / Present
static std::atomic<CpuMask> ONLINE;
```

**`CpuAffinity` 为什么跟着动**
`online_mask()` 以前返回引用，是因为有一个调用者**必须**跟随它：

```cpp
// scheduler.cpp，on_thread_init()
context.m_affinity.set_affinity_hard_shared(Cpu::online_mask());
```

一个"哪儿在线就能在哪儿跑"的线程，必须随着次级 CPU 上线而变宽；拷一份就会被冻结在
创建时刻——引导路径上就是"永远只有 BSP"。这个设计是对的，要留。

留法是让 `CpuAffinity` 持的是**原子单元**而不是裸引用：

```cpp
std::atomic<CpuMask> m_owned_soft;
std::atomic<CpuMask> m_owned_hard;
std::atomic<CpuMask> const *m_soft_affinity;
std::atomic<CpuMask> const *m_hard_affinity;
```

两个指针要么指向自己这两个（拷贝形式），要么指向 `Cpu::ONLINE`（跟随形式），
所以两边必须同型。自有的那两个其实从不竞争（affinity 在线程的 PI 锁下设置），
只是付一样的写法。`get_available_mask()` 里两次 load 用 relaxed——
它只是收窄候选集，中途上线的 CPU 不过是这一轮没被提供，
affinity 掩码上没有发布任何别的数据，没有需要 acquire 去建立的顺序。

**参照**
- Linux `cpu_online_mask` 是 `struct cpumask` 上的位操作（`set_cpu_online()` 用
  `cpumask_set_cpu()`，底下是 `set_bit()`，本身就是原子的），读用 `cpu_online()`；
  `p->cpus_ptr` 指向 `cpu_possible_mask` 这类内核生命期掩码的做法，正是这里
  `set_affinity_hard_shared()` 的原型。
- 同仓库的 `Scheduler::set_state()`：同样的 CAS 循环，注释里同样的理由。

**验证**
- 四核引导（`kernel.dev.dbg_1.obi`）：三颗 AP 依次 online，
  userboot 在 cpu1 进 ring 3，cpu0 8589 / cpu1 597 / cpu2 80594 / cpu3 80600，
  零断言失败。
- 反汇编确认自旋里的读没有被提出去：

```
8bc90: movq -0x5a2f(%rip), %rcx   # <ours::Cpu::ONLINE>
8bc97: testq %rax, %rcx
8bc9a: je   0x8bc90               <- 跳回 load 本身，不是跳过它
```

- 加了 `static_assert(std::atomic<CpuMask>::is_always_lock_free)`：本内核没有
  libatomic，`MP_MAX_CPUS` 大到一个 64 位字装不下时会变成链接错误而不是变慢，
  把这条限制写在明面上。

**未实测的部分**：这份验证做完之后，工作树被另一个会话在做的
`VmAspaceKind` 重构弄成编译不过的状态，所以最终形态没能再整树构建一次。
与已验证版本逐字比对过，差异只有一段注释。

**故意没做**
- `Possible` / `Present` 仍然是普通掩码，仍然按引用交出去。它们启动后不变。
- `srat.cpp:98` 的 `cpu_possible_mask().set(count)` 是穿过引用就地写，没动。
  它能工作，但"返回可写引用"这个形状本身值得收掉，另记。
- 三套"哪些 CPU 存在／起来了"的登记（`Cpu::STATES`、`CpuLocal::STATES`、
  `Scheduler::STATES`）没有合并，仍记在 `docs/repairs/260909/task-sched.md`。
