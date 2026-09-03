# x86 时钟探测与校准 — 改动记录

格式见 `docs/changes/TEMPLATE.md`。待修项在 `docs/repairs/260903/x86-clock.md`。
提交：`4fbc034 fix(time): make the tick reach the scheduler`。

范围：`kernel/arch/x86/apic/local_apic.cpp`（LAPIC 定时器与校准）、
`kernel/arch/x86/kernel/time/{hpet,time}.cpp`、`kernel/driver/clock/i8253.cpp`。
`kernel/time` 那一侧的改动在同目录的 `kernel-time.md`。

## 起因与追溯

第 1 条出自 `kernel/time` 的整体审查（编号 E16–E19，见 `kernel-time.md` 的追溯表）。
第 2 条起出自一次专门针对 x86/pc 时钟探测与校准的审查（2026-09-03），当时列了 11 条：

| 编号 | 内容 | 归宿 |
|---|---|---|
| 1 | HPET 接管后只响一次，APIC 校准死循环 | 第 3 条 |
| 2 | 校准的计数器差值算反了 | 第 3 条 |
| 3 | TSC-deadline 机器上测的是停着的计数器 | 第 3 条 |
| 4 | 校准期间从没写过分频寄存器 | 第 3 条 |
| 5 | `apic_advisor` 是摆设，重试分支死代码且会死循环 | 第 3 条 |
| 6 | 测了四次只用最不准的两次 | 第 3 条 |
| 7 | 测量没对齐 jiffy 边界 | 第 3 条 |
| 8 | 自旋没有超时，结果没有校验 | 第 2、3、4 条 |
| 9 | `VTABLE` 被运行期无同步改写 | 第 3 条 |
| 10 | HPET 缺三项校验（计数器在走、period 范围、通道数） | 第 3 条 |
| 11 | 杂项：PIT 无探测、32 位 HPET 被拒、TSC 未校准、悬空声明 | 部分修（第 3 条），其余进待修表 |

---

## 1. 三个时钟驱动的编程错误（2026-09-03）

**文件**
- `kernel/driver/clock/i8253.cpp` — `PIT_LATCH`、`shutdown()` 的通道
- `kernel/arch/x86/kernel/time/hpet.cpp` — `set_next_event_tick()` 的回读；
  空的 `set_next_event_time` 槽位改 `nullptr`
- `kernel/kernel/time/clock_event_device.cpp` — 新增 `program_min_delta()`
- `kernel/arch/x86/apic/local_apic.cpp` — 除数编码、寄存器写序、死代码
- `kernel/arch/x86/kernel/time/time.cpp` — 两个中断号空间的注释（非缺陷）

**症状**
jiffies 和建立在它之上的所有超时慢约五倍（PIT）；oneshot 模式下某些中断永远不来（HPET）；
LAPIC 的每个 delta 都错（除数编码）。

**原因**

**PIT**：`PIT_LATCH = 0`，而 8254 把 0 读作 65536——最大分频，得到 DOS 时代的 18.2 Hz
而不是 HZ。下游全都假设每 `NSEC_PER_TICK` 一次中断。同一个文件里 `shutdown()` 的两个字节
写错了通道：`outb(PIT_CH0, 0)` 之后是 `outb(PIT_CH2, 0)`——CH2 是 PC 扬声器。
结果是通道 0 只写了低字节、停在等高字节的状态，顺手拨了一下扬声器的门。

**HPET**：它的比较器是**相等**比较——没有"小于等于"，也没有倒计时模式。所以读计数器和写
比较器之间只要挤进一个 SMI、NMI，或者某些芯片组内部那两个 HPET 周期的写延迟，这次中断就
彻底丢了，下一次要等计数器回绕，64 位 14 MHz 下等于永远。

**LAPIC**：Divide Configuration Register 存的不是除数而是一个 4 位编码，而且编码不按数值顺序
（跳过 bit 2）：除 1 是 0xb，然后 0、1、2、3、8、9、0xa 对应 2…128。所以
`DIVISOR_ENCODING[]` 是按 **log2(除数)** 索引的，而代码写的是 `DIVISOR_ENCODING[divisor - 1]`
——要"除以 4"编出来的是"除以 16"。更糟的是**只有 >32 位那条分支才做编码**，
普通路径把原始除数直接写进寄存器，而原始值 1 的含义是"除以 4"。**这个函数从来没有编对过。**

**改法**
PIT 的 latch 改成 Linux `<linux/timex.h>` 的 `LATCH`：`(PIT_FREQUENCY + HZ/2) / HZ`；
`shutdown()` 两个字节都写 CH0。

HPET 写完回读计数器，判据照 Linux：不是"是否已经越过"，而是"距离目标是否已经不足
`HPET_MIN_CYCLES`"，因为写可能还在芯片组里飞。不满足返回 `Status::TimeOut`（即 -ETIME）。
**检测没有响应就没有意义**，所以配套在 `program_event()` 后面加了 `program_min_delta()`：
被拒绝就往后挪一点再试，最多 10 次，每次多加一个 `m_min_delta`，每次重读时钟（上一次失败
正是因为时间在走）。为什么是每次加一个最小值而不是翻倍：把一次尝试挤过界的是中断或 SMI，
那是个固定开销，不随 delta 缩放。上面没有人会重试——定时器队列编程完就走。

LAPIC 的编码表提到匿名命名空间由校准和 `set_next_event_tick()` 共用，索引改成 log2；
钳位分支后面那个必然失败的 `> 8` 判断删掉；寄存器写序反过来（写初值计数就是启动倒计时，
除数配置必须在它之前）；`set_next_event_tick_deadline()` 里对无符号 `Ticks` 的 `delta < 0`
是死代码，删掉。

**参照**
`Linux LATCH` / `pit_shutdown()` / `hpet_next_event()` / `clockevents_program_min_delta()`；
Intel SDM 卷 3 §11.5.4（APIC 定时器）。

**验证** 构建通过，clang-tidy 改动行干净。

**故意没做 / 判错**
**E19 我在审查里判错了。** 我说 `arch_time_init()` 注册的是 virq 0、unmask 的是
`apic_io_isa_to_global(0)`，有 ISA override 时两者不是一根线。**不对**，两个号故意在不同空间：
IO-APIC 的重定向表按 **GSI** 索引，所以 mask/unmask 要 GSI；但 `platform_init_apic()` 给那个
表项编的向量是 `PlatformIrqMin + isa_irq`，来自 **ISA 号**，`faults.cpp` 的
`irqvec_to_irqnum()` 再减掉同一个基址。所以到达 `handle_irq_generic()` 的是 ISA 号。
改动只有可读性（字面量换成 `ISA_IRQ_PIT`）并把这两个号空间写进注释——它看起来像 bug，
我已经为此查了一遍，别让下一个人再查一遍或者"修"坏它。

---

## 2. jiffies 去掉锁（2026-09-03）

**文件**
- `kernel/include/ours/time/clock_source.hpp` — `TICKS` 改 `std::atomic<Ticks>`，删 `SPINLOCK`
- `kernel/kernel/time/clock_source.cpp` — `read()` / `advance_ticks()`

**症状**
潜伏但会挂死：APIC 校准在几十毫秒里紧密轮询 `read()`，这是最容易撞上的地方。
（这一条是审查里的 C11，原本没排进八步，修 x86 校准时才发现它挡在路上。）

**原因**
`ClockSource::Default::read()` 为读一个字拿 `Guard<sync::SpinLock, IrqSave>`，
而这个组合是**空操作**（见 `repairs/260903/sync.md`）。于是 tick 中断可以落在 `read()`
持锁的那几条指令里，`advance_ticks()` 随即在**同一颗 CPU 上**自旋等待被自己中断的代码
释放锁——自死锁。

**改法**
`TICKS` 改成 `std::atomic<Ticks>`，relaxed 读写，锁删掉。Linux 的 jiffies 同样是一次普通的
volatile 读。只有 `CPUNUM` 那颗 CPU 写，所以三个字段之间的一致性不需要锁。

**参照** `Linux jiffies`（一个 CPU 拥有的字，读侧无锁）。

**验证** 构建通过。校准能跑完就是它不再自死锁的证据。

**故意没做**
没有为此本地造一个真会关中断的 guard——见 `kernel-time.md` 第 7 条的"故意没做"。
`LAST_TICKS` / `NEXT_PERIOD` 仍然无人读，保留。

---

## 3. 校准重写与 HPET 探测校验（2026-09-03）

**文件**
- `kernel/arch/x86/kernel/time/hpet.cpp` — 周期编程搬进 `set_mode_periodic()` 并补齐；
  新增 `shutdown()`、`hpet_counter_is_running()`；period 范围校验；通道数门槛放宽
- `kernel/arch/x86/apic/local_apic.cpp` — `Calibration` 重写、`wait_for_jiffies()` 加上限、
  `calibrate_once()`、`select_timer_mode()`、`setup_timer()` 不再改 vtable

**症状**
**启动挂死**，单核和双核 QEMU 下都必现：内核停在 APIC 校准的自旋里，没有任何输出。

**原因**

**HPET 的周期模式要求写两次比较器**：置上 `Tn_VAL_SET_CNF` 后，第一次写设的是比较器
（第一次触发的绝对时刻），第二次写设的才是累加器（周期）。`hpet_init_internal()` 只写了一次，
累加器留在 0，而 HPET 是相等比较，所以第一次匹配之后比较器不再前进——**响一次，然后静默**。
`HpetChannel::set_mode_periodic()` 那条路更是什么周期都不编程。

连起来：Irq+1 PIT 入队开始推 jiffies → Irq+2 HPET 入队（rating 120 > 110）换绑，PIT 被
shutdown，HPET 响一次就停，jiffies 冻住 → Irq+4 校准自旋等 jiffies 前进，永远等不到。

校准本身另有八处：**差值算反了**（Current Count 递减，代码算 `last - first`，
无符号回绕再截成 u32 得到 `2^32 - 真实值`）；**TSC-deadline 机器上测的是停着的计数器**
（校准以 oneshot 调 `setup_timer`，CPU 报 `TscDeadlineTimer` 就切进 deadline 模式，
而该模式下 Initial Count 被忽略、Current Count 恒为 0，于是 `APIC_PERIOD = 0`，
LAPIC 在 Sandy Bridge 之后的机器上根本不会入队）；**从没写过分频寄存器**（实际用复位值即
除以 2，却记成 1）；`apic_advisor` 参数**函数体里一次没用**，它的重试分支既是死代码
（`else if (best_time[trail] == INVALID)` 在第一次迭代后不可达）又会死循环
（`i -= 1` 写在 `i < 2` 的循环里），还有 u8 截断；**四次测量只用最不准的两对**
（`best_time[2]`、`[3]` 算出来再没被读过）；**没对齐 jiffy 边界**（起点在当前 jiffy 中间的
任意位置，真实经过时间在 `(N-1, N]` 之间，最短那次误差可达 100%）；
**自旋没有超时**；**`VTABLE` 被运行期无同步改写**（`inline static` 一份、所有 CPU 共享，
而 `setup_timer()` 会被任意 CPU 任意时刻调到）。

HPET 探测还缺三项校验：没有验证计数器在走（而 HPET 目前是全树唯一的时钟源，
一个停着的计数器会让 `SteadyClock::now()` 恒定，于是每个 deadline 永远在未来）；
period 只查了 0；`num_channels < 2` 就拒绝但代码只用 channel 0，那个 2 没有出处。

**改法**
周期编程搬进 `HpetChannel::set_mode_periodic()` 并补齐成两次写入；
`hpet_init_internal()` 只做设备级的事（挂 legacy route、清零并启动计数器），
通道 0 的编程交给框架，一个设备只剩一个主人。补 `shutdown()`（掩掉中断），
否则 `rebind_device()` detach 不掉它。

校准：`Calibration::apic_elapsed()` = 首减尾；不走 `setup_timer()`，自己置 `TscMode::OneShot`
并显式设分频（固定用除数 1）；`apic_timer_set_divisor()` 提到匿名命名空间共用；
删掉整套 `apic_advisor`；只测两次，取最短（1 tick）和最长（8 tick）；
`calibrate_once()` 先 `wait_for_jiffies(edge, 1)` 落到边界再写初值计数启动窗口；
`wait_for_jiffies()` 有 10 亿次迭代上限（tick 是唯一推进 jiffies 的东西，
在它被证明在跑之前没有别的时钟可以超时，所以界只能在迭代次数上）；
结果校验（长窗口两个计数都必须大于短窗口，商都不能为 0）；
新增 `select_timer_mode()` 只在 BSP 校准末尾调一次，那时没有 AP 在跑、也没有设备入队。

**顺带解掉一个 TODO**：`Calibration` 本来就同时采样 TSC，只是把它扔了。现在记为
`TSC_PERIOD`，`init_apic_timer()` 按选中的模式入队对应的频率——deadline 模式下设备的"tick"
就是 TSC 周期。这解掉了 `kernel-time.md` 第 4 条留下的那条。

HPET：新增 `hpet_counter_is_running()`（启用计数器后确认它真的在走）；
period 按 Linux 的范围校验（1e5–1e8 飞秒，即 10 MHz–10 GHz）；通道数门槛放宽到 1 并写明理由
——一个编造的门槛可能拒掉能用的设备。

命名：`DIVISOR`（存的是除数值）改成 `DIVISOR_LOG2`；`APIC_PERIOD` 的定义改成"**已分频**时钟
每 tick 的计数"，于是设备频率就是 `APIC_PERIOD * HZ`，不再在入队时又除一次分频。

**参照**
`Linux hpet_set_periodic()`（两次写）、`calibrate_APIC_clock()`（`wait_for_a_tick()` 对齐、
拿 TSC 交叉验证、`"APIC frequency too slow, disabling apic timer"` 的失败路径）。

**验证**
构建通过，clang-tidy 改动行干净。**QEMU 实测启动过了校准**：

```
[info] HPET Frequency = 100000000
[info] switched to clock-source HPET-CLKSRC (100000000 Hz, rating 250)
[info] APIC timer: 10593142 counts/tick at divisor 1, TSC 10593142 cycles/tick
[info] CPU[0] keeps tick device HPET over LVT
```

第三行本身就是周期 tick 工作的证明：校准要等 11 个 tick 才能完成，而在此之前它会永远等下去。
第四行是 `kernel-time.md` 第 5 条选择规则的判别实验——单核上 `cpu_possible_mask()` 就是 {0}，
和 LAPIC 的 per-CPU mask 相等，于是落到比 rating，HPET 的 120 赢；双核下这一行消失，
LAPIC 被接受。规则如设计工作。

启动随后崩在调度器里（`context.schedule()` 为空），不在本模块——那是 linker section 被
`--gc-sections` 回收，已由别人在 `10cceaf` 之前修掉。定位方法记在下面。

**顺带记下的定位方法**（没有调试器时）
临时在 `panic_exception()` 里把 `[rsp]` 附近几个字打出来——`call` 到空指针会先压返回地址。
然后对着**实际启动的**二进制符号化：`kernel.dev.obi` 装的是 `vmours.with-tests`
（入口 0x2f08b），不是 `vmours`；`vmours.image.elf` 是剥过符号的，要用没有后缀的 `vmours`。
插桩已还原。

**故意没做**
PIT 存在性探测（Linux `timer_irq_works()`）、32 位 HPET 支持、TSC 作为时钟源、
`HPET_LEGACY_IRQ_ENABLED` 等悬空声明——都进了待修表。

---

## 4. 频率改用比值法，窗口拉长到 200 ms（2026-09-03）

**文件**
- `kernel/arch/x86/apic/local_apic.cpp` — `Calibration` 新增 `hz_of()`、`counter_ran_out()`、
  `mono_first/last`；`apic_calibrate_clock()` 改用比值法、窗口 8→20 个 tick、
  加时钟源交叉验证；`APIC_PERIOD`/`TSC_PERIOD` 改名 `APIC_HZ`/`TSC_HZ`（前者保留为派生值）；
  `init_apic_timer()` 直接用频率入队

**症状**
潜伏——精度不够但还没有使用者在意。可观察的表现是**不可重复**：同一台 QEMU 两次启动算出的
APIC 频率差 2.37%（986,671,400 与 1,010,100,000 Hz）。而在加交叉验证之前，
连"这个数对不对"都无从判断：只有一个数，没有第二个东西可比。

**原因**
旧算法是「每个 jiffy 多少计数 × HZ」，第二步默认了「一个 jiffy 正好是 1/HZ 秒」——
而那恰恰是被测量的东西。tick 有多少抖动，频率就继承多少。

关键的观察是：`Calibration` 本来就在窗口两端**同时**采 APIC 计数和 `SteadyClock::now()`，
所以可以直接相除。两个量在同一时刻读，采样再晚也是一起晚，延迟在相除时约掉，
**窗口长度根本不进结果**，也就不依赖 tick 准不准。它只依赖时钟源的刻度，
而那是从 HPET 能力寄存器读出来的硬件常数，不是测量值。

第二个杠杆是窗口长度：端点误差是固定开销，与窗口长度无关，所以**相对**误差按 1/T 下降。

**改法**
- 新增 `Calibration::hz_of(count)` = `count × 1e9 / mono_elapsed_ns`。
- tick 周期检查**仍然**用两窗口相减：那里的尺子是整数个 tick，固定开销不会在单个窗口里
  自己约掉，必须在两个不同长度的窗口之间抵消。这一条同时验证三件事——tick 设备是不是真的
  每 1/HZ 响一次、时钟源的 tick→ns 换算对不对、以及由这个窗口导出的频率是否有意义。
- 阈值分两级：tick 设备能出的错都是**倍数级**的（PIT 的 latch 让 tick 长 5.5 倍、
  重复计数减半、设备停了则超时），而测量噪声只有几个百分点（TCG 下两次启动就差 7%）。
  所以 **10% 告警**（Linux 告警的那个数），**2 倍才拒绝**。
- 窗口 70 ms → 200 ms（跨度 7 → 19 个 tick），误差降到约三分之一，启动多花 130 毫秒。
- 拉长窗口带出一个新风险：定时器是 one-shot、从 `0xFFFFFFFF` 往下数、**数到 0 就停**，
  窗口超过量程时"经过的计数"变成上限而不是测量值。唯一可观察的症状是末值为 0，
  故加 `counter_ran_out()`，为真则拒绝校准。
- 命名跟着语义改：被测量的是频率（`APIC_HZ`），每 tick 计数成了派生值。
- 没有时钟源时回落到旧的数 tick 法并 `log::warn`——没有 HPET 的机器上没有独立的尺子。

**参照**
`Linux calibrate_APIC_clock()`：窗口用 `HZ/5`，也是 200 ms；它同样拿 TSC 交叉验证 APIC 的结果，
偏差超 10% 告警。不同之处是 Linux 仍以「每个校准循环多少计数」为主结果，我们直接取比值。

**验证**
同一台 QEMU（单核）各两次启动：

| | 第一次 | 第二次 | 两次相对差 |
|---|---|---|---|
| jiffy 法（旧） | 986,671,400 Hz | 1,010,100,000 Hz | 2.37% |
| 比值法 + 200 ms（新） | 1,000,000,000 Hz | 1,000,000,000 Hz | **0 ppm** |

两次给出**逐位相同**的 1,000,000,000 Hz，正是 QEMU 对 APIC 总线和 TSC 建模的速率。
而 tick 周期仍在动（10,035,842 与 10,095,263 ns，偏离 10 ms 0.36% 与 0.95%）——
噪声全部留在了它该在的地方：那是 tick 设备的真实抖动，不是频率测量的误差。
**把「这个计数器多快」和「一个 tick 多长」分开，前者就不再继承后者的误差。**

双核下另外确认了 per-CPU 选择规则：单核打印 `CPU[0] keeps tick device HPET over LVT`
（被拒），双核没有这一行（LAPIC 被接受）。

构建通过，clang-tidy 改动行干净。

**故意没做**
重复测量取中位数、线性回归代替两点相减、运行期持续修正。理由和标准误公式记在
`repairs/260903/x86-clock.md` 第 4 节。现在的精度（ppm 级）已经远超使用需求——
定时器最小 delta 是 0xF 个 tick。

另外：六个原始采样和两个窗口各自的差值目前只在**失败路径**打印，成功时看不到，
所以还没法审查短窗口的固定开销占比。→ 待修表。
