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

---

## 3. `CpuAffinity` 自己持有掩码；`set_cpu_affinity_hard(CpuMask::of(n))` 不再悬垂（2026-09-03）

**文件**
- `kernel/include/ours/task/sched/context.hpp` — `CpuAffinity` 内嵌两份 owned 掩码，
  复制型 setter 与 `_shared` 跟踪型 setter 分开；拷贝／移动 delete。
- `kernel/include/ours/task/sched/scheduler.hpp` / `sched/scheduler.cpp` —
  `Scheduler::set_cpu_affinity()` 改收 `CpuMask const &`、返回 `void`；
  `on_thread_init()` 两个分支分别用 `_shared` 和复制。
- `kernel/include/ours/task/thread.hpp` / `kernel/kernel/task/thread.cpp` —
  `Thread::set_cpu_affinity_{hard,soft}()` 改为传引用；文档说明会复制。
- `kernel/include/ours/cpu.hpp` — 删掉第 2 条加的 `cpu_mask_of()`。

**症状**

潜伏，随时会炸。三个调用点都把临时量的地址存进了线程：

```cpp
laborer->set_cpu_affinity_hard(CpuMask::of(0));                 // kernel/top/main.cpp:132
m_thread->set_cpu_affinity_hard(CpuMask::of(cpu));              // kernel/kernel/task/dpc.cpp:24
Thread::Current::get()->set_cpu_affinity_hard(CpuMask::of(dest)); // thread.cpp migrate_to()
```

`CpuMask::of()` 按值返回；绑定到 `CpuMask const &` 形参只把临时量的寿命延到这条语句结束。
`Scheduler::set_cpu_affinity()` 拿 `&affinity_mask` 换进 `m_hard_affinity`，语句一结束，
线程里就是一个指向已经死掉的栈对象的指针。之后每一次 `get_available_mask()` 都在读那块
栈上现在放着的随便什么东西。

`main.cpp:132` 就在第 1 条修好的 spawn 之后一行，是启动路径下一个必到之处。

**原因**

`set_cpu_affinity_hard(CpuMask const &)` 这个签名什么都没说。它看起来像所有收 `const &`
的函数一样「用完即弃」，实际却把地址留了下来，要求实参活得比线程还久。三个调用点、
三个不同的人写的、全错——这不是三次巧合，是接口在撒谎。

前一版的注释（第 2 条里的 `cpu_mask_of()`）就是在这个约束下打的补丁：既然指针非拥有，
那就提供一张静态表让人取地址。这是在替接口的缺陷找活路，不是修它。

但**只**改成持有也不对。`on_thread_init()` 给普通线程装的是 `Cpu::online_mask()`——
一个还在变的活掩码：启动时只有 BSP online，次级 CPU 陆续上来。复制它等于把线程冻结在
「创建那一刻已 online 的 CPU」上；启动路径上创建的每个线程都会被永远钉在 BSP。
指针存在的合法理由就是这个：**跟踪**。所以两种语义都得有，而默认名字必须给安全的那种。

**改法**

照 Linux `task_struct` 的形状：`cpumask_t cpus_mask` 内嵌，`cpumask_t *cpus_ptr` 平时指向它。

```cpp
CpuMask m_owned_soft, m_owned_hard;        // 复制型 setter 的落脚点
CpuMask const *m_soft_affinity, *m_hard_affinity;   // 平时指向上面，shared 时指向外面
```

- `set_affinity_hard(mask)`：复制进 `m_owned_hard`，指针指回自己。**任何**掩码都安全，
  临时量也安全。这是「什么都不想」的默认。
- `set_affinity_hard_shared(mask)`：只存指针。名字里带 `shared` 就是要让写的人停一下：
  `mask` 必须活得比线程久。目前唯一合法用户是 `on_thread_init()` 装 `Cpu::online_mask()`。
- 拷贝／移动构造与赋值 delete：指针指向自身，拷贝会让副本指着原件的掩码。现在没人拷贝
  （它随 `Thread` 原地构造），delete 是把将来可能出现的静默 aliasing 变成编译错误。
- `Scheduler::set_cpu_affinity()`：原来收 `CpuMask const *`、swap 进去、把换下来的旧指针
  **返回**。两件事都要随所有权一起走——存调用方地址就是三处悬垂的来源；返回旧指针现在会
  返回线程内部 `m_owned_hard` 的地址，让调用方在线程改了之后还攥着它。没有任何调用方读过
  返回值。改成收引用、返回 `void`、内部走复制型 setter。它不提供 shared 变体：
  有资格 shared 的只有内核自己的掩码，装它们的只有 `on_thread_init()`，那里直接碰 `m_affinity`。
- `on_thread_init()`：普通线程 `set_affinity_hard_shared(Cpu::online_mask())`（要跟踪）；
  钉死的线程 `set_affinity_hard(CpuMask::of(target_cpu))`（一颗 CPU 没什么可跟踪的，复制
  正好让一个在调用点现造的掩码可以安全装入）。
- 删掉 `cpu_mask_of()`：它存在的唯一理由「非拥有指针要求实参有静态存储期」已经不成立。
  留着一个理由已死的函数，比删掉更糟——它的注释会继续对读者撒谎。
- 三个悬垂点**一行未改**，自动变对。这正是修接口而不是修调用点的意义。

否掉的方案：只把三处改成 `cpu_mask_of(n)`。能止血，但下一个写 `set_cpu_affinity_hard(...)`
的人还是会踩，因为签名还是那副样子。

**参照**

`Linux task_struct`：`cpumask_t *cpus_ptr` 旁边放一份 owned `cpumask_t cpus_mask`，
`cpus_ptr` 平时 `== &cpus_mask`，只在 `migrate_disable()` 之类临时覆盖时指向别处。
`set_cpus_allowed_common()` 是 `cpumask_copy(&p->cpus_mask, ...)`——复制，不存调用方的指针。

**验证**

- 构建通过；`clang-format` 已跑；`clang-tidy` 对 `scheduler.cpp` 与 `thread.cpp` 跑过，
  唯一新告警是 `get_available_mask` 改 const 后要 `[[nodiscard]]`，已加，其余都是既有条目。
- 每线程多 2 × `sizeof(CpuMask)` = 16 字节（`MP_MAX_CPUS=16`，一个字一个掩码）。
- **QEMU 未实测**，原因同 `task.md` 第 1 条：mm 在途的 `verify<Self>()` 改动让启动停在
  `init_vmm()`。这条改动的正确性论证是静态的（所有权语义 + 编译期 delete），
  等 mm 修好要回来实测 `main.cpp:132` 之后的路径。

**故意没做**

- 没有把 `on_thread_init()` 的默认亲和性从跟踪 `online_mask()` 改成 Linux 的
  `cpu_possible_mask` 快照。那是语义改动，先保持原样。
- 没有给 `Scheduler::set_cpu_affinity()` 加 shared 变体，理由见改法。

---

## 4. `do_unpark_thread()` 只认 Parked/Sleeping，`resume()` 一到就断言（2026-09-03）

**文件**
- `kernel/kernel/task/sched/scheduler.cpp` — `do_unpark_thread()` 的来源状态断言。

**症状**

`start_kernel()` 里 `laborer->resume()` 一调就停（dbg 镜像）：

```
Assertion failed on thread->state() & (Thread::State::Parked | Thread::State::Sleeping)
```

**原因**

`Thread::resume()` 对 `State::Initial`（从没跑过）和 `State::Suspend` 两种线程都走
`Scheduler::unpark_thread()`；`Thread::suspend_or_kill()` 对 `Suspend` 态执行 Kill 也走它。
而 `do_unpark_thread()` 的断言只写了等待队列会产生的两种：`Parked | Sleeping`。

```cpp
DEBUG_ASSERT(thread->state() & (Thread::State::Parked | Thread::State::Sleeping));
```

`UnparkControl::Initial` 这个枚举值就是为"第一次 resume"设的，说明设计时知道这条路，
断言却没跟上。每一个 spawn 出来的线程第一次 `resume()` 都会撞它，
`laborer` 只是启动路径上第一个。从 `Suspend` 恢复也一样撞。

**改法**

断言改成"能被唤醒的状态集合"：`Initial | Suspend | Parked | Sleeping`。这四个正好
是 `unpark_thread()` 全部调用方各自的来源态。

**参照**

`Zircon Thread::Resume()`：`state() == THREAD_INITIAL || state() == THREAD_SUSPENDED`
都进 `Scheduler::Unblock()`；`Scheduler::UnblockCommon()` 对来源态只断言 `!thread->IsIdle()`，
然后 `thread->set_ready()`——被唤醒这件事本身就把线程置成 READY，不管它从哪个非运行态来。

**验证**

- 构建通过，`clang-format` 无改动，`clang-tidy` 无新告警。
- QEMU 两个镜像：断言消失，都多走一步，停在同一个 #DE——`Fair::enqueue_thread()` 内联的
  `RunQueue::avg_vruntime()` 里 `total_vruntime / weight_sum`，`weight_sum == 0`
  （dbg `+0x214db` = `enqueue_thread+0x16b`，dev `+0x1cef3` = `enqueue_thread+0x153`）。
  两个镜像停在同一处，说明不再有被断言掩盖的差异。

**故意没做**

- 没有在这里动那个 #DE。它有两个独立的根：每个线程的权重都是 0（`BaseProfile(priority)`
  把 `priority` 扔了，`priority_to_weight()` 从没被调用），以及 `avg_vruntime()` 没有
  Linux `avg_vruntime()` 那句 `if (load)` 守卫。各自一条改动，下面接着写。

---

## 5. `BaseProfile(priority)` 把优先级扔了，所有线程的权重都是 0（2026-09-03）

**文件**
- `kernel/include/ours/task/sched/profiles.hpp` — `BaseProfile` 构造函数用 `priority_to_weight()`；
  三个权重字段改成表的类型 `Weight`。
- `kernel/include/ours/task/sched/common.hpp` — `priority_to_weight()` 的范围断言改成闭区间。
- `kernel/include/ours/task/sched/types.hpp` — 删掉再没有使用者的 `SchedWeight`（`i32`）别名。

**症状**

第 4 条修完之后两个镜像停在同一个 #DE：`Fair::enqueue_thread()` 内联的
`RunQueue::avg_vruntime()` 里 `total_vruntime / weight_sum`，`weight_sum == 0`。

**原因**

```cpp
constexpr explicit BaseProfile(i32 priority, bool inheritible = true)
    : weight(), discipline() {}
```

`priority` 形参没被用到；`weight()` 值初始化成 0。`common.hpp` 里 `priority_to_weight()`
连同 32 项的 `PRIORITY_TO_WEIGHT_MAP` 早就写好，但全树没有一个调用者。于是不管
`DEFAULT_PRIORITY`、`IDLE_PRIORITY` 还是别的，每个线程的权重都是 0——**0 不是"低权重"，
是"没有权重"**：线程对运行队列的 `weight_sum_` 贡献为零，`calc_weighted_time()` 里
`weight / MID_WEIGHT` 也恒为 0，虚拟时间根本不前进。

三个权重字段的类型是 `SchedWeight = i32`，而权重表是 `Weight = usize`，
`Context::weight()` 也返回 `Weight`——这个 i32/usize 的不一致本身就是"这条路从没接通"的
痕迹：接通了立刻会撞窄化告警。

`priority_to_weight()` 自己的范围断言是 `priority < HIGHEST_PRIORITY`，开区间，把名字
承诺合法的那个最高优先级（31，表的最后一项）拒之门外——没人撞到只是因为没人调它。

**改法**

- `BaseProfile(priority)` → `weight(priority_to_weight(static_cast<usize>(priority)))`。
  转换放在"从优先级造出 profile"的唯一入口，后面所有人拿到的都是权重。
- 三个权重字段 `SchedWeight` → `Weight`，和表、和 `Context::weight()` 一致；
  `SchedWeight` 别名随之没有使用者，删掉。
- `priority_to_weight()` 断言改成 `<= HIGHEST_PRIORITY`。

**参照**

`Zircon SchedulerState::BaseProfile(int priority, bool inheritable)`：
`fair{.weight{SchedulerState::ConvertPriorityToWeight(priority)}}`——同一个位置做同一个转换。

**验证**

- 构建通过；`clang-format` 已跑；`clang-tidy`（经 `thread.cpp` 这个 TU）对三个头无告警。
- QEMU 两个镜像：停点**不变**，仍是 `enqueue_thread` 里同一个 #DE。这是预期的：
  `enqueue_thread()` 先 `adjust_vlag()` 再 `rq.enqueue_object()`，所以第一个线程入队时
  `weight_sum_` 还是 0、`current_` 还是空——权重非零也救不了一个空队列上的除法。
  下一条加 Linux `avg_vruntime()` 那句 `if (load)` 守卫。
  本条改动的正确性是静态的（映射存在且被调用），停点不动不代表没起作用。

**故意没做**

- 没动 `calc_weighted_time()` 里 `weight / MID_WEIGHT` 的整数截断：对低于中权重的线程
  （`IDLE_PRIORITY` = 7 → 503/10550 = 0）结果仍是 0。这是另一个 bug，已记进待修。
- 没动 `inheritible`（拼写也不对）这个未使用的形参。

---

## 6. 启动阶段换用轮转（RR）纪律，Fair 暂时退出构建（2026-09-03）

**文件**
- `kernel/include/ours/task/sched/rr.hpp` / `kernel/kernel/task/sched/rr.cpp` — 新增 `RrRunQueue` 与
  `RoundRobin` 纪律，注册为 `Discipline::Normal`。
- `kernel/kernel/task/sched/BUILD.gn` — `fair.cpp` 换成 `rr.cpp`；`fair.cpp`/`fair.hpp` 一行未动。
- `kernel/include/ours/task/sched/context.hpp` — `Context` 加 `m_rr_hook`（list 钩子）与 `RrQueueOption`；
  `Context`/`Timeslice` 各加 `friend class RoundRobin`。
- `kernel/include/ours/task/sched/scheduler.hpp` — `Scheduler` 里在 `m_fair_runqueue` 旁加 `m_rr_runqueue`
  与 `rr_run_queue()`。
- `kernel/include/ours/task/sched/types.hpp` / `kernel/include/ours/task/thread.hpp` — 前向声明与
  `friend sched::RoundRobin`，和 Fair 同样的写法。

**症状**

Fair 的 `RunQueue::avg_vruntime()` 在第一个线程入队时除零（第 5 条的停点）。修它要动 Fair 的算术，
而 Fair 是作者自己的设计，明确不由助手改动。

**原因**

不是 bug 的原因，是决策的原因：启动路径后面还有一串通用 `Scheduler` 层的问题要走，每一步都需要
一个"不会算错"的纪律来推进。RR 没有权重、没有 lag、没有 deadline——只有一条 FIFO 和一个固定
时间片，出问题时能确定是通用层的，不是纪律的。

**改法**

- `RrRunQueue`：侵入式 FIFO（`Context::m_rr_hook`，和 Fair 的 set 钩子不同形，一个 Context 同一时间
  只在一个纪律的队列上）。放在 `Scheduler` 里，理由与 Fair 的队列相同——`Scheduler` 已经是 per-CPU 单例。
- `RoundRobin`：只实现通用 `Scheduler` 真正驱动的那些入口。
  - `enqueue/dequeue`：正在运行的线程不在 FIFO 上（`set_next_thread` 把它摘下来），所以对它的
    enqueue 是 no-op、dequeue 只记账。
  - `put_prev_thread`：还可运行就先记账、再排到队尾——这就是"轮转"。
  - `set_next_thread`：摘出队列，`prev_runtime` 作为本片基线，`curr_started_time` 作为计时起点。
  - `update_thread`：每次时钟更新给当前线程记账；片用完且队里还有人就 `mark_preempt_pending()`。
    只有自己一个可运行线程时不抢占——没有人可轮换。
  - `on_thread_init`：把 `m_timeslice.m_base` 设成 `runtime.scheduling_period()`，否则
    `Timeslice::length()` 是 0，第一拍就"用完"。
  - `curr_started_time` 由纪律维护——通用层从不写它，Fair 之前也是自己维护。
- 记账辅助函数做成 `RoundRobin` 的私有静态成员，而不是文件级函数：`Context`/`Thread` 给
  `RoundRobin` 的 friend 罩不到自由函数。
- 末尾显式实例化 `template class Schedule<RoundRobin, Discipline::Normal>;`——和 fair.cpp 末尾
  那段一样，不写的话 `REGISTRATION` 不会被实例化，链接器收不到，`Scheduler::init()` 找不到纪律。
- 换回 Fair 的方法：`BUILD.gn` 里 `rr.cpp` → `fair.cpp`。两个队列槽同时存在，切换只是注册谁的问题。

**参照**

无，本项目自有设计（Zircon 只有一个调度器；Linux 的 `SCHED_RR` 在 `kernel/sched/rt.c`，
思路相同：`requeue_task_rt()` 在 `task_tick_rt()` 片用完时把任务移到队尾）。

**验证**

- 构建通过；`clang-format` 已跑；`clang-tidy` 对 `rr.hpp`/`rr.cpp` 无告警。
- QEMU 两个镜像都越过了 Fair 的 #DE，进到了新的地方——而且是**通用层**：
  - dbg：`reschedule_mask()` 里 `current_thread->m_canary.verify<Self>()`，`Self` 是 `Scheduler`
    而 `current_thread` 是 `Thread*`——拿错类型的魔数校验 canary，必炸。
  - dev（断言关闭，走得更远）：`Thread::trampoline()` → `trampoline_handoff()` →
    `finish_switch_context()` 释放 `prev_thread` 的 pi 锁时 `assert_held()` 失败。
    这意味着 **RR 上的第一次真实上下文切换已经发生**：laborer 被 `evaluate_next_thread` 选中并切了过去。
    锁没被持有，是因为 `reschedule_mask()` 直接调 `Scheduler::Current::preempt()`，
    而不像 `flush_pending_local()` 那样走取 pi 锁的 `Thread::Current::preempt()`。
  两条都是通用 `Scheduler` 的笔误，各自一条改动。

**故意没做**

- 没有实现 `unpark_preempt`（唤醒不抢占，纯 FIFO）、`select_target_cpu`/`migrate_thread`
  （用 ISchedule 默认）、`override_thread_base_profile`（RR 没有权重可改）。
- 没有删 Fair 的任何东西，包括 `Scheduler::m_fair_runqueue` 和 `scheduler.hpp` 对 `fair.hpp` 的包含。

---

## 7. 三处拿 `Scheduler` 的魔数去校验 `Thread` 的 canary（2026-09-03）

**文件**
- `kernel/kernel/task/sched/scheduler.cpp` — `unpark_thread(UnparkList)`、`reschedule_mask()`、
  `Scheduler::Current::tick()` 三处 `verify<Self>()` → `verify<Thread>()`。

**症状**

RR 换上后 dbg 镜像在 `laborer->resume()` 一进 `reschedule_mask()` 就停：

```
Assertion failed on m_magic == gktl::magic<T>() with message: Error type magic
```

lldb 栈：`Canary::verify<ours::task::sched::Scheduler>` ← `reschedule_mask` ← `unpark_thread`
← `Thread::resume` ← `start_kernel`。

**原因**

`Scheduler` 的成员函数里 `Self` 是 `Scheduler`。三处对一个 `Thread *` 写了
`->m_canary.verify<Self>()`，等于拿 `magic<Scheduler>()` 去比 `Thread` 构造时种下的
`magic<Thread>()`，永远不等。它们之前没炸只是因为启动从没走到这里。

这和 mm 那边 `VmMapping` canary 的教训是同一类：canary 的种子和校验各自拼一次类型名，
拼错一次就是必炸的断言，而且断言文本只说"type magic 错了"，不说是哪个类型。

**改法**

三处改成 `verify<Thread>()`。`self.m_canary.verify<Self>()`（校验 Scheduler 自己）的那些不动，
它们是对的。

**参照**

无，本项目自有设计。Zircon 的 `fbl::Canary<magic>` 把魔数做成模板非类型参数，
校验时不需要再写一遍类型名，结构上避免了这一类错误——mm 的 `docs/changes/260903/mm.md`
对此有更完整的讨论。

**验证**

- 构建通过，`clang-format` 无改动，`clang-tidy` 无新告警。
- dbg 镜像：canary 断言消失，前进到 `Scheduler::Current::preempt()` 的
  `DEBUG_ASSERT(curr == self.m_runtime.curr_thread)`——`curr_thread` 从没人播种（下一条）。
  日志里同时出现 `IPI-Resched on CPU[0]`：`IpiService::send_resched(CpuMask)` 算好了
  `targets` 却发了原始 `mask`，给自己发了 IPI（待修里已有，单独一条）。
- dev 镜像：停点不变（`finish_switch_context()` 释放未持有的 pi 锁），预期如此——
  dev 没有 DEBUG_ASSERT，本来就没在 canary 处停。

**故意没做**

- 没有改成 Zircon 那种非类型模板参数的 canary。那是 gktl 的接口改动，不在这里做。

---

## 8. 调度器从没被告知"现在跑的是谁"：播种 `curr_thread` 与空闲线程（2026-09-03）

**文件**
- `kernel/include/ours/task/sched/scheduler.hpp` / `sched/scheduler.cpp` — 新增
  `Scheduler::init_first_thread(Thread *, CpuNum)`。
- `kernel/kernel/task/init.cpp` — `init_idle_task_percpu()` 建完 idler 后调用它。

**症状**

canary 修完后 dbg 停在：

```
Assertion failed on curr == self.m_runtime.curr_thread
```

（`Scheduler::Current::preempt()`；同一个断言在 `reschedule_common` 等六处。）

**原因**

`m_runtime.curr_thread` 全代码只有一处写入——`switch_context()`。`m_idle_thread` 一处都没有。
也就是说，一颗 CPU 在**第一次切换之前**正在执行的那个线程（BSP 上的 boot 上下文、`task::init()`
之后每颗 CPU 的 idler），调度器根本不知道它存在：`curr_thread` 一直是空，直到有人解引用它——
`unpark_preempt` 会、`Fair::enqueue_thread` 会（第 5 条里穿过零页读写的就是它）。
`m_idle_thread` 也一直是空，`evaluate_next_thread()` 无事可做时返回它，`put_prev_thread()` /
`set_next_thread()` 拿它判断 Idling 状态。

`Thread::Current::set(&idler.thread(), cpunum)` 告诉了 CpuLocal，却没告诉调度器；两边一不一致，
第一次重调度就在 `curr == curr_thread` 上撞。

**改法**

`Scheduler::init_first_thread(thread, cpunum)`，照 Zircon `InitializeFirstThread()` 的形状：
钉到本 CPU、记 `curr_cpu`/`last_cpu`、绑定纪律并让纪律 `on_thread_init`（正在运行的线程也是
纪律必须知道的线程）、然后做没有别的路径会做的那一步——直接写 `curr_thread`、启动它的片时钟
`curr_started_time`、若是 idler 则记为 `m_idle_thread`。`task::init()` 在每颗 CPU 的
`Thread::Current::set()` 之后立刻调用。

**参照**

`Zircon Scheduler::InitializeFirstThread(Thread*)`：`state_ = THREAD_RUNNING`、
`curr_cpu_ = last_cpu_ = current_cpu`、`hard_affinity_ = cpu_num_to_mask(current_cpu)`、
`scheduler->active_thread_ = thread`、`weight_total_ = ...`。Zircon 的 idle 线程不存指针，
从 `percpu::Get(cpu).idle_power_thread.thread()` 推导（`Scheduler::DequeueThread()` 末尾）。

**验证**

- 构建通过，`clang-format` 已跑，`clang-tidy` 无新告警（`init.cpp` 只剩既有的 `BOOT_THREAD` 命名条目）。
- dbg：`curr == curr_thread` 断言消失，前进到 `reschedule_common()` 的
  `DEBUG_ASSERT(arch::irqs_disabled(), "Enter {} with IRQ enabled")`（这条断言的消息在格式化时
  又抛了 libc++ `format_error`，日志里只看得到 `format_error ... "%s"`，靠 lldb 才读到正文）。
- dev：停点不变（`finish_switch_context()` 释放未持有的 pi 锁）。
- 两条新停点同根：`reschedule_mask()` 直接调 `Scheduler::Current::preempt()`，而**更深一层**，
  `lockdep::Guard<Lock, Options...>` 把 `IrqSave`/`PreemptDisable` 选项只存进 `std::tuple`
  从不使用——`guard()` 只做 `m_lock.acquire()`。所以内核里所有 `Guard<SpinLock, IrqSave>` 都只是
  裸自旋锁，`Thread::resume()` 的 `IrqSaveAndPreemptDisable` 也一样。那是 lockdep 的事，
  已转给 lib/concurrent 会话，并记进待修。

**故意没做**

- 没有把 `m_idle_thread` 改成像 Zircon 那样从 percpu 推导。现在它有了唯一的写入点，
  先够用；改成推导是后续清理。
- 没有给 BSP 的 boot 上下文（`create_first()`）播种——它在 `task::init()` 里被 idler 顶替，
  之后不再是任何调度器的 current。

---

## 9. `reschedule_mask()` 本地抢占不持 pi 锁，切换对面释放了一把没人拿的锁（2026-09-03）

**文件**
- `kernel/kernel/task/sched/scheduler.cpp` — `reschedule_mask()` 末尾改走 `Thread::Current::preempt()`。

**症状**

dev 镜像（断言关闭，走得最远）在 laborer 第一次被切到时停：

```
Assertion failed on m_lockval.load(MemoryOrder::Relaxed) == holder_state(...)
```

lldb 栈：`SpinLock::release` ← `assert_held` ← `finish_switch_context` ← `trampoline_handoff`
← `Thread::trampoline`。dbg 镜像则停在 `reschedule_common()` 的 `irqs_disabled()` 断言。

**原因**

切换协议把当前线程的 pi 锁**跨上下文切换传递**：出去的一侧拿着它进 `reschedule_common()`，
进来的一侧在 `finish_switch_context()` 里释放。`Thread::Current::preempt()` 和
`PreemptionState::flush_pending_local()` 都遵守——前者用 `Guard<SpinLock, IrqSave>` 取锁再进
调度器。`reschedule_mask()` 是唯一直接调 `Scheduler::Current::preempt(current_thread)` 的地方，
不取锁就进去了，于是对面释放的是一把没人持有的锁。

**改法**

`Thread::Current::preempt()`，和 `flush_pending_local()` 一致。

**参照**

`Zircon Scheduler::RescheduleMask()` → `Preempt()`，在 thread lock 持有下进行。

**验证**

- 构建、`clang-format`、`clang-tidy` 无新告警。
- dev：`assert_held` 断言消失，之后**静默挂住**——日志停在切换前最后一行。最像
  `IdleThread::run()`：`m_states` 永远 `current != target`，每圈在关中断、关抢占下
  `Thread::Current::reschedule()`，而 `Scheduler::Current::reschedule()` 见 `!is_preemptible`
  就 `add_pending(); return;`，死循环无输出。已记待修，是下一条。
- dbg：仍停在 `irqs_disabled()`。这条不归本改动：`lockdep::Guard<Lock, Options...>` 把
  `IrqSave`/`PreemptDisable` 存进 `std::tuple` 从不使用（`guard()` 只 `m_lock.acquire()`），
  所以 `Thread::Current::preempt()` 虽然拿了锁，中断并没有关。全树没有任何 `Guard` 路径构造
  `AutoInterruptDisabler`。已转 lib/concurrent 会话。

**故意没做**

- 没有在这里给 `Guard` 补 state saver——那是 lockdep 的接口，不是 task 的。

---

## 11. `set_next_thread()` 断言 `curr_thread == 0`，描述的是一个不存在的协议（2026-09-03）

**文件**
- `kernel/include/ours/task/sched/scheduler_inline.hpp` — 删掉 `set_next_thread()` 开头的
  `DEBUG_ASSERT(0 == m_runtime.curr_thread)`。

**症状**

lockdep 的 `Guard` 修好之后，dbg 镜像走到第一次真实切换：

```
Assertion failed on 0 == m_runtime.curr_thread
```

**原因**

这条断言描述的握手是：`put_prev_thread()` 把 `curr_thread` 清空，`set_next_thread()` 看到它是空的。
但 `curr_thread` 全代码只有 `switch_context()` 一个写入点，而且在 `set_next_thread()` **之后**
才跑；`put_prev_thread()` 根本不碰它。之前没炸只是因为 `curr_thread` 从来没被播种（第 8 条），
一直是 0——断言恰好成立。播种之后它必炸。Zircon 也是在切换处写 `active_thread_`
（`scheduler.cc:2113`），不在挑选处。

**改法**

删掉断言，注释说明为什么这里对 `curr_thread` 没有前置条件。

**参照**

`Zircon Scheduler::RescheduleCommon()`：`active_thread_ = next_thread` 在切换处。

**验证**

- 构建通过。dbg：断言消失，laborer 被切上 CPU 并跑进 `init_arch()`（日志出现 Bootstrap
  PGD/GDT 等 arch 初始化 trace），随后在 `init_platform()` → `x86_bring_aps()` →
  `sleep_for()` 处撞下一条（第 12 条）。

---

## 12. `sleep_etc()` 进 `reschedule_common()` 时没有关抢占（2026-09-03）

**文件**
- `kernel/kernel/task/thread.cpp` — `Thread::Current::sleep_etc()` 的 pi 锁 guard 改用
  `lockdep::IrqSaveAndPreemptDisable`。

**症状**

第 11 条之后 dbg 停在 `x86_bring_aps()` 的第一次 `sleep_for(10ms)`：

```
Assertion failed on curr->preemption_state().preemption_disabled_count() != 0
with message: Enter reschedule_common() with preempt enabled
```

（消息在格式化时又抛 `format_error`，正文靠 lldb。）栈：`reschedule_common` ← `sleep_etc`
← `sleep_for` ← `x86_bring_aps` ← `platform_init_mp` ← `labour_routine`。

**原因**

`sleep_etc()` 用 `Guard<SpinLock, IrqSave>` 拿 pi 锁，然后经 `WaiterState::sleep()` 一路带着
这把锁进 `reschedule_common()`；后者要求并断言"抢占已关、中断已关、持 pi 锁"。在 lockdep 的
`Guard` 修好之前这些选项什么都不做，`IrqSave` 和 `IrqSaveAndPreemptDisable` 没有区别；
修好之后区别就是这条断言。`Thread::resume()` 用的本来就是组合选项。

**改法**

guard 改成 `lockdep::IrqSaveAndPreemptDisable`。Zircon `Thread::Current::SleepEtc()` 用
`PreemptDisableAndIrqSaveOption` 进 park。

**验证**

- 构建通过。dbg：断言消失，laborer 进入 sleep 后日志静默（idler 应当接手并 `hlt`，10 ms 后
  定时器唤醒 laborer）。静默的原因正在用 gdbstub 追（idle 循环 / 定时器回调 / `reschedule_mask`
  断点），见下一条。

**故意没做**

- `Thread::Current::preempt()/reschedule()/yield()/yield_to()` 仍是 `IrqSave`；
  它们进 `reschedule_common()` 时会撞同一条断言。`reschedule()` 还有一层
  `is_preemptible()` 门（要求中断开着），在 guard 关中断之后永远进不去——那是同一族问题，
  等启动走到那里逐条处理，先不批量改。

---

## 13. `trampoline_handoff()` 先开中断、后放锁，第一个 tick 撞上还没放的 pi 锁（2026-09-03）

**文件**
- `kernel/kernel/task/sched/scheduler.cpp` — `trampoline_handoff()` 里 `local_irq_enable()` 挪到两把锁释放之后。

**症状**

第 12 条之后 dbg 静默；gdbstub 追到的是：

```
Assertion failed on !is_held()
SpinLock::acquire ← ScopedGuard{pi, rq} ← Scheduler::tick_thread ← TickSched::on_tick
← TickDevice::handle_periodic_event
```

`tick_thread()` 去拿当前线程（刚被切上来的 laborer）的 pi 锁，而这把锁本 CPU 已经持有。

**原因**

切换协议把两把 pi 锁跨切换传给新线程：前一个线程的（`finish_switch_context()` 释放）和
新线程自己的（唤醒它的人拿的，`trampoline_handoff()` 释放）。旧代码的顺序是
`local_irq_enable()` → `finish_switch_context()` → `release()`：中断一开，tick 就可能在
两次释放之前进来，`tick_thread()` 对同一个线程再取 pi 锁，`!is_held()` 断言。
在 lockdep `Guard` 不关中断的年代这里的中断本来就是开的，问题被别的东西盖住了。

**改法**

先放两把锁，再 `local_irq_enable()`。Zircon `thread_trampoline`/`Scheduler::TrampolineHandoff`
同样是 LockHandoff 在前、`arch_enable_ints()` 在后。

**验证**

- 构建通过，`clang-tidy` 无新告警。
- dbg：`!is_held()` 断言消失；启动仍在 `x86_bring_aps()` 的 10 ms `sleep_for()` 之后静默——
  `bring_aps` 只睡一次就该继续，所以是唤醒链（tick → 定时器回调 → `unsleep` → `unpark` →
  切回 laborer）里还有问题，gdbstub 正在追（下一条）。


---

## 14. `evaluate_next_thread()` 对未注册纪律的空槽做虚调用，CPU 静默消失（2026-09-03）

**文件**
- `kernel/kernel/task/sched/scheduler.cpp` — `evaluate_next_thread()` 跳过 `m_schedules` 里的空槽。

**症状**
laborer 第一次 `sleep_for()` 进入 `reschedule_common()` 后静默挂住：探针显示走到了
`evaluate_next_thread()` 入口，之后什么都没有；没有断言、没有故障。

**原因**
`m_schedules` 按 `Discipline` 索引，没注册的纪律（今天的 Deadline）槽是空指针。循环对每个槽
无条件 `schedule->evaluate_next_thread()`：空指针的 vtable 从地址 0 读出来（启动阶段零页是映射的），
跳进零页——不缺页、不报错。之前活着只是因为第一个槽（RR）总能挑出线程提前返回；
第一次"没活可干"就走到了空槽。

**改法**
`if (!schedule) continue;`。

**参照**
无，本项目自有设计（Zircon 只有一个调度器，没有按纪律索引的表）。

**验证**
构建通过；dbg：随后出现 `switch laborer -> Idle/0`，切回 idler 成功（探针，已剥除）。


---

## 15. 切换前没有替 `next` 取 pi 锁；`reschedule_mask()` 的本地抢占改为只记 pending（2026-09-03）

**文件**
- `kernel/kernel/task/sched/scheduler.cpp` — `switch_context()` 在切换前 `next->get_pi_lock().acquire()`；
  `reschedule_mask()` 本地 CPU 只 `add_pending()`。

**症状**
切回 idler 之后：`Assertion failed on m_lockval.load(...) == holder_state(...)`——
idler 沿着自己的栈往上返回时，`Thread::Current::preempt()` 的 guard 释放 idler 的 pi 锁，
而这把锁已经被 laborer 那边的 `trampoline_handoff()`（作为 `prev`）释放过了。

**原因**
切换协议是"进来的一侧释放 `prev` 的锁，继续持有自己的锁往上返回"（Zircon `RescheduleCommon`
的 `next_thread->get_lock().AssertHeld()` 与 `LockHandoffInternal()`）。这要求**出去的一侧在切换前
先持有 `next` 的锁**——ours 没有这一步，于是每个被切换回来的线程都在释放一把没人持有的锁。
配套地，`Thread::resume()` 持着被唤醒线程的 pi 锁调 `unpark_thread()` → `reschedule_mask()`，
若在那里同步抢占，`switch_context()` 会对同一把锁再取一次；而 tick 的定时器回调也会从中断
上下文到这里，同步切换会把中断处理到一半的帧留在栈上。Zircon `PreemptLocked()` 持自旋锁时、
`RescheduleMask()` 在中断里都只记 pending。

**改法**
`switch_context()` 切换前取 `next` 的 pi 锁；`reschedule_mask()` 对本地 CPU 只 `add_pending()`，
由 `enable_preemption()` 的 flush 和空闲循环兑现。

**验证**
构建通过；dbg：该断言消失，idler 恢复后正常返回到 `become_idle()`。


---

## 16. "可抢占"从来不成立：标志位混进了判定，boot 线程也没关抢占（2026-09-03）

**文件**
- `kernel/include/ours/task/preemption_state.hpp` — `is_preemption_enabled()` 只看四个计数和时间片延长标志。
- `kernel/kernel/task/thread.cpp` — `preempt/reschedule/yield/yield_to` 的 guard 改 `IrqSaveAndPreemptDisable`；
  `reschedule()` 的可抢占判定挪到 guard 之前；`create_first()` 给 boot 线程关抢占。
- `kernel/kernel/task/sched/scheduler.cpp` — `Scheduler::Current::reschedule()` 去掉 guard 之后的判定，
  删掉 `is_preemptible()`。

**症状**
三步递进：(1) 空闲循环无限打印"runnable=1 -> reschedule"却从不切换；(2) `become_idle()` 的
`enable_preemption()` 从不 flush pending；(3) 修完 (1)(2) 后 `time::init()` 里一次 flush 抢占进了
还不存在的调度器，撞 canary。

**原因**
(1) `Thread::Current::reschedule()` 的 guard 先关中断，`Scheduler::Current::reschedule()` 再用
`is_preemptible()`（要求中断开着）做门——门永远关着。
(2) `is_preemption_enabled()` 是 `!raw_value()`，照抄 Zircon `PreemptIsEnabled()` 的 `state_ == 0`，
但 ours 的字里还塞了 `NoReschedPreempt`/`NoReschedEager` 两个**构造时为 true** 的标志——
每个线程从出生起就"抢占未开"。这两个标志记录的是"有没有请求挂起"，不是"允不允许抢占"。
(3) Zircon `thread_construct_first()` 对 boot 线程 `PreemptDisable()`，直到本 CPU 的 idle 线程就绪
才 `PreemptReenable()`（thread.cc:175 / 2028）；ours 的 `create_first()` 没做，(2) 修好后 flush 立刻发生。

**改法**
判定放在 guard 之前（不可抢占的上下文只记 pending）；guard 用 `IrqSaveAndPreemptDisable`
（`reschedule_common()` 断言的正是这个状态）；`is_preemption_enabled()` 只看计数；
`create_first()` 关抢占，`task::init()` 装上的 idler 自带一份计数由 `become_idle()` 释放。

**参照**
`Zircon Thread::Current::Reschedule()`/`Preempt()`、`PreemptionState::PreemptIsEnabled()`、
`thread_construct_first()`。

**验证**
构建通过；dbg：空闲循环切到 laborer，`become_idle()` 的 flush 生效，canary 断言消失。


---

## 17. `evaluate_next_thread()` 的"继续跑 curr"分支没写 `next`，`set_next_thread(nullptr)`（2026-09-03）

**文件**
- `kernel/kernel/task/sched/scheduler.cpp` — 该分支改为 `next = curr; return next;`。

**症状**
idler 恢复后 flush 一个 pending 抢占，无其它可运行线程，探针显示 `set_next null` 后静默。

**原因**
`evaluate_next_thread()` 用 `gktl::defer` 在作用域退出时 `put_prev_thread(curr); set_next_thread(next);`，
`next` 按引用捕获。其它分支都先写 `next` 再返回，唯独 `return curr;` 没写，`next` 仍是空，
`set_next_thread(nullptr)` 对空线程 `set_state_running()`。

**改法**
统一经 `next` 返回。

**验证**
构建通过；dbg：定时器到期 → `unsleep` → 切回 laborer → `sleep_etc` 返回，laborer 走进
`reclaim_init_area()`。

---

## 10. 空闲循环是一个关中断的静默死循环（2026-09-03）

**文件**
- `kernel/kernel/task/idle_thread.cpp` — `IdleThread::run()` 重写。
- `kernel/arch/x86/kernel/task/idle_thread.cpp` — `ArchIdleThread::idle()` 从空函数改成 `sti; hlt`。

**症状**

第 9 条之后 dev 镜像在第一次上下文切换后静默挂住：没有断言、没有故障，日志停在切换前
最后一行，QEMU 一直跑到被杀。

**原因**

三件事叠在一起：

1. `m_states` 构造成 `{current = Booting, target = Running}`，之后**没有任何写入**，
   于是 `current != target` 永远为真，旧循环每一圈都走"要重调度"的分支。
2. 那一圈是在 `AutoInterruptDisabler` + `AutoPreemptDisabler` 之下调 `Thread::Current::reschedule()`
   的，而 `Scheduler::Current::reschedule()` 对 `!is_preemptible(curr)`（抢占关**或**中断关）
   的回答是 `add_pending(); return;`——什么都不做。
3. `ArchIdleThread::idle()` 是空函数，没有 `hlt`；就算走到 idle 分支也是空转。

结果：idler 一旦运行，就在关中断下无限循环。中断关着，tick 进不来，谁也抢占不了它。
这正是切换回 idler 之后的"什么都没发生"。

**改法**

照 Zircon `IdlePowerThread::Run()` 的形状：

- 一进 `run()` 就把状态机置成 `{Running, Running}`——Zircon 的 `state_` 初值就是 `kActive`。
- 每圈先在**关中断**下判断有没有活（`Scheduler::num_runnable()`）：没有就 `m_arch.idle()`。
  关中断下判断是为了让"判断"和"halt"之间不会漏掉一个唤醒——`idle()` 用 `sti; hlt`
  把开中断和 halt 做成原子（`sti` 在下一条指令之后才生效，所以中断要么在 halt 中被响应，
  要么根本进不到判断和 halt 之间）。Zircon 的 `x86_enable_ints_and_hlt()` 是同一对指令，
  `ArchIdlePowerThread::EnterIdleState()` 的注释把这个竞争讲得很清楚。
- 有活就在**开中断、开抢占**下 `Thread::Current::reschedule()`，让它真的切出去。

**参照**

`Zircon IdlePowerThread::Run()`（`kernel/kernel/idle_power_thread.cc`）与
`ArchIdlePowerThread::EnterIdleState()` / `x86_enable_ints_and_hlt()`（`arch/x86/mp.cc`）。

**验证**

- 构建通过，`clang-format` 已跑，`clang-tidy` 无新告警。
- dev 镜像：日志与改动前一样停在"keeps tick device"——但这是**预期**的：laborer 后面只有
  `log::debug`（两个 gibberish 线程），而 dev 镜像 `KERNEL_DEBUG_LEVEL=0`，`logz4::debug()`
  被 `if constexpr` 编掉了。所以 dev 上"成功"和"挂住"看起来一样，只能靠 gdbstub 区分：
  在 dbg 镜像上用临时探针（已剥除）观察到：`idle: hlt` 两次、`timer expired`、`unsleep`、
  `switch Idle/0 -> laborer`、`sleep_etc: woke`——即 idler 真的 halt 了，tick 唤醒了 laborer。
- dbg 镜像：仍被 lockdep `Guard` 不关中断的缺口挡在 `reschedule_common()` 的
  `irqs_disabled()` 断言（待修第 1 条），本条改动在 dbg 上不可观测。

**故意没做**

- 没有实现 Offline/Suspend 状态的转移，`m_states` 里那两个值目前没有使用者。
- 没有处理"判断无活之后、halt 之前抢占请求已挂起"的情况（Zircon 检查 `preempts_pending()`）：
  当前所有唤醒都经 IPI/tick 中断到达，`sti; hlt` 的原子性已经覆盖；等有非中断路径的
  挂起抢占再补。
