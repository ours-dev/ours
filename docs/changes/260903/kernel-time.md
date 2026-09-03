# kernel/time — 改动记录

这一批工作的"改了什么、为什么"。每节标题里带自己的日期（2026-09-02 至 09-03）。
待修项在 `docs/repairs/260903/kernel-time.md`。

**关于章节编号**：这些内容原本是 `docs/repairs-20260830.md` 的章节，编号原样保留，
因为各节之间大量互相引用（"见第三十节"）。那份文件同时有另一个 agent 在写，编号已经撞车——
比如有两个"第三十一节"，另一个是 cpu-local 的。以本目录下的文件为准。

**格式**：下面这些条目写在 `TEMPLATE.md` 定下格式之前，字段没有显式标出，
但内容上覆盖的是同样几件事。本文件最后一条是按模板写的，可作参考。

**为什么没从原文件里删掉**：那份文件正被并发写入，整文件重写会冲掉别人在写的内容。
所以这里是复制，原文保留作归档。

---

## 二十三、`Clock` 的构造函数解引用空指针（2026-09-02）

`Clock::Clock()` 原来在构造函数里装 `ClockSource::Default::get()`，而它返回的是
`*CLKEVT`——一个只有 `ClockSource::Default::init()` 才会填的 `static inline ClockSource *`，
而那要等到 `time::init()`。`SystemClock::CLOCK` 和 `SteadyClock::CLOCK` 都是
`static inline`，构造函数跑在 `ccrt::init_global_static_objects()` 里，是 `start_kernel`
的第一件事，比 `time::init()` 早了十几级。所以那里的 source 恒为空，
`m_cycle_last = source->read()` 是一次穿过空 vtable 的调用。

之所以一直没炸：低地址在 `x86_init_mmu_early()` 把 `pgd[0] = pgd[511]` 之后是通向 physmap
的，读得到垃圾而不缺页——和第十八节第二站、第十九节那个是同一类掩盖。

改成从「没有 source」开始：`ticks()` 本来就对空 source 返回 0，`change_source()` 也本来就
把第一个真实 source 走 `override_source()`。构造函数现在只给 `m_canary` 上正确的 magic
（它原来是 0，一旦断言打开 `verify<Self>()` 就是下一个要炸的地方），其余字段显式清零。

`numerator_` / `denominator_` 保持 0 而不是 1/1：tick→ns 的比例是 clock source 的属性，在
它到来之前假装 tick 就是 ns 是个错误答案，而 0 会在 `update_time()` 的断言上变成可诊断的。
顺带说一句，**全树没有任何地方给 `Clock::numerator_` / `denominator_` 赋过值**，
`update_time()` 里的 `cycles_delta * numerator_ / denominator_` 目前必然除零——这在流程上还
没走到，先记在这里，`update_time()` 里加了断言把它变响。

## 二十六、`kernel/time` 审查（2026-09-02）

只读不改。对照物：Linux `kernel/time/{clockevents,clocksource,tick-common,hrtimer,timekeeping}.c`
和 Zircon `kernel/kernel/timer.cc`。按「先炸的先列」排序，末尾给修复顺序。

### A. 事件链断在两处——现在没有任何调度 tick，也没有任何定时器会到期

**A1. `Timer` 到 `TimerScheduler` 是断的。** `TimerScheduler::activate_timer()` 是空函数
（`timer_scheduler.cpp:12`），`Timer::forward(Duration)` 也是空的（`timer.cpp:21`）；
`Timer::set_deadline()`、`Timer::forward(Time, Duration)`、`TickDevice::update_jiffies_tick/time()`、
`ClockSource::Default::init()` 都是**只有声明没有定义**。后果：`Thread::Current::sleep_etc()` 和
`WaitQueue::park_thread_postamble()` 里的超时永远不会触发——带超时的睡眠等于无限睡眠。

**A2. `TickDevice` 的周期处理函数只推进 jiffies。** `TickDevice::handle_periodic_event()` 做的全部事
情是 `ClockSource::Default::advance_ticks(1)` 再补编程下一次事件；它不调 `TickSched`，也不调
`task::Scheduler::Current::tick()`。全树唯一调 `Scheduler::Current::tick()` 的地方是
`TickSched::handle_event()`，而它只能由一个 `Timer` 驱动（A1 说了永远不会到期），且
`TickSched::emulate()` 全树无人调用，`time::init()` 是空函数。

Linux 的对应链是 `tick_handle_periodic()` → `tick_periodic()` → `do_timer()`（只在
`tick_do_timer_cpu` 上推进 jiffies）+ `update_process_times()` → `scheduler_tick()` +
`run_local_timers()`。方案：

1. `handle_periodic_event()` 里先判断 `Cpu::id() == ClockSource::Default::CPUNUM` 再推进
   jiffies（现在每颗 CPU 的设备都推，SMP 上 jiffies 跑 N 倍快；头文件里写的「Requires: 当前 CPU
   是 CPUNUM」没有任何代码兑现）。
2. 之后无条件调一个 `TickSched::on_tick(now)`：`Scheduler::Current::tick(now)` +
   `prof::Sampler` + `TimerScheduler::Current::run_local_queues()`。Legacy 模式下 `TickSched` 就
   是这个入口，不需要 `Timer`；HpTick 模式下才由 `Timer` 驱动同一个函数。
3. `time::init()` 里初始化 `ClockSource::Default`（把 `CPUNUM` 设成 BSP）并对每颗 CPU
   `TickSched::emulate(Legacy)`；AP 起来时在 `start_nonboot_cpu()` 里再做一次。

### B. 会立刻出错的正确性问题

**B3. `ClockEventDevice::program_event()` 的「现在」是 epoch 0。**
`clock_event_device.cpp:100` 写的是 `auto const now = TimePoint<>();`，于是 `delta = expiry - 0`
是绝对时间，永远被 `std::clamp` 钳到 `m_max_delta`——每次编程都是最大间隔。Linux
`clockevents_program_event()` 用 `ktime_get()`。改成 `SteadyClock::now()`。

**B4. `TickDevice::program_event()` 的「无穷」比较是错的。** 它拿
`ustl::NumericLimits<TimePoint<>>::max()` 当哨兵，但 `std::numeric_limits` 对 `time_point` 没有特
化，主模板的 `max()` 返回 `T()`——也就是 epoch 0。所以 `TimerQueue::update_next_deadline()` 在队列
为空时传进来的 `TimePoint<>::max()` 走不进 shutdown 分支，反而按 B3 的 `max_delta` 反复编程；
而一个恰好等于 0 的 deadline 会把设备关掉。改成和 `INFINITE_TIME` 比较。Linux 的对应是
`KTIME_MAX` → `clockevents_switch_state(ONESHOT_STOPPED)`。

**B5. `ClockEventDevice::entroll(self)` 的默认参数自相矛盾。** 它展开成
`entroll(self, HZ, 128, HZ)`：`min_delta = 128 > max_delta = 100`，`configure()` 第一行的
`DEBUG_ASSERT(min_delta < max_delta)` 必炸；LAPIC 定时器（`init_apic_timer()`）走的正是这条。
就算断言关着，`freq = HZ = 100` 会让 `m_scaler` 变成 1e7 ns/tick，LAPIC 的 delta 换算全错。
方案：删掉无参重载，LAPIC 用校准出来的频率调带参版本；Linux 的 `clockevents_config_and_register()`
就是三个参数全必填。

**B6. `TickDevice::rebind_device()` 有三个错。**
(a) `handler` 先被 `ustl::move` 进新设备，紧接着 Oneshot 分支又拿这个已被搬空的 `handler` 去
`switch_to_oneshot()`——oneshot 模式下换设备会丢掉处理函数。
(b) `irq::set_affinity(m_clkdev->irqnum(), m_clkdev->cpumask())` 用的是**旧**设备的中断号和掩码。
(c) `check_new_device()` 无条件接受：没有 rating 比较、没有 feature 检查、没有 per-CPU 优先。
LAPIC 只有 `Feature::Oneshot`，被绑到还处在 Periodic 模式的 `TickDevice` 上时
`switch_mode(Periodic)` 返回 `Unsupported`，返回值被丢弃，设备停在 `Detached`。
Linux `tick_check_new_device()` 的规则：新设备 cpumask 必须含本 CPU；per-CPU 设备优先于全局设备；
rating 高者胜；oneshot-capable 优先。`ClockEventDevice` 需要加一个 `rating` 字段。

**B7. `ClockSource::enroll()` 把频率丢了，通知也发错了对象。**
`hz` 参数没有任何用途；`SystemClock::notify(self)` 传的是 `self` 而不是刚选出的 `best`；只通知
`SystemClock`，`SteadyClock::CLOCK` 永远没有 source——而全树用的都是 `SteadyClock::now()`，
它会在 `update_time()` 的断言上停下（§二十三加的那条），断言关着就是除零。
`select_best_source()` 的 for 循环第一次迭代必 `break`，`Flags::Unstable` 完全不起作用；它读
`REGISTERED_SOURCES` 不拿 `REGISTERED_SOURCES_MUTEX`。
方案：`ClockSource` 记住 `freq` 并算出 `(mult, shift)`——`calculate_mult_shift()` 已经写好在
`mod.cpp` 里，全树无人调用；`Clock::override_source()` 从 source 取比例；enroll 先通知
`SteadyClock`；选择用 rating 并跳过 Unstable。

**B8. `Timer::cancel()` 只置位，而 `Timer` 是栈对象。** `run_expired_timers()` 从不检查
`m_cancalled`；`sleep_etc()` / `park_thread_postamble()` 里的 `Timer` 在线程被别的原因唤醒后
就析构了，但节点还挂在 `m_active_timers` 里——`~Timer()` 的 `DEBUG_ASSERT(!is_linked())` 先炸，
关掉断言就是 use-after-free。「懒删除避免锁竞争」的注释在栈对象上不成立。
方案照 Zircon `Timer::Cancel()`：拿队列锁摘链；若回调正在别的 CPU 上执行，自旋等
`active_cpu_` 回到 INVALID 再返回（`Timer::m_active_cpu` 字段已经有了，没用上）。Linux 对应
`hrtimer_cancel()` / `timer_delete_sync()`。

### C. 并发与锁

**C9. `TimerQueue` / `TimerScheduler` 没有任何锁。** `activate()` 来自线程上下文（`sleep_etc`
里在 `pi_lock` 下，IRQ 关着），`run_expired_timers()` 来自中断——同一个 `Set` 被两个上下文改。
Zircon 是每 CPU 一把 `timer_lock`（IrqSave 自旋锁），Linux 是 `base->lock`。方案：
`TimerScheduler` 持一把 per-CPU `SpinLock`；跑回调时先摘链、置 `active_cpu`、放锁，再调回调
（Zircon `TimerQueue::Tick()` 的顺序），回调里才能安全地重新 `activate()`。

**C10. `Clock::update_time()` 是「读时改状态」。** `now()` 在中断和线程里并发调用就会撕裂
`m_time_shift` / `m_cycle_last`；`m_seq` 声明了没用。另外 `cycles_delta * numerator_` 对 HPET
（14.3 MHz，`numerator_ ≈ 1e9`）几秒就溢出 64 位；32 位 source 还需要 mask（Linux
`clocksource->mask`）。建议照 Zircon `current_mono_time()`：`now = affine(read())`，读路径纯函数不
写任何东西，只在换 source 时在 seqlock 下改 base；换算用 `(mult, shift)` 或 `mtl::scale`（它已经是
128 位中间值）。

**C11. `ClockSource::Default::read()` 为读一个字拿 IrqSave 自旋锁。** jiffies 在 Linux 是一次
volatile 读。改 `std::atomic<Ticks>` relaxed 读；`advance_ticks()` 只在 CPUNUM 上调（见 A2）。

**C12. `HpetClockSource::m_cache` 不是原子的。** contended 路径在别的 CPU 上裸读，持锁方裸写。
Linux `read_hpet()` 用 `union hpet_lock` 的 cmpxchg。改 `std::atomic<u64>`。

**C13. `ClockEventDevice::GCED_MUTEX` 是睡眠锁，却用 `Guard<Mutex, IrqSave>`。** 现在只在 init 期
无争用地过一遍所以没事，但这个组合本身是错的。

### D. 未初始化与 canary——和 §二十三 `Clock` 同一类

**D14.** `TickDevice`、`TickSched`、`ClockSource`、`TimerQueue` 的 `m_canary` 都用了空构造
（magic 0），而各自的方法都 `verify<Self>()`：断言一开，`TickDevice::program_event()`、
`ClockSource::enroll()`、`TimerQueue::run_expired_timers()` 第一次进入就炸。
`TimerScheduler` / `TimerQueue` 没有构造函数，`cpunum_` / `m_cpunum` / `m_running` /
`m_clock_source` 全是垃圾；`Timer` 的 `m_queue` / `m_active_cpu` 同样没初始化。

**D15.** `NOOP_DEVICE` 是 `INIT_DATA`，但每 CPU `TickDevice` 的模板拷贝里 `m_clkdev` 指着它；
没拿到真实设备的 CPU 在 init 段回收后持有悬空指针。改成普通 static，或 `m_clkdev = nullptr`
并在 `program_event()` 里判空。

### E. 驱动侧

**E16. HPET `set_next_event_tick()` 没有「写完比较器再确认没错过」。** 读计数器、加 delta、写比
较器之间计数器还在走，delta 小时比较器一写就已经落后，oneshot 模式下这次中断永远不来，tick
停摆。Linux `hpet_next_event()` 写完后再读一次计数器，落后就返回 `-ETIME` 让上层重试。
`set_next_event_time()` 是空实现却返回 `Ok`，应在 vtable 里填 `nullptr`。

**E17. i8253 `set_mode_periodic()` 写的 latch 是 0**（=65536 分频，18.2 Hz），不是 HZ。Linux
`LATCH = (PIT_TICK_RATE + HZ/2) / HZ`。

**E18. LAPIC。** `set_next_event_tick()` 的除数编码索引错（§二十二第 2 条已记）；
`set_next_event_tick_deadline()` 的 `delta < 0` 对 `usize` 是死代码；校准
（`calibrate_apic_clock_single`）靠 `ClockSource::Default::read()` 被 PIT/HPET 的周期中断推进，
依赖 A2 那条链。

**E19. `arch_time_init()` 注册的是 virq 0，unmask 的是 `apic_io_isa_to_global(0)`。** 有 ISA
override（IRQ0 → GSI2 很常见）时两者不是同一根线。另外 `hpet.hpp` 的 `HPET_CLKEVT` /
`is_hpet_enabled()` / `hpet_enable()` / `hpet_disable()`，`arch/time.hpp` 的 `time::hpet_init()` /
`pit_init()`，`platform/timer.hpp` 的整套 `current_mono_ticks()` 等，都是悬空声明。

### F. 接口设计

**F20. `Deadline` 的隐式构造是个坑。** `Deadline(Duration)` 的含义是「自 epoch 起的绝对时刻」，
`Deadline()` 是 epoch 0。`thread.cpp:300` 的 `join(nullptr, time::Deadline())` 本意是无限等，
实际是「立刻超时」。`Timer::operator<` 按 `latest()` 排序，但 slack 的合并语义
（earliest/latest 区间）没有任何地方用到。建议照 Zircon：`Deadline::after(Duration, slack)` /
`Deadline::at(TimePoint)`，构造函数 `explicit`，默认值 = infinite；`TimeSlack` 带 mode
（Early / Late / Both）。

**F21. 两套时间表示并存。** `Ticks` / `Time` 都是 `usize`（`system/public/ours/types.hpp`），
和 chrono 的 `TimePoint<>` / `Duration` 并存；无符号让所有 `< 0` 检查失效。建议 `Ticks` 用
`i64`（Zircon `zx_ticks_t`），删掉 `Time`。`cfg.hpp` 用 `#define HZ` 且放在 include 之前，
污染全局命名空间，改 `constexpr`。

**F22. 杂项。** `m_cancalled` → `m_cancelled`；`entroll` → `enroll`；`ClockSource::Default` 的成
员叫 `CLKEVT`（它是 ClockSource，不是 ClockEventDevice）；`ClockSource::m_priority` 无人设置；
`SystemClock::now()` / `ticks()` 声明未定义，`REAL_BASE` 未用；`timer_queue.hpp` 尾注释写成
`TIMER-QUEUE`。

### 建议的修复顺序

按启动会先撞上什么来排，每步单独验证：

1. D14 / D15：canary 和未初始化字段——不修这个，断言一开什么都跑不到。
2. B7 + C10：`ClockSource` → `Clock` 的频率贯通，`now()` 改成纯函数——`SteadyClock::now()`
   能出正确时间，调度器和 mutex 自旋才有意义。
3. B3 / B4 / B5：`program_event` 的 now、无穷比较、`entroll` 默认参数。
4. B6：`check_new_device` 的 rating + feature + per-CPU 规则。
5. A1 / A2：把周期 tick 接到 `TickSched` → `Scheduler::tick()` → `run_local_queues()`。
6. B8 + C9：`TimerScheduler` 加锁、实现 `activate` / 同步 `cancel` / `forward`。
7. E：HPET 的错过检测、PIT 的 latch、LAPIC 的除数编码和中断号。
8. F：`Deadline` 接口、`Ticks` 类型、命名。

## 二十七、`kernel/time` 第一步：并掉 `TimerScheduler`，补齐 canary 与未初始化字段（2026-09-02）

对应第二十六节的 D14 / D15，外加一个你定的结构调整：**丢掉 `TimerScheduler`，让 `TimerQueue`
兼职它的工作**。

### 并入

`TimerScheduler` 除了一个 `TimerQueue perclock_[MaxNumClocks]` 数组和两个空函数之外没有任何自己
的状态，于是 `TimerQueue` 直接成为 per-CPU 对象（继承 `mm::CpuLocal::Current<TimerQueue>`，和
`TickSched` 一样），原来「每个 clock 一个 queue」变成它私有的 `ClockBase m_bases[]`——每个 base
是一个 `Set<Timer>` 加一个 `offset`，就是 Linux `hrtimer_cpu_base` 里嵌 `hrtimer_clock_base[]`
的形状；Zircon 的 `TimerQueue` 也是 per-CPU 对象、上面没有任何东西。`timer_scheduler.{hpp,cpp}`
删除，`Timer::activate()` 现在调 `TimerQueue::activate_timer()`（仍是空的，第六步填），
`TickSched::handle_event()` 调 `TimerQueue::get().run_expired_timers()`。

顺带去掉的：原 `TimerQueue::m_clock_source` 指针——没有任何读写，per-clock 的时间换算已经由
`ClockBase::offset` 表达。`m_next_timer_deadline` 提到队列一级（`m_next_deadline`），因为一颗
CPU 只有一个 tick 设备，「下一次编程时刻」是跨 clock 取 min 的，不属于单个 clock。

### 绑定 CPU 号

`TimerQueue` 的构造函数只跑在模板拷贝上，写不进 CPU 号。补了 `TimerQueue::init(cpunum)`，由
`time::init()` 从 BSP 遍历 `cpu_possible_mask()` 对每个 unit 调一次——照抄 `task::init()` 绑
`Scheduler` 的做法。为什么不放到 AP 自己的启动路径：BSP 上发起的 sleep / 等待队列超时可能在
AP 起来之前就要挂到它的队列上，队列若还是 `INVALID_CPU_NUM` 就不知道该编程谁的 tick 设备。
`time::init()` 之前是空函数，这是它的第一项工作。

### canary 与未初始化字段

| 对象 | 之前 | 现在 |
|---|---|---|
| `TickDevice` / `TickSched` / `ClockSource` / `TimerQueue` | `m_canary()`，magic 为 0，任何 `verify<Self>()` 必炸 | `m_canary(this)` |
| `TickSched::m_last_tick` / `m_next_tick` | 未初始化 | 0 |
| `TimerQueue::m_cpunum` / `m_running` / `m_next_deadline` | 未初始化 | `INVALID_CPU_NUM` / `nullptr` / `INFINITE_TIME` |
| `Timer::m_queue` / `m_active_cpu` / `m_cancelled` | 未初始化 | `nullptr` / `INVALID_CPU_NUM` / `false` |
| `TimeSlack::m_slack` | `= default` 构造留下未定值 | `Duration{}` |

`Timer::m_cancalled` 顺手改成 `m_cancelled`（只在 `timer.hpp` / `timer.cpp` 两处）。

### `NOOP_DEVICE`：比 D15 多出一个问题

D15 说的是它放在 `INIT_DATA`，per-CPU `TickDevice` 模板里的指针在 init 段回收后悬空。改的时候发
现**它的 vtable 指针本身是空的**：`rebind_device()` 换设备时对旧设备调 `shutdown()`，而
`ClockEventDevice` 的每个 vtable 包装都是先 `m_vtable->slot` 再判空——第一次 `enroll`（PIT）
就会穿过空 vtable 读。和第二十三节 `Clock` 那次同一个掩盖机制：低地址通向 physmap，读到垃圾
不缺页。现在它带一个全空槽位的 `constexpr Vtable`，放普通 `.data`，进了匿名命名空间。

### 验证

`ninja -C out/bringup.x86` 通过。`clang-tidy` 在改动行上只剩两条：`TimerQueue::cpunum()` 加了
`[[nodiscard]]`；`NOOP_DEVICE` 的命名——`.clang-tidy` 要 `VariableCase: lower_case`，但全树的
命名空间级对象都是 UPPER_CASE（`I8253_CLKEVT`、`CLOCK_EVENT`、`HPET_MMIO`，以及同一批警告里的
`CLOCK`、`GCED`、`TICKS`……），配置和约定不一致是既有问题，这里按约定并加了带理由的 NOLINT。
`tick_device.cpp:64` 的 `bugprone-use-after-move` 是第二十六节 B6(a) 那条，留给第四步。

下一步：B7 + C10，`ClockSource` → `Clock` 的频率贯通，`now()` 改成纯函数。

## 二十八、`kernel/time` 第二步：频率贯通到 `Clock`，`now()` 改成纯函数（2026-09-02）

对应第二十六节的 B7 + C10。

### `ClockSource` 记住频率，并把它变成一个比例

`enroll(self, hz)` 原来把 `hz` 扔掉，`Clock::numerator_ / denominator_` 全树无人赋值。现在
`enroll` 算出 `m_ticks_to_ns = 1e9 / hz`（`mtl::reduce` 约分后装进 `mtl::Ratio<u32>`；
HPET 的 1e9/14318180 约成 50000000/715909），暴露 `ticks_to_time(Ticks) -> Duration`。
`hz == 0` 或约分后仍装不进 u32 的频率（> 2^32 Hz）返回 `InvalidArguments`。

**为什么是 `mtl::Ratio` 而不是我先前说的 `calculate_mult_shift()`**：`(mult, shift)` 是一个
32×64 的乘积，只在 delta 有界时才不溢出，而 Linux 靠每个 tick 一次的 `timekeeping_update()`
推进 `cycle_last` 来保证这个有界——那正是我们要去掉的「读时写」。`now()` 一旦是纯函数，
delta 就是「自上次换 source 以来」的全部 ticks，无界，需要 96 位中间值。`mtl::scale` 就是这么算
的（高低 32 位分开乘再除），`ClockEventDevice::m_scaler` 已经在用它。`calculate_mult_shift()`
留着没动。

### `Clock` 变成一个 seqlock 保护的仿射映射

```
now() = anchor.time + source->ticks_to_time(source->read() - anchor.ticks)
```

`Anchor { source, ticks, time }` 三个字整体在 `sync::SeqLock`（`ours/sync/seqlock.hpp`，树里已
有但从没人用过）下快照；读路径不写任何东西，中断里和调度器锁下随便调。唯一的写者是换
source：`override_source()` 在 `write()` 里先用旧 anchor 算出此刻的时间，再把它钉到新 source
的当前计数上——时间在换源时连续。原来的 `update_time()`、`m_cycle_last`、`m_time_shift`、
`m_time_base`、`numerator_ / denominator_`、那个从没用过的 `AtomicU8 m_seq` 全部删除。

计数器回绕靠无符号减法处理，前提是 64 位计数器；`enroll()` 的注释写明了这个要求，32 位源以后
要接的话需要 mask 加周期性重新锚定（Linux 的做法），不是这一步的事。

**没有 source 时 `now()` 返回 0**，不再断言。理由：页老化（`PageQueues::rotate_reclaimable_queues`）
和调度时钟在 `time::init()` 之前就会读它，「还没开始计数所以什么都没过去」是个自洽的答案，
和 `ticks()` 一直以来的语义一致；这些调用方拿到 0 delta 都无所谓。

### `enroll` 的通知与选择

- 通知的是选出来的 `best`，不再是 `self`。
- 先通知 `SteadyClock`（新加了 `SteadyClock::notify()`），再通知 `SystemClock`——内核读的是前者，
  它不能空着。
- `select_best_source()` 原来的 for 循环第一次迭代必 `break`，`Unstable` 形同虚设。现在按
  Linux `clocksource_select()` 的两条规则：跳过 `Unstable`；tick 设备在 oneshot 模式时跳过没有
  `HpClock`（即 `CLOCK_SOURCE_VALID_FOR_HRES`）的源。
- `REGISTERED_SOURCES` 加了 `TA_GUARDED`，`enqueue_source` / `select_best_source` 加了
  `TA_REQUIRES`，`enroll` 一次拿锁把入队和选择做完；原来 select 是裸读列表。

### 验证

`ninja -C out/bringup.x86` 通过；`clang-tidy` 在改动行上干净（剩余的全是既有的
`static inline` 成员大写命名那一类）。下一步：B3 / B4 / B5，`ClockEventDevice::program_event`
的 now、`TickDevice::program_event` 的无穷比较、`entroll()` 的默认参数。

## 二十九、`kernel/time` 第三步：`program_event` 的 now、无穷比较、`entroll` 默认参数（2026-09-02）

对应第二十六节的 B3 / B4 / B5。

### B3：`ClockEventDevice::program_event()` 的 now

`auto const now = TimePoint<>();` 是 epoch 0，`delta = expiry - 0` 是绝对时间，永远被钳到
`m_max_delta`。改成 `SteadyClock::now()`（第二步之后它才有正确的值）。

顺带改了一个语义：过去的 deadline 不再返回 `InvalidArguments`，而是按 `m_min_delta` 编程——
钳位自然覆盖了负 delta。调用方是 `TimerQueue`，它没有重试循环，对一个已经晚了的 deadline
它要的就是"硬件能给的最早一次中断"，这是 Linux `clockevents_program_event()` 的 `force` 路径。

### B4：`TickDevice::program_event()`

三处：

1. **periodic 模式下是空操作。** 设备本来就每 1/HZ 打一次，定时器队列从那个 tick 里跑；碰设备
   反而有害——periodic PIT 的"下一次事件"寄存器就是它的周期，periodic 模式的 APIC 定时器会
   重装最后写进去的计数。Linux 同样只在 `hres_active` 时让 `hrtimer_reprogram()` 碰设备。
   之前 `Driver::reset_preempt()` 每次时间片重置都会往 periodic PIT 里写 latch。
2. **无穷比较**：`ustl::NumericLimits<TimePoint>::max()` 是未特化的 `std::numeric_limits`，返回
   值初始化的 `TimePoint`，即 epoch——"无穷"分支对时间 0 触发，对真正的无穷永不触发。改成和
   `INFINITE_TIME` 比。
3. **无穷的处理是停而不是关**：oneshot 模式下 `INFINITE_TIME` 意味着"没有待处理的定时器"，
   进 `OneshotStopped`（Linux 对 `KTIME_MAX` 的 `ONESHOT_STOPPED`），下一个真实 deadline 再唤醒；
   原来是 `Shutdown`，而 `ClockEventDevice::program_event()` 对 `Shutdown` 返回 `BadState`，
   一旦队列空过一次设备就再也编程不了。`switch_mode` 的返回值现在也检查了。

### B5：`entroll()` 的默认参数

无参重载 `entroll(self)` 展开成 `(HZ, 128, HZ)`：`min 128 > max 100` 断言必炸，`freq = 100`
让设备以为一个 tick 是 10 ms。删掉它，Linux 的 `clockevents_config_and_register()` 三个参数
也都是必填。

唯一的调用方是 LAPIC。`init_apic_timer()` 现在用校准结果算频率：`APIC_PERIOD` 是除数为 1 时
每 1/HZ 秒的总线周期数，`DIVISOR` 是计数器实际经过的除数，所以设备的 tick 频率是
`APIC_PERIOD * HZ / DIVISOR`；min/max 用 Linux 的 `0xF` / `0x7FFFFFFF`。**没校准就不入队**
（`APIC_PERIOD == 0`）：拿一个编造的频率入队，会把一台每个 delta 都错的设备交给 tick 层，
不如多在 PIT/HPET 上待一会儿；AP 一定在 BSP 校准之后才走到这里，所以对 AP 永远不会是 0。

留给第七步的：`set_next_event_tick()` 对 `divisor ≤ 128` 的路径把 `DIVISOR` 的原始值当编码写进
`TimerDivConf`（原始值 1 的编码是"除以 4"），以及 TSC-deadline 模式下 tick 其实是 TSC 周期、
频率不是这个数——第二十二节第 2 条和第二十六节 E18 已记。

### 验证

`ninja -C out/bringup.x86` 通过，`clang-tidy` 改动行干净。下一步：B6，
`TickDevice::check_new_device / rebind_device` 的 rating + feature + per-CPU 规则，以及被 move
空的 handler 和用错设备的 `set_affinity`。

## 三十、`kernel/time` 第四步：`TickDevice` 的设备选择与换绑（2026-09-02）

对应第二十六节的 B6。这一步的核心是把 Linux `kernel/time/tick-common.c` 里
`tick_check_new_device()` 那一套决策搬过来，所以先说清楚它在解决什么问题。

### 为什么需要"选择"

一颗 CPU 的 tick 只能由一个事件设备驱动，但 x86 上会有三种设备先后来报到：PIT（全局，
Irq + 1）、HPET（全局，Irq + 2）、每颗 CPU 自己的 local APIC 定时器（per-CPU，BSP 在
Irq + 4 校准后，AP 在自己起来时）。全局设备一次只能打断一颗 CPU，所以每颗 CPU 最终都应该
落在自己的 LAPIC 上，全局设备只是 LAPIC 还没就绪时的过渡。原来的 `check_new_device()`
无条件换绑：谁最后来谁上，LAPIC 只支持 oneshot 却被绑到 Periodic 模式的 TickDevice 上，
`switch_mode(Periodic)` 返回 `Unsupported` 被丢弃，设备停在 `Detached`。

### 三条规则（`tick_check_percpu` + `tick_check_preferred`）

`TickDevice::check_new_device(newdev)` 现在依次问：

1. **它能打断我吗？**（`check_percpu`）`newdev.cpumask` 得含本 CPU。若它是 per-CPU 设备
   （mask 恰好只有本 CPU），直接放行；若它是全局设备，而本 CPU 现在用的是 per-CPU 设备，
   拒绝——per-CPU 设备白拿，不会被别的 CPU 抢走，没理由换成一个要跟别人共享的。
   Linux 还多一条"全局设备的中断能不能被重定向到本 CPU"（`irq_can_set_affinity`），
   我们的 IRQ 层没有这个查询，`irq::set_affinity()` 目前是个永远成功的桩，所以这条现在
   是空话，注释里写了它将来该插在哪。
2. **oneshot 能力压倒一切**（`check_preferred` 上半）：不支持 oneshot 的设备不能替换支持
   oneshot 的，本 CPU 已经在 Oneshot 模式时更不能装。高精度定时器和 idle 停 tick 都依赖它。
3. **然后比 rating**（`check_preferred` 下半），但 mask 不同的设备优先——经过第 1 条，
   mask 不同只剩一种情况：新的是 per-CPU、旧的是全局，那么不管 rating 都换。这就是 Linux
   给 LAPIC 打 100、PIT 打 110 却仍然每颗 CPU 都用 LAPIC 的原因。

被拒绝时返回 `Status::AlreadyBound`（"本 CPU 保持现有绑定"），Linux 这时会把它装成广播设备，
我们还没有广播，先放着。

### `rating` 与 per-CPU mask

`ClockEventDevice` 加了 `Rating m_rating`（构造函数第四个参数）。三个驱动：

| 设备 | rating | 说明 |
|---|---|---|
| PIT | 110 | Linux 的值 |
| LAPIC | 100 | Linux 的值；低于 PIT 无所谓，per-CPU 先于 rating |
| HPET | **120** | Linux 是 50，这里故意高于 PIT，见下 |

HPET 为什么反着来：Linux 里 HPET 和 PIT 不会同时存在——`hpet_time_init()` 先试 HPET，失败才
注册 PIT，所以 HPET 的低 rating 从没和 PIT 比过。我们是 PIT 先入队、HPET 后入队，而
`hpet_init_internal()` 一开 legacy replacement route，IRQ0 就从 PIT 手里转到 HPET，
`time::CLOCK_EVENT` 也指向 HPET；**拥有中断线的设备必须是 TickDevice 绑定的那个**，否则中断
落进 HPET 的 `handle_event()` 里没有 handler，tick 就没了。所以 HPET 必须赢下它在 Linux 数值
下会输的比较。这是一个因初始化顺序而生的约定，注释写在 `hpet.cpp` 的常量上。

LAPIC 的 per-CPU 身份靠 mask 只含自己一颗 CPU。`ApicTimerEvent` 是 `CPU_LOCAL` 对象，构造
函数只跑在模板上，之后按字节复制到每个 unit——构造时取 `&m_cpumask` 会让所有副本都指向模板
里的那一个。所以 mask 留空，`init_apic_timer()` 在每颗 CPU 自己起来时填 `CpuMask::of(Cpu::id())`
再 `set_cpumask()` 指回自己的成员。这一类"CPU_LOCAL 对象里的自指针"是第二十五节那个坑的近亲。

### `rebind_device()` 的三处修正

照 `tick_install_device()` + `tick_setup_device()` 重写：

1. **handler 不再丢**。原来先把 handler `move` 进新设备，再把被搬空的对象传给
   `switch_to_oneshot()`——Oneshot 模式下换设备装进去的是个空 handler。现在旧设备的 handler
   和 `next_event` 先存到局部变量，然后旧设备置空 handler、`switch_mode(Detached)`
   （`clockevents_exchange_device()`），新设备 `Shutdown`，最后按模式
   `setup_periodic()` / `setup_oneshot()` 把局部变量交出去，恰好一次。
2. **affinity 设的是新设备**。原来 `set_affinity(旧设备的 irq, 旧设备的 mask)`，什么都没改。
   现在对全局设备（mask ≠ 本 CPU）且 irq 有效时，把它的中断指向本 CPU。
3. **第一次绑定**（从占位设备换到真设备）没有可继承的 deadline，oneshot 路径用 `now()`
   武装，和 `tick_setup_device()` 的 `ktime_get()` 一致。

`setup_periodic()`：设备原生支持 periodic 就切过去；不支持的（TSC-deadline 模式的 LAPIC、
没有 `Tn_PER_INT_CAP` 的 HPET 比较器）切 oneshot 并武装一个 tick 之后，之后由
`handle_periodic_event()` 每次重新武装——上层看不出区别。`setup_oneshot()` 装 handler、
切 oneshot、`program_event(next_event)`（`INFINITE_TIME` 意味着什么都不武装）。

顺带：`ClockEventDevice::switch_mode(Detached / Shutdown)` 对没有 `shutdown` 槽位的设备
（HPET）现在返回 Ok——没东西可关不是失败，Linux 的 `set_state_shutdown` 同样可选；不改这个，
`rebind_device()` 就 detach 不掉 HPET。

### 补：占位设备靠"输掉一切比较"被替换，不靠特判

你指出 `m_clkdev` 永远非空（至少是 `NOOP_DEVICE`），`check_preferred()` 里的 `has_device()`
没必要。确实：那两处特判真正防的只是 `NOOP_DEVICE` 的 `m_cpumask == nullptr`，最后一行
`m_clkdev->cpumask()` 会解引用空指针；其余判断对它天然成立。

改法是让占位设备成为一个合法但必输的设备：`Feature::None`（不会成为挡住 periodic-only 新设备
的 oneshot 在位者）、rating 0（低于一切真设备）、一个**空的** `CpuMask`（既不是 per-CPU，
所以全局设备可以替换它；也不等于任何真设备的 mask，所以 `check_preferred()` 选新的）。
`check_percpu()` / `check_preferred()` 里的 `has_device()` 全部删掉。`rebind_device()` 里那个
保留：它不是空指针保护，是语义区别——第一次绑定没有可继承的 deadline，要用 `now()` 武装
（Linux `tick_setup_device()` 的 `if (!td->evtdev)`）；从占位设备继承的话拿到的是默认构造的
`next_event` 即 epoch 0，会立刻触发一次，行为碰巧对但是靠巧合。

空 mask 按你的建议提成了公共的：`ours/cpu.hpp` 里加了 `cpu_none_mask()` / `cpu_all_mask()`，
与 `cpu_possible_mask()` 并列，就是 Linux `cpumask.h` 的 `cpu_none_mask` / `cpu_all_mask`。
返回的是对静态对象的引用，因为调用方（`ClockEventDevice`）存的是指针，临时对象会悬空；
`cpu_all_mask()` 是位集的全部容量（内核编译时的 CPU 上限），比 `cpu_possible_mask()`
（固件报告的实际存在的）宽，这个区别和 Linux 一致。曾试过做成 `CpuMask::NONE` 静态成员，
但 `CpuMask` 是 `ustl::BitSet` 的别名，要为此改 ustl，撤回了。占位设备本身不做成公共的：
Linux 也没有统一的占位 `clock_event_device`，它用 `evtdev = NULL` 加
`clockevents_handle_noop`，占位设备是 `TickDevice` 一家的事。

### 验证

`ninja -C out/bringup.x86` 通过；`clang-tidy` 改动行干净（第二十七节起一直挂着的
`tick_device.cpp` use-after-move 随 handler 修正一起消失）。下一步：A1 / A2，把周期 tick 接到
`TickSched` → `Scheduler::tick()` → `TimerQueue::run_expired_timers()`。

## 三十一、`kernel/time` 第五步：把周期 tick 接到调度器（2026-09-02）

对应第二十六节的 A1 / A2。这一步之后，调度器第一次收到 tick。

### 之前的链断在哪

`TickDevice::handle_periodic_event()` 只做 `advance_ticks(1)`；全树唯一调
`Scheduler::Current::tick()` 的是 `TickSched::handle_event()`，而它只能由一个 `Timer` 驱动，
`TickSched::emulate()` 又无人调用。所以 PIT 每 10 ms 打一次中断，什么也不会发生。

### 现在的链（Linux `tick_handle_periodic()` → `tick_periodic()`）

```
中断 → ClockEventDevice::handle_event()
     → TickDevice::handle_periodic_event(dev)
         1. cpu == ClockSource::Default::CPUNUM ？ advance_ticks(1)      // do_timer()
         2. TickSched::get().on_tick(now)                                // update_process_times()
              Scheduler::Current::tick(now)
              prof::Sampler::on_scheduler_tick()
              TimerQueue::get().run_expired_timers()                     // run_local_timers()
         3. 设备是 oneshot 模拟的 periodic ？ program_event(next_event + NSEC_PER_TICK)
```

三点说明：

- **jiffies 只由一颗 CPU 推进。** 之前每颗 CPU 的设备都推，N 核机器上 jiffies 跑 N 倍快。
  哪颗 CPU 推：`ClockSource::Default::CPUNUM`，初值 `INVALID_CPU_NUM`，由**第一个绑上真实
  设备的 CPU** 在 `TickDevice::rebind_device()` 里认领——这就是 Linux `tick_setup_device()`
  认领 `tick_do_timer_cpu` 的做法。不写死 CPU 0 是因为只有拥有运行中设备的 CPU 才保证会 tick。
- **补编程从上次 deadline 起算而不是从 now 起算**，中断延迟不会累积成漂移。若错过了 tick，
  deadline 落在过去，`program_event()` 钳到设备最小 delta，丢掉的 tick 每次中断补一个。
  Linux 在这里循环、每个错过的 tick 调一次 `tick_periodic()`，那是因为它的 jiffies 喂
  timekeeping；我们的时间来自 `Clock`，jiffies 只是计数，一次补一个够用。
- **`TickSched::on_tick()` 是 tick 的语义层**，和 tick 从哪来分开：Legacy 模式由设备的周期
  handler 直接调，HpTick 模式由 `m_timer` 调（同一个 `on_tick()`，之后 `forward` 一个周期再
  `Restart`——`forward` 还是空的，第六步）。`tick_sched.hpp` 的类注释写了这两种模式和 Linux
  `tick_sched_timer` 的对应。

### 为什么 `Shutdown` 模式下 tick 被忽略，以及谁把它切到 `Legacy`

tick 在 `task::init()` 之前就会来：PIT 在 Irq + 1 入队并开始周期中断，LAPIC 在 Irq + 4 靠数
jiffies 校准自己。那时 `Scheduler::init()` 还没跑，`Scheduler::Current::tick()` 会碰一个未初始化
的调度器。所以 `TickSched` 初始 `Shutdown`，`on_tick()` 在这个模式下直接返回——tick 那时唯一的
用途就是推进 jiffies，而那已经在设备层做完了。

切到 `Legacy` 的时机：`task::init()` 从 BSP 把每颗 possible CPU 的 `Scheduler` 都建好，
`main.cpp` 紧接着 `set_init_level(Threading)`，所以 `tick_sched.cpp` 里挂了一个
`GKTL_INIT_HOOK(StartLegacyTick, ..., InitLevel::Threading)`，从 BSP 遍历 possible CPU 把每个
`TickSched` 置 `Legacy`——和 `task::init()` 建调度器、`time::init()` 绑 `TimerQueue` 同一个
"BSP 替所有 CPU 做"的模式，AP 上线后第一个 tick 就能到达它的调度器。`emulate(HpTick)` 则必须
在本 CPU 上调，因为它武装的 `Timer` 会挂到发起 CPU 的队列上；注释里写了这个约束。

顺带：`emulate()` 原来调 `m_timer.set_deadline({})`，那个函数只有声明没有定义——之前没炸是因为
`emulate()` 无人引用被 `--gc-sections` 整段丢掉了；一旦从 hook 里引用它就会链接失败。
`activate()` 本身就设 deadline，删掉那行。

### 验证

`ninja -C out/bringup.x86` 通过。下一步：B8 + C9，`TimerQueue` 加锁、实现 `activate` /
同步 `cancel` / `forward`——这一步之后带超时的 sleep 和等待队列才第一次会醒。

## 三十二、`kernel/time` 第六步：`TimerQueue` 加锁，`Timer` 的同步 cancel 与 forward（2026-09-02）

对应第二十六节的 B8 + C9。这一步之后带超时的 sleep 和等待队列第一次会醒，也第一次能安全地
提前醒。参照是 Zircon `kernel/kernel/timer.cc`（本地 `../../fuchsia/zircon`），加锁粒度取的是
Linux 的。

### 要解决的是什么

`Timer` 是栈对象（`Thread::Current::sleep_etc()`、`WaitQueue::park_thread_postamble()` 里都是
局部变量）。所以唯一重要的约定是：**`cancel()` 返回时，回调没在跑、也不会再跑**，之后栈帧
可以安全消失。原来的 `cancel()` 只置一个标志、节点留在队列里，等于 use-after-free；
`TimerQueue` 又完全没锁，线程上下文的 `activate` 和中断里的 `run_expired_timers` 改同一棵树。

### `Timer` 的三个状态字段（Zircon 同名）

| 字段 | 谁写 | 用途 |
|---|---|---|
| `m_queue` | `activate_timer()` 在队列锁下写 | 这个 timer 最后挂在哪个 CPU 的队列上。任何要改 timer 的人先锁它——`TimerQueue::lock_queue_of()`：读指针、锁、再读一次确认没被搬走，就是 Linux `lock_hrtimer_base()` 的循环 |
| `m_active_cpu` | `run_expired_timers()` 在摘链时写 CPU 号、回调返回后写 `INVALID` | "回调正在哪颗 CPU 上跑"。`cancel()` 摘链后自旋等它回到 `INVALID`，这就是"不会再跑"从"大概率"变成"一定"的地方 |
| `m_cancelled` | `cancel()` 第一件事就置位 | 给回调看的：回调若需要一把取消者正持有的锁，不能硬拿（取消者在等回调结束，回调在等锁，死锁），`trylock_or_cancel()` 一边试锁一边看这个标志，看到就放弃 |

### `cancel()`（Zircon `Timer::Cancel()` 逐步对应）

1. 置 `m_cancelled`。
2. 若 `m_active_cpu == 本 CPU`：在自己的回调里取消，没法等自己，清掉 handler 返回 `false`。
3. `lock_queue_of()` 锁住所在队列；若还 `is_linked()`，摘链——回调还没开始，摘了就永远不会
   开始，返回值将是 `true`。若摘的是**本 CPU** 队列的头，重新编程设备；若是别的 CPU 的队列，
   不动它的设备：让它到点空跑一次，比发 IPI 通知便宜（Zircon 同样的取舍，注释里写了）。
4. 放锁，自旋等 `m_active_cpu` 回 `INVALID`——覆盖"已经摘链、回调正在别的 CPU 上跑"的情况。
5. 清 handler（重用时忘了 `activate` 会撞空指针而不是跑旧代码）。

### 锁：per-CPU 而不是全局

Zircon 是一把全局 `TimerLock`；Linux 是每个 `hrtimer_cpu_base` 一把。这里跟 Linux：
`TimerQueue::m_lock`，IrqSave 自旋锁，守 `m_bases` / `m_next_deadline` / `m_running`
（全部 `TA_GUARDED`，内部方法 `TA_REQUIRES`）。代价就是上面的 `lock_queue_of()`。
`run_expired_timers()` 每个 timer 一轮：锁下 `pop_expired_locked()`（摘链、标 active），
**放锁**跑回调，锁下处理 `Restart`，最后清 active。放锁跑回调是回调能 `activate` / `cancel`
其他 timer（或自己）的前提。

`activate_timer()` 里三条检查照 Zircon `Timer::Set()`：已在队列里是 bug（`ASSERT`）；在自己
回调里重新武装是允许的（`Restart` 之外的另一种周期写法），但若已被 `cancel` 就静默放弃——
取消者马上要销毁它；在别的 CPU 的回调里武装是 bug。

### `forward()`

Linux `hrtimer_forward()`：把 deadline 按整数个 `interval` 推到 `now` 之后，返回跳过了几个周期。
三种情况：还没到期，不动；晚了不到一个周期，加一个；晚了很多，除法一次跳过去，商截断导致还在
过去时再补一个。只能对没入队的 timer 调（deadline 是树的排序键）。`TickSched::handle_event()`
用它 + `Restart` 实现 HpTick 的周期 tick。`Deadline` 加了 `slack()` 访问器供它重建 deadline。

### 排序键的并列

`Timer::operator<` 原来只比 `latest()`，而 `Set` 是 `insert_unique`——两个同一时刻到期的
timer（同一个 tick 里发出的两个 `sleep_for(10ms)`）第二个会插入失败。现在并列时按地址排，
等价于 Linux timerqueue 对相等键"插到右边"的效果；谁先跑无所谓。

### 等待队列的超时回调（消费者侧唯一的改动）

`Event::wait_etc()` **持着 PI 锁**调 `park_thread()`，而 `park_thread_postamble()` 醒来后
持着这把锁 `cancel()`。超时回调原来直接 `Guard{pi_lock}`：回调等锁、取消者等回调，正是上面
说的死锁。改成 Zircon `WaitQueue::TimeoutHandler()` 的写法：`timer->trylock_or_cancel(pi_lock)`，
被取消就直接返回（线程已经因别的原因醒了，没什么可超时的）。锁手动 `release()`，因为
`lockdep::Guard` 没有"接管一把试出来的锁"的构造；中断本来就是关的，和原来不带 IrqSave 的
Guard 假设一致。`handle_timeout` 签名多了 `Timer *`。`sleep_etc()` 那边 `cancel()` 在 PI 锁
作用域之外，不需要改。

### 补：`m_running` 由 `run_expired_timers()` 设，不在 pop 里

你问 `pop_expired_locked()` 为什么设 `m_running`——语义确实不对：弹出时 timer 还没在跑。
`m_active_cpu` 在同一处设是对的，但它表达的是"被本 CPU 占有"而不是"正在运行"：必须在放锁前
设好，否则放锁到回调开始之间别的 CPU 的 `cancel()` 会把它当成已经跑完而销毁（Zircon 同一位置
的注释是 "Mark the timer busy"）。`m_running` 按你的要求保留，但移到 `run_expired_timers()`：
在交出 timer 的那个临界区里置位、在收回它的那个临界区里清零，和 Linux
`hrtimer_cpu_base::running` 一样，含义是"本 CPU 正在这个 timer 的回调里"，精确到没有任何一刻
回调在跑而它为空。`Restart` 的处理并进了同一个收回临界区，不额外拿锁。

### 补：`Guard<SpinLock, IrqSave>` 根本不关中断，和 `with_queue_locked`

你说 `lock_queue_of` 把锁交出去不好看，改成把工作传进来。改的时候发现一个更要紧的东西。

**`Guard<sync::SpinLock, IrqSave>` 是空操作。** `ours::IrqSave`（`ours/lockdep.hpp:11`）是个空
tag struct，`lockdep::Guard` 只是把它塞进 `std::tuple<bool, Options...>`，没有任何地方读它；
`Guard::guard()` 调的是普通 `acquire()`。`spin_lock.hpp` 底部的
`LOCKDEP_STATE_SAVER(SpinLock, lockdep::SavedStateOption::IrqSave, AutoInterruptDisabler)`
声明了本该让它变真的映射，但 `Guard` 从不消费 `StateSaver`，而且它命名的是 `lockdep::IrqSave`，
和所有调用点传的 `ours::IrqSave` 是**两个不同类型**。

这让第六步的代码带着一个活的自死锁：`activate_timer()` 和 `Timer::cancel()` 跑在线程上下文，
拿本 CPU 的队列锁时中断没关；tick 中断在同一颗 CPU 上跑 `run_expired_timers()`，要同一把锁。
每次 sleep、每次等待队列超时都武装 timer，tick 每 1/HZ 秒一次——这不是理论路径。第五步之前
tick 链是断的，所以一直没暴露。

我一度在 `TimerQueue` 里加了个私有的 `LockGuard`（直接 `acquire_irq_save` /
`release_irq_restore`）绕过去，你否掉了：**不要写自己的 LockGuard，就用 `Guard`，IrqSave 那件事
后面会修，不是现在。** 已撤回，所有临界区用回 `Guard<sync::SpinLock, IrqSave>`。理由是对的——
本地分叉一个共享原语会让树里出现两套加锁写法，把真正的缺陷从负责修它的人眼前藏起来，等共享的
修复落地之后还留下一坨死代码。缺陷记在这里和 `timer_queue.hpp` 的类注释里，调用点保持标准写法。

所以现在的状态是：`kernel/time` 的队列锁在 `Guard` 修好之前不关中断，tick 落在
`activate_timer()` 或 `Timer::cancel()` 里会让这颗 CPU 自死锁。这一条是**已知、已记录、已交给
别人**，不在 kernel/time 这条线里修。

`lock_queue_of()` 换成模板 `with_queue_locked(timer, fn)`：循环里确定队列、`Guard` 锁住、
调 `fn(queue)`，锁不再离开作用域。`cancel()` 那段变成一个 lambda（带
`TA_NO_THREAD_SAFETY_ANALYSIS`，分析器跟不过回调边界）。

全树的 `Guard<..., IrqSave>` 都有这个问题——`scheduler.cpp`、`clock_source.cpp`、
`page_queues.cpp`、`thread.cpp` 的 PI 锁……。正确的修法是让 `Guard` 真正消费 `StateSaver`
（统一 `ours::IrqSave` 和 `lockdep::IrqSave` 这两个同名不同源的类型）。

### 关于 `earliest_deadline_locked()` 的遍历

你问既然是 BST 为什么还遍历。那个循环遍历的是 **clock（当前 2 个：Monotonic、Boottime）**，
不是 timer；每个 `base.active.begin()` 就是取该 BST 的最左节点，`begin_node()` 是
`header->left`（`bstree_algorithms.hpp:47`），一次指针加载，O(1)。所以是 2 次 O(1)，不是 O(n)。

分成多棵树的原因和 Linux `hrtimer_cpu_base` 里放 `hrtimer_clock_base[]` 一样：不同 clock 有各自
的原点，合成一棵树就得把每个 timer 的键归一化到单调时间，而 offset 变化时（比如 realtime 被
settimeofday 调整）要给整棵树重新做键。Zircon 同样是 `monotonic_timer_list_` 和
`boot_timer_list_` 两条链，取最小值时也是两边比。

**顺带发现一个还没处理的缺口**：`ClockBase::offset` 至今没人读。Linux 在这个循环里做
`expires = ktime_sub(hrtimer_get_expires(timer), base->offset)` 把 clock 时间换算成单调时间再比；
`pop_expired_locked()` 里拿单调的 `now` 直接和 timer 的 deadline 比也是同一个问题。现在
`offset` 恒为 0、`Boottime` 也没有真正的使用者，所以是潜伏的；一旦 `Boottime` 有了非零 offset
就会错。留着待定。

### 没做的

slack 合并（Zircon `InsertIntoTimerList` 那套按 `[earliest, latest]` 区间靠拢的逻辑）——
`Deadline` 的 `earliest()/latest()` 还在，树按 `latest()` 排，将来要做只改 `enqueue_locked()`。

### 验证

`ninja -C out/bringup.x86` 通过，`clang-tidy` 改动行干净。下一步：E，驱动侧——HPET 的错过
检测、PIT 的 latch、LAPIC 的除数编码和中断号。

## 三十四、`kernel/time` 第八步：接口与命名（2026-09-03）

对应第二十六节的 F20–F22，最后一步。分三小段做的。

### F-a：命名，以及三个接上了的成员

- `ClockEventDevice::entroll` → `enroll`（5 处）。
- `ClockSource::m_priority` 从来没人赋值，而 `enqueue_source()` 按它排序，等于所有源并列。
  改名 `m_rating` 并加进 `enroll()` 的参数：`enroll(self, hz, rating)`。用 Linux 的
  clocksource 刻度（jiffies 1、ACPI PM 200、HPET 250、TSC 300），HPET 给 250。
  注意这和第三十节 `ClockEventDevice::Rating` 是两把不同的尺子——一个衡量"读时间准不准"，
  一个衡量"发中断合不合适"，同一个 HPET 在两把尺子上的位置没有关系。
- `ClockSource::Default::CLKEVT` 是个 `ClockSource *`，名字却说它是 clock event device，
  改名 `SOURCE`。顺带把这个类的真实状态写进注释：只有计数器那一半是活的（`read()` /
  `advance_ticks()` 有定义，周期 tick 在调），`init()` 没有定义、`SOURCE` 恒为空，
  所以 jiffies 还没有作为 ClockSource 入队，`get()` 不能调（加了断言）。这就是为什么
  `SteadyClock` 在 HPET 于 `InitLevel::Irq + 2` 入队之前一直读到 0。
- `SystemClock::now()` / `ticks()` 是只有声明没有定义。补上：`now()` 就是稳态时钟加
  `REAL_BASE`。没有人读 RTC，`REAL_BASE` 恒为 0，所以今天它和 `SteadyClock` 给出一样的答案
  ——"系统在纪元时刻启动"是这个退化情况下正确的答案，比一个链接不上的声明强。

### F-b：`HZ` 从宏改成常量

`#define HZ 100` 原来写在 `cfg.hpp` 的 include **之前**，于是之后被拉进来的每个头文件都看到一个
两字母的宏占着全局命名空间。改成 `ours::time::HZ`。`OURS_CONFIG_HZ` 保留为宏——它是必须能从
命令行设定的编译开关，这是这棵树里唯一还留着宏的理由。

类型是 `Duration::rep`（i64）而不是 `usize`，这一条不是风格问题：`Nanoseconds / u64` 会取
`int64_t` 和 `uint64_t` 的公共类型即 `uint64_t`，`NSEC_PER_TICK` 会悄悄变成和 `Duration` 不同的
类型。加了个 `static_assert` 兜住 HZ 高到一个 tick 不足 1 纳秒的情况。三个使用点
（PIT 的 latch、LAPIC 的频率、HPET 的比较器初值）改成 `time::HZ`。

### F-c：`Deadline` 只能通过具名工厂构造

两个坑：

1. `Deadline(Duration)` 是隐式的，所以 `Deadline(Milliseconds(10))` 能编译，读起来像"10 毫秒
   之后"，实际是"纪元后 10 毫秒"——一个几十年前的时刻。
2. `Deadline()` 是纪元 0，即"已经超时"，而每个这么写的调用方想说的都是"没有超时"。

现在默认构造 = 无穷，绝对时刻走 `Deadline::at(when, slack)`，`infinite()` 保留。
`TimeSlack` 的构造函数加 `explicit`（一个裸 duration 是 deadline 的可能性远大于 slack）。
四个调用点改掉；那段注释掉的死构造函数删了。

**没有加 `after(Duration)`**：至今每个调用方都因为自己的原因先读了时钟，手上就有那个时刻。
注释里写了将来真有需要时它该放在 `at()` 旁边。

**F20 里我判错的一条**：`thread.cpp` 的 `detach()` 走
`join(nullptr, Deadline())` 并不是"本意无限等却立刻超时"——`Thread::join()` 只在
`state() != Dead` 时才 park，而 `detach()` 只在已死时走到那里，deadline 根本不会被读。
改成 `Deadline::infinite()` 是为了说实话，不是修 bug，注释里写明了。

**slack 的现状**：`latest()` 同时被用作排序键和到期判据，所以带 slack 的 timer 总是把 slack
用满（"late"策略）。合并（coalescing）没有实现，`TimeSlack` 的注释里写了将来 early/late/centre
模式该落在哪。

### F21：`Ticks` 不改成有符号，理由变了

第二十六节建议 `Ticks` 用 `i64`（照 Zircon 的 `zx_ticks_t`），理由是"无符号让所有 `< 0` 检查
失效"。那两个 `< 0` 检查都是死代码，已经在第三步和第七步删掉了，所以这个理由没了。而反方向的
理由是硬的：`Ticks` 装的是会回绕的硬件计数器，`Clock::time_of()` 用"当前值减锚点"读它，
**只有无符号算术才能跨过回绕给出正确的差**。所以保持无符号，并把这个理由写进
`ClockSource::read()` 的注释，免得下次又被"改成有符号"这个建议绕回来。

做了的是类型统一：`ClockSource::read()` 和 `Default::read()` 的返回类型从 `usize` 改成 `Ticks`
（同一个底层类型，只是说清楚它是什么），vtable 槽位同步；`m_max_cycle` / `m_last_cycle` 也改成
`Ticks`，并注明它们是给还没写的 watchdog 用的（Linux `clocksource_watchdog` 靠它们发现跳变的
源并打上 `Flags::Unstable`）。

`Time` / `Instant`（都是 `usize`，在 `system/public/ours/types.hpp`）只被 `platform/timer.hpp`
用到，而那整个文件是悬空声明。按"空声明可能是有意的脚手架"的约定没有删，记在这里。

### 八步做完之后 `kernel/time` 的状态

能用了：`SteadyClock::now()` 有正确的纳秒（HPET 入队之后）；周期 tick 到达调度器；
带超时的 sleep 和等待队列会醒，也能安全地提前取消；设备选择按 per-CPU / oneshot / rating 三条
规则走；PIT、HPET、LAPIC 三个驱动的编程错误都修了。

还欠着的，按重要性：

1. **队列锁不关中断**——`Guard<..., IrqSave>` 是空操作（第三十二节），tick 落在
   `activate_timer()` / `Timer::cancel()` 里会自死锁。已交给别的 agent 修 `Guard`。
2. **`ClockBase::offset` 没人读**（第三十二节）：跨 clock 比较 deadline 时没做换算。
   `Boottime` 现在没有真正的使用者，所以潜伏着。
3. **jiffies 没有作为 ClockSource 入队**：`Default::init()` 无定义，所以 HPET 之前
   `SteadyClock` 读 0。
4. **TSC-deadline 模式的频率错配**（第三十三节）：需要先有 TSC 频率校准。
5. **slack 合并**、**tick 广播（broadcast）**、**clocksource watchdog**、**NOHZ**：都还没有。
6. `platform/timer.hpp` 整个文件、`arch/hpet.hpp` 的 `HPET_CLKEVT` 等悬空声明。

## 三十七、`ClockEventDevice::Vtable` 改成按方法自动生成（2026-09-03）

你说手工打包太丑，要"自动注册"，并在三个方案里选了 A（consteval 生成常量表），
明确排除 B（virtual，理由是指令缓存）。C（linker section 的驱动注册表）是正交的另一件事，
没做。

### 手写表错在哪（不只是丑）

1. 每个驱动都要把九个槽位列全，包括 `nullptr` 的。文件里"本设备不支持 suspend"这句话被以
   `nullptr` 的形式说了九遍，而不是干脆不提。
2. **往 `Vtable` 加一个槽位不会让任何驱动编译失败**——指定初始化器会把没列出的成员值初始化，
   于是新槽位在所有驱动里静默为空。这比编译错误糟。
3. 槽位名和方法名的配对是手写的，而九个槽位里有五个签名都是
   `(ClockEventDevice *) -> Status`，把 `resume()` 填进 `suspend` 槽照样通过类型检查。
4. 每个驱动还要重复 `using Packer = ...<&Self::m_clkevt>` 和把 `&VTABLE` 传给构造函数。

### 做法

`priv::clkevt_vtable<Driver, Member>()` 是个 `consteval` 函数，逐槽问驱动有没有对应的方法：

```cpp
if constexpr (requires { &Driver::set_mode_periodic; }) {
    static_assert(requires(Driver *driver) {
        { driver->set_mode_periodic() } -> std::same_as<Status>;
    }, "set_mode_periodic must be `auto set_mode_periodic() -> Status`");
    vtable.set_mode_periodic = Packer::template pack<&Driver::set_mode_periodic>();
}
```

**两步探测是有意的**：先问"有没有这个名字"，再用 `static_assert` 断言签名对。只测可调用性的话，
返回类型写错的方法会被静默跳过——那正是这次要消灭的"静默空槽"。

驱动侧现在只写一处：

```cpp
m_clkevt("HPET", &time::CLKEVT_VTABLE_OF<Self, &Self::m_clkevt>, ...)
```

不定义的操作就是不支持的操作。PIT 少了 14 行，HPET 少了 16 行，LAPIC 少了 12 行。

驱动要守两条：方法必须 public（生成器不是驱动的 friend），这些名字不能重载
（`&Driver::name` 必须唯一解析）。都写在生成器的注释里。

### 三个细节

- **表放在命名空间作用域而不是 `ClockEventDevice` 的静态成员。** 试过做成员：`Vtable` 在类体内
  还不完整，静态成员就没法声明成 `constexpr`，而没有 `constexpr` 的话表在运行期是对的、编译期
  却读不出来（`static_assert` 会报 "initializer is unknown"）。
- **LAPIC 保留一份可变副本**，因为它对同一个槽位有两个实现、运行期二选一：
  `static ClockEventDevice::Vtable VTABLE = time::CLKEVT_VTABLE_OF<...>;`，
  `select_timer_mode()` 只在 BSP 上改两个槽位一次。生成器提供计数模式的默认值。
- **构造函数的 mem-initializer 是 complete-class 上下文**，所以在那里写
  `&CLKEVT_VTABLE_OF<Self, &Self::m_clkevt>` 能求值。类体里的静态成员初始化器不是——验证时
  一开始把探测断言写在类里，报的就是这个错。

### 补上的一道检查

自动绑定的失败模式是"名字写错 → 槽位静默为空 → 第一次切模式时报 Unsupported，离错误现场好几层"。
`ClockEventDevice::enroll()` 现在校验能力和表是否一致：声明了 `Feature::Periodic` 就必须有
`set_mode_periodic`，声明了 `Oneshot` 就必须有 `set_mode_oneshot`，声明了 `TimePoint` 就必须有
`set_next_event_time`、否则必须有 `set_next_event_tick`。任何一条不满足就带设备名报错返回。
生成器里另加了一条永久 `static_assert`：一个时钟事件设备至少要能被 tick 或 time 之一武装。

### 验证

`ninja` 通过，`clang-tidy` / `clang-format` 干净。绑定结果是用一段临时探针在**编译期**证实的
（放在成员函数体里才能求值），九条断言全部成立：HPET 定义的五个槽位都绑上了，没定义的四个
（`set_next_event_time` / `suspend` / `stop_oneshot` / `broadcast`）都是空。证实后探针已删除。

### 没做的

方案 C：让驱动通过 linker section 自己登记，取代现在 `arch_time_init` / `platform_hpet_init` /
`init_bsp_apic_timer` 三个手写 init hook 和手挑的级别，形式上照 `Scheduler::init()` 读
`SCHEDULES_START/END`。它解决的是"谁来探测"，和这次的"怎么分发"正交。
