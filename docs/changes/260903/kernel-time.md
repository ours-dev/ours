# kernel/time — 改动记录

格式见 `docs/changes/TEMPLATE.md`。待修项在 `docs/repairs/260903/kernel-time.md`。
提交：`4fbc034 fix(time): make the tick reach the scheduler`（第 2–10 条），
`10cceaf` 之前的若干提交（第 1 条）。

## 起因与追溯

第 2 条起的工作出自一次对 `kernel/time` 的整体审查（2026-09-02），当时列了 22 条编号发现。
这张表是它们各自的归宿，编号保留是因为下面的条目和代码注释里都还在引用：

| 编号 | 内容 | 归宿 |
|---|---|---|
| A1 | `Timer` → `TimerScheduler` 断开，超时永不触发 | 第 7 条 |
| A2 | 周期处理只推 jiffies，不调调度器 | 第 6 条 |
| B3 | `program_event()` 的 now 是 epoch 0 | 第 4 条 |
| B4 | 无穷比较用了未特化的 `NumericLimits` | 第 4 条 |
| B5 | `entroll(self)` 默认参数 min > max | 第 4 条 |
| B6 | `rebind_device()` 丢 handler、affinity 设错设备、无选择规则 | 第 5 条 |
| B7 | `enroll()` 丢频率、通知错对象、选择逻辑失效 | 第 3 条 |
| B8 | `cancel()` 只置位，而 `Timer` 是栈对象 | 第 7 条 |
| C9 | `TimerQueue` 没有任何锁 | 第 7 条 |
| C10 | `Clock::update_time()` 读时改状态、会溢出 | 第 3 条 |
| C11 | `Default::read()` 为读一个字拿 IrqSave 自旋锁 | `x86-clock.md` 第 2 条 |
| C12 | `HpetClockSource::m_cache` 不是原子的 | **未修**，`repairs/260903/x86-clock.md` |
| C13 | `GCED_MUTEX` 是睡眠锁却配 `IrqSave` | **未修**，`repairs/260903/kernel-time.md` |
| D14 | 四个类的 canary 是空构造，字段未初始化 | 第 2 条 |
| D15 | `NOOP_DEVICE` 在 `INIT_DATA`，且 vtable 指针为空 | 第 2 条 |
| E16 | HPET 写晚了的比较器永远不触发 | `x86-clock.md` 第 1 条 |
| E17 | PIT 的 latch 是 0，即 18.2 Hz | `x86-clock.md` 第 1 条 |
| E18 | LAPIC 的除数编码索引错 | `x86-clock.md` 第 1 条 |
| E19 | `arch_time_init()` 的中断号不一致 | **审查判错**，代码是对的，见 `x86-clock.md` 第 1 条 |
| F20 | `Deadline` 隐式构造、默认值是"已超时" | 第 8 条（其中"`join` 本意是无限等"一条判错） |
| F21 | `Ticks` 应改成有符号 | **判错并否决**，第 8 条说明理由 |
| F22 | 命名杂项 | 第 8 条 |

---

## 1. `Clock` 的构造函数解引用空指针（2026-09-02）

**文件**
- `kernel/kernel/time/clock.cpp` — 构造函数不再安装时钟源。

**症状**
潜伏。没炸是因为低地址在 `x86_init_mmu_early()` 把 `pgd[0] = pgd[511]` 之后通向 physmap，
读得到垃圾而不缺页。

**原因**
构造函数装 `ClockSource::Default::get()`，它返回 `*CLKEVT`——一个只有
`ClockSource::Default::init()` 才会填的指针，而那要等到 `time::init()`。
`SystemClock::CLOCK` 和 `SteadyClock::CLOCK` 都是 `static inline`，构造函数跑在
`ccrt::init_global_static_objects()` 里，是 `start_kernel` 的第一件事，比 `time::init()`
早十几级。所以那里的 source 恒为空，`m_cycle_last = source->read()` 是一次穿过空 vtable 的调用。

**改法**
从"没有 source"开始。`ticks()` 本来就对空 source 返回 0，`change_source()` 本来就把第一个
真实 source 走 `override_source()`。构造函数只给 `m_canary` 上正确的 magic（它原来是 0，
一旦断言打开 `verify<Self>()` 就是下一个要炸的地方），其余字段显式清零。

比例保持 0 而不是 1/1：tick→ns 的比例是 clock source 的属性，在它到来之前假装 tick 就是 ns
是个错误答案，而 0 会在断言上变成可诊断的。

**参照** 无，这是本项目自身的初始化顺序问题。

**验证** 构建通过。当时还走不到这段代码。

**故意没做** 没有让构造函数去等 source——那会把初始化顺序反过来。

---

## 2. 并掉 `TimerScheduler`，补齐 canary 与未初始化字段（2026-09-02）

**文件**
- 删除 `kernel/include/ours/time/timer_scheduler.hpp`、`kernel/kernel/time/timer_scheduler.cpp`
- `timer_queue.{hpp,cpp}` — `TimerQueue` 成为 per-CPU 对象，接管原 scheduler 的职责
- `timer.hpp`、`tick_sched.cpp`、`tick_device.cpp`、`clock_source.cpp` — canary 与字段初始化
- `init.cpp` — `time::init()` 从空函数变成绑定每个 CPU 的队列

**症状**
断言一开就炸：`TickDevice::program_event()`、`ClockSource::enroll()`、
`TimerQueue::run_expired_timers()` 第一次进入就在 `verify<Self>()` 上失败。
`TimerScheduler` / `TimerQueue` 没有构造函数，`cpunum_` / `m_running` / `m_clock_source` 全是垃圾。

**原因**
`gktl::Canary` 的空构造留下 magic 0，而 `verify<T>()` 比的是 `magic<T>()`。这批对象都用了
空构造。`NOOP_DEVICE` 还多一个问题：**它的 vtable 指针本身是空的**，而 `rebind_device()`
换设备时对旧设备调 `shutdown()`，`ClockEventDevice` 的每个 vtable 包装都是先 `m_vtable->slot`
再判空——第一次 `enroll`（PIT）就会穿过空 vtable 读。掩盖机制和第 1 条相同。

`TimerScheduler` 除了一个 `TimerQueue perclock_[]` 数组和两个空函数之外没有任何自己的状态。

**改法**
`TimerQueue` 直接成为 per-CPU 对象（继承 `mm::CpuLocal::Current<TimerQueue>`），
原来"每个 clock 一个 queue"变成它私有的 `ClockBase m_bases[]`——每个 base 是一个
`Set<Timer>` 加一个 offset。删掉的还有 `m_clock_source`（无人读写）；
`m_next_deadline` 提到队列一级，因为一颗 CPU 只有一个 tick 设备。

`TimerQueue::init(cpunum)` 由 `time::init()` 从 BSP 遍历 `cpu_possible_mask()` 对每个 unit
调一次。构造函数写不进 CPU 号：它只跑在模板拷贝上。不放到 AP 自己的启动路径，是因为 BSP 上
发起的 sleep 可能在 AP 起来之前就要挂到它的队列上。

`NOOP_DEVICE` 现在带一个全空槽位的 `constexpr Vtable`、放普通 `.data`、mask 用
`cpu_none_mask()`——它靠"无 feature、rating 0、空 mask"输掉每一个比较而被替换，
不靠调用点特判。

**参照**
`Linux hrtimer_cpu_base` 内嵌 `hrtimer_clock_base[]`；`Zircon TimerQueue` 是 per-CPU 对象、
上面没有任何东西。两个参照都不设 scheduler 层。

**验证** 构建通过，clang-tidy 改动行干净。

**故意没做**
`Timer::m_cancalled` → `m_cancelled` 只是拼写；同步语义留到第 7 条。

---

## 3. 频率贯通到 `Clock`，`now()` 改成纯函数（2026-09-02）

**文件**
- `clock_source.{hpp,cpp}` — 记住频率并算出 `mtl::Ratio`，暴露 `ticks_to_time()`；
  修正源选择；列表加 `TA_GUARDED`
- `clock.{hpp,cpp}` — `Clock` 重写成 seqlock 保护的仿射映射；新增 `SteadyClock::notify()`

**症状**
`SteadyClock::now()` 必然除零——全树没有任何地方给 `Clock` 的比例赋过值。
断言打开则停在 `update_time()` 的断言上。

**原因**
`enroll(self, hz)` 把 `hz` 扔掉。而且它只通知 `SystemClock`，`SteadyClock::CLOCK` 永远没有
source——内核用的全是 `SteadyClock::now()`。

`now()` 本身还有两个毛病：它每次都写 `m_time_shift` 和 `m_cycle_last`，中断里和线程里同时调
就撕裂；`cycles_delta * numerator_` 对 HPET 几秒就溢出 64 位。

**改法**
`enroll` 算出 `m_ticks_to_ns = 1e9 / hz`（`mtl::reduce` 约分后装进 `mtl::Ratio<u32>`；
HPET 的 1e9/14318180 约成 50000000/715909），`hz == 0` 或装不进 u32 的返回 `InvalidArguments`。

`Clock` 变成 `Anchor { source, ticks, time }` 三个字，整体在 `sync::SeqLock` 下快照：

```
now() = anchor.time + source->ticks_to_time(source->read() - anchor.ticks)
```

读路径不写任何东西。唯一的写者是换 source：`override_source()` 在 `write()` 里先用旧 anchor
算出此刻的时间，再钉到新 source 的当前计数上——时间在换源时连续。
删掉 `update_time()`、`m_cycle_last`、`m_time_shift`、`m_time_base`、`numerator_/denominator_`
和从没用过的 `AtomicU8 m_seq`。

没有 source 时 `now()` 返回 0 而不断言：页老化和调度时钟在 `time::init()` 之前就会读它，
"还没开始计数所以什么都没过去"是自洽的答案。

`enroll` 改为通知选出来的 `best`（不是 `self`），先通知 `SteadyClock` 再通知 `SystemClock`。
`select_best_source()` 原来的 for 循环第一次迭代必 `break`，`Unstable` 形同虚设；
现在按两条规则跳过 `Unstable` 和 oneshot 模式下没有 `HpClock` 的源。

**参照**
`Zircon current_mono_time()` 的纯函数形状。**没有**照 Linux 的 `(mult, shift)`：那是 32×64 的
乘积，只在 delta 有界时不溢出，而有界靠的是 Linux 每 tick 一次的 `timekeeping_update()` 推进
`cycle_last`——正是要去掉的读时写。纯函数下 delta 无界，需要 `mtl::scale` 的 96 位中间值。
源选择照 `Linux clocksource_select()`。

**验证** 构建通过，clang-tidy 干净。QEMU 实测见 `x86-clock.md` 第 3 条。

**故意没做**
`calculate_mult_shift()` 留着没动（仍无人调用）。32 位计数器的 mask 与周期重锚定没做——
`enroll()` 的注释写明了只支持 64 位计数器。

---

## 4. `program_event` 的 now、无穷比较、`enroll` 默认参数（2026-09-02）

**文件**
- `clock_event_device.{hpp,cpp}` — `program_event()` 的 now；删掉无参 `entroll`
- `tick_device.cpp` — `program_event()` 的三处
- `kernel/arch/x86/apic/local_apic.cpp` — LAPIC 改用校准频率入队

**症状**
每次编程都是最大间隔；队列空过一次之后设备再也编程不了；LAPIC 入队时断言必炸。

**原因**
1. `auto const now = TimePoint<>()` 是 epoch 0，`delta = expiry - 0` 是绝对时间，
   永远被钳到 `m_max_delta`。
2. `ustl::NumericLimits<TimePoint<>>::max()` 是未特化的 `std::numeric_limits`，主模板返回
   值初始化的 `TimePoint`，即 epoch——"无穷"分支对时间 0 触发，对真正的无穷永不触发。
3. 无穷时进 `Shutdown`，而 `program_event()` 对 `Shutdown` 返回 `BadState`。
4. periodic 模式下也去碰设备：periodic PIT 的"下一次事件"寄存器就是它的周期，
   periodic 的 APIC 定时器会重装最后写进去的计数。之前每次时间片重置都会写坏 PIT 的 latch。
5. `entroll(self)` 展开成 `(HZ, 128, HZ)`：`min 128 > max 100` 断言必炸；就算断言关着，
   `freq = 100` 会让设备以为一个 tick 是 10 ms。

**改法**
now 改成 `SteadyClock::now()`；过去的 deadline 按 `m_min_delta` 编程而不是拒绝（钳位自然覆盖
负 delta）——调用方是 `TimerQueue`，它没有重试循环，对已经晚了的 deadline 它要的就是"硬件能给
的最早一次中断"。无穷比较改成 `INFINITE_TIME`，处理改成 `OneshotStopped`。
periodic 模式下 `TickDevice::program_event()` 是空操作并返回 Ok。
删掉无参 `entroll`；LAPIC 用校准结果算频率，没校准就不入队。

**参照**
`Linux clockevents_program_event()` 的 `force` 路径；对 `KTIME_MAX` 的 `ONESHOT_STOPPED`；
只在 `hres_active` 时让 `hrtimer_reprogram()` 碰设备；
`clockevents_config_and_register()` 三个参数全必填。

**验证** 构建通过，clang-tidy 干净。

**故意没做**
LAPIC 在 TSC-deadline 模式下 tick 是 TSC 周期、频率不是这个数——当时写成 TODO，
在 `x86-clock.md` 第 4 条解掉。

---

## 5. `TickDevice` 的设备选择与换绑（2026-09-02）

**文件**
- `tick_device.{hpp,cpp}` — 选择规则、`rebind_device()` 重写、`setup_periodic/oneshot`
- `clock_event_device.{hpp,cpp}` — 新增 `Rating`、`set_cpumask()`、`has_feature()`；
  `switch_mode(Detached/Shutdown)` 对没有 shutdown 槽位的设备返回 Ok
- `i8253.cpp`（110）、`hpet.cpp`（120）、`local_apic.cpp`（100）— 各自的 rating 与 per-CPU mask
- `kernel/include/ours/cpu.hpp` — `cpu_none_mask()` / `cpu_all_mask()`（见 `platform.md`）

**症状**
LAPIC 只支持 oneshot，却被绑到 Periodic 模式的 `TickDevice` 上，
`switch_mode(Periodic)` 返回 `Unsupported` 被丢弃，设备停在 `Detached`。
Oneshot 模式下换设备会装进一个空 handler，tick 静默停止。

**原因**
x86 上会有三种设备先后报到：PIT（全局，Irq+1）、HPET（全局，Irq+2）、每颗 CPU 的 LAPIC
（BSP 在 Irq+4，AP 在自己起来时）。全局设备一次只能打断一颗 CPU，所以每颗 CPU 最终都该落在
自己的 LAPIC 上。原来的 `check_new_device()` 无条件换绑：谁最后来谁上。

`rebind_device()` 还有三错：handler 先被 `move` 进新设备，紧接着 Oneshot 分支又拿这个已被
搬空的对象去 `switch_to_oneshot()`；`set_affinity` 用的是**旧**设备的中断号和掩码，
什么都没改；第一次绑定没有可继承的 deadline。

**改法**
`check_percpu()`：mask 得含本 CPU；per-CPU 设备（mask 恰好只有本 CPU）直接放行；
全局设备不能顶掉 per-CPU 的。`check_preferred()`：oneshot 能力压倒 rating；
然后比 rating，但 mask 不同的优先——经过第一关，mask 不同只剩"新的是 per-CPU、旧的是全局"
这一种情况。被拒返回 `AlreadyBound`。

`rebind_device()` 照 `tick_install_device()` + `tick_setup_device()` 重写：
旧设备的 handler 和 `next_event` 先存局部变量，旧设备置空 handler、`switch_mode(Detached)`，
新设备 `Shutdown`，最后按模式恰好交出一次。affinity 设**新**设备。
拆出 `setup_periodic()`（不支持原生 periodic 的设备用 oneshot 模拟，从上层看不出区别）
和 `setup_oneshot()`。

jiffies 的推进 CPU 在这里认领：`ClockSource::Default::CPUNUM` 初值 `INVALID_CPU_NUM`，
第一个绑上真实设备的 CPU 占下它。不写死 CPU 0，因为只有拥有运行中设备的 CPU 才保证会 tick。

**rating 里 HPET 反着来**：Linux 给 HPET 50、PIT 110，但那里两者不会同时存在
（`hpet_time_init()` 先试 HPET，失败才注册 PIT）。我们是 PIT 先入队、HPET 后入队，而
`hpet_init_internal()` 一开 legacy replacement route，IRQ0 就从 PIT 转到 HPET——
**拥有中断线的设备必须是 TickDevice 绑定的那个**，否则中断落进 HPET 的 `handle_event()` 里
没有 handler。所以 HPET 给 120，赢下它在 Linux 数值下会输的比较。

LAPIC 的 per-CPU 身份靠 mask 只含自己。它是 `CPU_LOCAL` 对象，构造函数只跑在模板上、
之后按字节复制，所以构造时取 `&m_cpumask` 会让所有副本都指向模板里的那一个；
mask 留空，`init_apic_timer()` 在每颗 CPU 自己起来时填。

**参照**
`Linux tick_check_new_device()` / `tick_check_percpu()` / `tick_check_preferred()` /
`tick_install_device()` / `tick_setup_device()` / `tick_setup_periodic()` /
`clockevents_exchange_device()`；jiffies 的 CPU 是 `tick_do_timer_cpu`。

**验证**
构建通过，clang-tidy 干净（第 2 条起挂着的 use-after-move 随 handler 修正消失）。
单核/双核 QEMU 的判别实验见 `x86-clock.md` 第 3 条。

**故意没做**
广播（一个全局设备用 IPI 唤醒休眠的 CPU）没做，所以输掉选择的设备是直接放在一边，
而不是装成广播设备。Linux `tick_check_percpu()` 里"全局设备的中断能否重定向到本 CPU"那一条
也没做——`irq::set_affinity()` 目前是个永远成功的桩，那条会是空话；注释写了它将来该插在哪。

---

## 6. 把周期 tick 接到调度器（2026-09-02）

**文件**
- `tick_sched.{hpp,cpp}` — `on_tick()` 成为 tick 的语义层；`InitLevel::Threading` 的 init hook
- `tick_device.cpp` — `handle_periodic_event()` 补齐三步
- `clock_source.hpp` — `CPUNUM` 初值改 `INVALID_CPU_NUM`

**症状**
PIT 每 10 ms 打一次中断，什么也不会发生。

**原因**
`handle_periodic_event()` 只做 `advance_ticks(1)`。全树唯一调 `Scheduler::Current::tick()` 的是
`TickSched::handle_event()`，而它只能由一个 `Timer` 驱动（当时永不到期），
`TickSched::emulate()` 又无人调用，`time::init()` 是空函数。

而且**每颗 CPU 的设备都推 jiffies**，N 核机器上 jiffies 跑 N 倍快；头文件里写的
「Requires: 当前 CPU 是 CPUNUM」没有任何代码兑现。

**改法**
```
中断 → ClockEventDevice::handle_event()
     → TickDevice::handle_periodic_event(dev)
         1. cpu == CPUNUM ？ advance_ticks(1)                  // do_timer()
         2. TickSched::get().on_tick(now)                      // update_process_times()
              Scheduler::Current::tick(now)
              prof::Sampler::on_scheduler_tick()
              TimerQueue::get().run_expired_timers()           // run_local_timers()
         3. oneshot 模拟的 periodic ？ program_event(next_event + NSEC_PER_TICK)
```

补编程从**上次 deadline** 起算而不是从 now 起算，中断延迟不会累积成漂移。若错过了 tick，
deadline 落在过去，`program_event()` 钳到最小 delta，丢掉的 tick 每次中断补一个。
Linux 在这里循环、每个错过的 tick 调一次 `tick_periodic()`，那是因为它的 jiffies 喂
timekeeping；我们的时间来自 `Clock`，jiffies 只是计数。

`TickSched::on_tick()` 把 tick 的语义和来源分开：Legacy 由设备的周期 handler 直接调，
HpTick 由 `m_timer` 调同一个函数。`Shutdown` 模式下忽略 tick——tick 在 `task::init()` 之前就会来
（PIT 在 Irq+1 开始中断，LAPIC 在 Irq+4 靠数 jiffies 校准），那时 `Scheduler::init()` 还没跑。
切到 Legacy 的时机是 `InitLevel::Threading`，`task::init()` 刚把每颗 CPU 的 scheduler 建好之后，
由 BSP 替所有 possible CPU 置位。

**参照** `Linux tick_handle_periodic()` → `tick_periodic()` → `do_timer()` + `update_process_times()`。

**验证** 构建通过。QEMU 实测见 `x86-clock.md` 第 3 条。

**故意没做**
`emulate()` 原来调的 `Timer::set_deadline()` 只有声明没有定义——之前没炸是因为 `emulate()`
无人引用被 `--gc-sections` 丢掉了。删掉那行（`activate()` 本身就设 deadline）。
`emulate(HpTick)` 仍然无人调用。

---

## 7. `TimerQueue` 加锁，`Timer` 的同步 cancel 与 forward（2026-09-02）

**文件**
- `timer.{hpp,cpp}` — 三个状态字段、同步 `cancel()`、`trylock_or_cancel()`、`forward()`、
  排序键并列处理
- `timer_queue.{hpp,cpp}` — per-CPU 锁、`with_queue_locked()`、`activate_timer()`、
  `run_expired_timers()` 的三段式
- `kernel/include/ours/task/wait_queue.hpp`、`kernel/kernel/task/wait_queue.cpp` —
  超时回调改用 `trylock_or_cancel()`

**症状**
带超时的 sleep 和等待队列永不醒；提前醒来则是 use-after-free——线程被别的原因唤醒后
`Timer` 析构了，节点还挂在 `m_active_timers` 里。同一时刻到期的两个 timer 第二个插入失败。

**原因**
`Timer` 是栈对象（`sleep_etc()`、`park_thread_postamble()` 里都是局部变量），
所以唯一重要的约定是：**`cancel()` 返回时，回调没在跑、也不会再跑**。原来的 `cancel()` 只置
一个标志、节点留在队列里。`TimerQueue` 又完全没锁，线程上下文的 `activate` 和中断里的
`run_expired_timers` 改同一棵树。`Set` 是 `insert_unique`，而 `operator<` 只比 `latest()`。

**改法**
三个状态字段，照 Zircon：

| 字段 | 谁写 | 用途 |
|---|---|---|
| `m_queue` | `activate_timer()` 在队列锁下 | 挂在哪个 CPU 的队列上；改 timer 先锁它 |
| `m_active_cpu` | `run_expired_timers()` 摘链时写、回调返回后清 | "被哪颗 CPU 占有"；`cancel()` 自旋等它清零 |
| `m_cancelled` | `cancel()` 第一件事 | 给回调看：需要取消者持有的锁时用 `trylock_or_cancel()` 放弃 |

锁是 per-CPU 而不是全局（Zircon 是一把全局 `TimerLock`，Linux 是每个 `hrtimer_cpu_base`
一把；跟 Linux）。代价是 `Timer` 必须能从别的 CPU 找到并锁住它所在的队列——
`with_queue_locked(timer, fn)`：读指针、锁、**在锁下再读一次确认没被搬走**，不一致就放锁重来。
`m_queue` 是在队列锁下写的，而读它时还没拿到锁，读到的可能是旧值：读到"在 CPU 0"，
去锁 CPU 0，而另一个人在 CPU 2 上 `activate` 了它——锁住 CPU 0 保护不了 CPU 2 队列里的节点。

`run_expired_timers()` 每个 timer 一轮：锁下 `pop_expired_locked()`（摘链、标 `m_active_cpu`），
**放锁**跑回调，锁下处理 `Restart` 并清 `m_running`，最后清 `m_active_cpu`。
放锁跑回调是回调能 `activate` / `cancel` 其他 timer（或自己）的前提。
`m_active_cpu` 必须在放锁**之前**标上：否则放锁到回调开始之间，别的 CPU 的 `cancel()`
会看到"不在树里、也没人占着"，以为回调已经跑完而销毁 timer。

跨 CPU 取消时不重编程对方的设备：让它到点空跑一次，比发 IPI 通知便宜。

`forward()` 照 `hrtimer_forward()`：把 deadline 按整数个 interval 推到 now 之后并返回跳过了
几个周期。排序键并列时按地址排，等价于 Linux timerqueue 对相等键"插到右边"。

**等待队列的死锁**：`Event::wait_etc()` 持着 PI 锁调 `park_thread()`，而
`park_thread_postamble()` 醒来后持着这把锁 `cancel()`。超时回调原来直接 `Guard{pi_lock}`：
回调等锁、取消者等回调。改成 `timer->trylock_or_cancel(pi_lock)`，被取消就直接返回。
锁手动 `release()`，因为 `lockdep::Guard` 没有"接管一把试出来的锁"的构造。

**参照**
`Zircon Timer::Set()` / `Timer::Cancel()` / `Timer::TrylockOrCancel()` /
`TimerQueue::Tick()` / `WaitQueue::TimeoutHandler()`；
`Linux lock_hrtimer_base()` / `hrtimer_forward()` / `hrtimer_cpu_base::lock`。

**验证** 构建通过，clang-tidy 改动行干净。

**故意没做**
- **slack 合并**（Zircon `InsertIntoTimerList()` 按 `[earliest, latest]` 区间靠拢那套）。
- 一度加了私有的 `TimerQueue::LockGuard` 直接 `acquire_irq_save`，绕开 `Guard<..., IrqSave>`
  是空操作这个缺陷。**已撤回**：本地分叉共享原语会让树里出现两套加锁写法、把真正的缺陷从
  负责修它的人眼前藏起来、共享修复落地后还留一坨死代码。缺陷本身记在
  `repairs/260903/sync.md`，`timer_queue.hpp` 的类注释里也写明"不要在本地绕开"。
- `m_running` 一度删掉过（它没有比 `m_active_cpu` 多任何信息），按要求保留，
  但移到 `run_expired_timers()`：在交出 timer 的那个临界区置位、在收回它的那个临界区清零，
  语义是"本 CPU 正在这个 timer 的回调里"，即 Linux 的 `hrtimer_cpu_base::running`。
  `pop_expired_locked()` 只做摘链和占有标记——弹出时 timer 还没在跑。
- 批量弹出所有到期 timer（Linux 老 timer wheel 的做法）没做：那会让 timer 处在"不在树里、
  也还没开始跑"的中间状态，`cancel()` 就要同时处理两种成员身份。hrtimer 和 Zircon 都是一次一个。

---

## 8. 接口与命名：`Deadline` 工厂、`enroll`、`HZ` 常量（2026-09-03）

**文件**
- `timer.hpp` — `Deadline` 具名工厂、`TimeSlack` 加 `explicit`
- `clock_source.{hpp,cpp}` — `entroll`→`enroll`、`m_priority`→`m_rating` 并接进 `enroll()`、
  `Default::CLKEVT`→`SOURCE`、`read()` 返回 `Ticks`
- `clock.cpp` — 补上 `SystemClock::now()` / `ticks()` 的定义
- `cfg.hpp` — `HZ` 从宏改成常量
- `kernel/kernel/task/thread.cpp`、`tick_sched.cpp`、`timer.cpp` — 调用点

**症状**
`Deadline(Milliseconds(10))` 能编译，读起来像"10 毫秒之后"，实际是"纪元后 10 毫秒"——
一个几十年前的时刻。`Deadline()` 是"已经超时"，而每个这么写的调用方想说的都是"没有超时"。
`ClockSource::m_priority` 从来没人赋值而排序又靠它，等于所有源并列。
`SystemClock::now()` / `ticks()` 只有声明没有定义。

**原因**
隐式构造 + 默认值语义反了。`HZ` 是个两字母的 `#define`，而且写在 `cfg.hpp` 的 include
**之前**，于是之后被拉进来的每个头文件都看到它占着全局命名空间。

**改法**
默认构造 = 无穷，绝对时刻走 `Deadline::at(when, slack)`，`infinite()` 保留，
`TimeSlack` 构造函数加 `explicit`（一个裸 duration 是 deadline 的可能性远大于 slack）。

`HZ` 改成 `ours::time::HZ`，`OURS_CONFIG_HZ` 保留为宏——它是必须能从命令行设定的编译开关。
类型取 `Duration::rep`（i64）而不是 `usize`：`Nanoseconds / u64` 会取 `int64_t` 和 `uint64_t`
的公共类型即 `uint64_t`，`NSEC_PER_TICK` 会悄悄变成和 `Duration` 不同的类型。

`m_rating` 接进 `enroll(self, hz, rating)`，HPET 给 Linux 的 250。
注意这和 `ClockEventDevice::Rating` 是两把不同的尺子——一个量"读时间准不准"，
一个量"发中断合不合适"，同一个 HPET 在两把尺子上的位置没有关系。

`Default::CLKEVT` 是个 `ClockSource *` 却叫 clkevt，改名 `SOURCE`，并把这个类的真实状态
写进注释：只有计数器那一半是活的，`init()` 没有定义、`SOURCE` 恒为空。

**参照** `Zircon Deadline::after()/at()`；`Linux clocksource::rating`（jiffies 1、ACPI PM 200、HPET 250、TSC 300）。

**验证** 构建通过，clang-tidy 干净。

**故意没做**
- **`after(Duration)` 没加**：至今每个调用方都因为自己的原因先读了时钟，手上就有那个时刻。
- **`Ticks` 不改成有符号。** 审查里建议照 Zircon 的 `zx_ticks_t` 用 i64，理由是"无符号让所有
  `< 0` 检查失效"。那两个 `< 0` 检查都是死代码、已删，理由没了；而反方向的理由是硬的：
  `Ticks` 装的是会回绕的硬件计数器，`Clock::time_of()` 用"当前值减锚点"读它，
  **只有无符号算术才能跨过回绕给出正确的差**。这条写进了 `ClockSource::read()` 的注释，
  免得下次又被这个建议绕回来。只统一了类型（`read()` 从 `usize` 改成 `Ticks`）。
- **`thread.cpp` 的 `detach()` 那条是审查判错。** 我说"`join(nullptr, Deadline())` 本意是无限等
  却立刻超时"——不对，`Thread::join()` 只在 `state() != Dead` 时才 park，而 `detach()` 只在
  已死时走到那里，deadline 根本不会被读。改成 `Deadline::infinite()` 是为了说实话。
- slack 的现状：`latest()` 同时被用作排序键和到期判据，所以带 slack 的 timer 总是把 slack 用满
  （"late"策略）。

---

## 9. `ClockEventDevice::Vtable` 改成按方法自动生成（2026-09-03）

**文件**
- `clock_event_device.hpp` — `priv::clkevt_vtable<Driver, Member>()` + `CLKEVT_VTABLE_OF`
- `clock_event_device.cpp` — `enroll()` 校验能力与表是否一致
- `i8253.cpp`（−14 行）、`hpet.cpp`（−16 行）、`local_apic.cpp`（−12 行）

**症状**
无功能缺陷；是可维护性问题，且有一个静默失效模式。

**原因**
手写表错在四处，不只是丑：

1. 每个驱动都要把九个槽位列全，包括 `nullptr` 的。"本设备不支持 suspend"这句话被以
   `nullptr` 的形式说了九遍，而不是干脆不提。
2. **往 `Vtable` 加一个槽位不会让任何驱动编译失败**——指定初始化器会把没列出的成员值初始化，
   于是新槽位在所有驱动里静默为空。这比编译错误糟。
3. 槽位名和方法名的配对是手写的，而九个槽位里有五个签名都是
   `(ClockEventDevice *) -> Status`，把 `resume()` 填进 `suspend` 槽照样通过类型检查。
4. LAPIC 为了运行期切模式，不得不把 `constexpr` 的表改成可变的，并在每次
   `setup_timer()`（任意 CPU、任意时刻）里改写别的 CPU 正在分发的函数指针。

**改法**
`consteval` 函数逐槽问驱动有没有对应的方法：

```cpp
if constexpr (requires { &Driver::set_mode_periodic; }) {
    static_assert(requires(Driver *d) { { d->set_mode_periodic() } -> std::same_as<Status>; },
                  "set_mode_periodic must be `auto set_mode_periodic() -> Status`");
    vtable.set_mode_periodic = Packer::template pack<&Driver::set_mode_periodic>();
}
```

**两步探测是有意的**：先问"有没有这个名字"，再断言签名对。只测可调用性的话，返回类型写错的
方法会被静默跳过——那正是要消灭的失效模式。驱动侧只剩一处接线，不定义的操作就是不支持的操作。

三个踩到的细节：表放在**命名空间作用域**而不是 `ClockEventDevice` 的静态成员——做成员时
`Vtable` 在类体内还不完整，静态成员声明不上 `constexpr`，结果表在运行期是对的、编译期却读不出来。
LAPIC 保留一份**可变副本**（`= CLKEVT_VTABLE_OF<...>`），`select_timer_mode()` 只在 BSP 上改两个
槽位一次，那时没有 AP 在跑、也没有设备入队。构造函数的 mem-initializer 是 complete-class
上下文，所以在那里能求值；类体里的静态成员初始化器不是。

自动绑定的失效模式（名字写错 → 槽位静默为空 → 第一次切模式时报 Unsupported，
离错误现场好几层）由 `enroll()` 的一致性检查堵住：声明了 `Feature::Periodic` 就必须有
`set_mode_periodic`，等等。生成器里另加一条永久 `static_assert`：设备至少要能被 tick 或 time
之一武装。

**参照** 无，本项目自有设计。曾提过改用 `virtual`（基类给默认实现返回 `Unsupported`，
开销与现在相同且少一次 `container_of`），被否决，理由是指令缓存。

**验证**
构建通过，clang-tidy 干净。绑定结果用一段临时探针在**编译期**证实过（放在成员函数体里
才能求值），九条断言全部成立：HPET 定义的五个槽位都绑上了，没定义的四个
（`set_next_event_time` / `suspend` / `stop_oneshot` / `broadcast`）都是空。证实后探针已删除。

**故意没做**
- `ClockSource::Vtable` 还是手写的（同一套探测直接可套，槽位只有五个）→ 待修表。
- 驱动自注册（linker section，照 `Scheduler::init()` 读 `SCHEDULES_START/END`）→ 待修表。
  它解决的是"谁来探测"，与"怎么分发"正交。

---

## 10. 应用 `ClockBase` 的 per-clock offset（2026-09-03）

**文件**
- `timer_queue.{hpp,cpp}` — `earliest_deadline_locked()` 减 offset，
  `pop_expired_locked()` 加 offset

**症状**
潜伏。每个 offset 都是 0：Monotonic 的按定义是 0，Boottime 只在跨越挂起时才和 Monotonic
分离，而挂起还不存在。`Boottime` 也没有真正的使用者。

**原因**
`ClockBase::offset` 声明了却从没人读。一个 timer 的 deadline 是用它自己的 clock 表达的，
而 tick 设备是按单调时间编程的，所以跨 clock 取最小值之前必须先换算。
反方向同理：`pop_expired_locked()` 拿单调的 `now` 直接和 clock 时间的 deadline 比也是错的。

**改法**
`monotonic = clock_time - offset`，`clock_now = now + offset`。前者跳过 `INFINITE_TIME`
（减一个 offset 只会把它绕回可表示范围）。头文件里注明 offset 该在什么时候被写：
Boottime 的会在机器第一次挂起时变成非零，那是 setter 该出现的地方。

减完要重新包一层 `TimePoint<>`：减一个 duration 得到的是 `std::chrono` 的基类型。

**参照** `Linux __hrtimer_get_next_event()` 在同一位置减 `hrtimer_clock_base::offset`。

**验证** 构建通过。offset 恒为 0，所以这是"表达式正确"而不是行为变化，实测观察不到差别。

**故意没做** 没有加 setter——挂起/恢复还不存在，加了就是没人调的死代码。
