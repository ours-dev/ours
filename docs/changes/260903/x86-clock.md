# x86 时钟探测与校准 — 改动记录

这一批工作的"改了什么、为什么"。每节标题里带自己的日期（2026-09-03）。
待修项在 `docs/repairs/260903/x86-clock.md`。

编号与来源同 `kernel-time.changes.md` 的说明：原是 `docs/repairs-20260830.md` 的章节，
编号原样保留以维持互相引用，原文件因有并发写入而未删除。**此后新增的条目不再用那套编号**
（它已经在原文件里和别的模块撞车），直接用标题加日期。

**格式**：带编号的条目写在 `TEMPLATE.md` 定下格式之前；本文件最后一条是按模板写的。

---

## 三十三、`kernel/time` 第七步：驱动侧（2026-09-03）

对应第二十六节的 E16–E19。四条里三条是真 bug，一条是我审查时判错了。

### E17：PIT 的 reload 值是 0，即 18.2 Hz

`PIT_LATCH = 0`，而 8254 把 0 读作 65536——最大分频，得到 DOS 时代的 18.2 Hz，不是 HZ。
下游全都假设每 `NSEC_PER_TICK` 一次中断，所以 jiffies 和建立在它之上的所有超时慢了大约五倍。
改成 Linux `<linux/timex.h>` 的 `LATCH`：`(PIT_FREQUENCY + HZ/2) / HZ`，四舍五入。

同一个文件里 `shutdown()` 的两个字节写错了通道：`outb(PIT_CH0, 0)` 之后是
`outb(PIT_CH2, 0)`——CH2 是 PC 扬声器。结果是通道 0 只写了低字节、停在等高字节的状态，
顺手拨了一下扬声器的门。Linux `pit_shutdown()` 是连着两次写 CH0（LSB、MSB）。改了之后
`PIT_CH2` 没人用了，一并删掉（`-Werror=unused-const-variable` 抓出来的）。

顺带给两个模式设置加了注释：0x34 是模式 2（rate generator，计数器自己重装，不需要再编程），
0x38 是模式 4（software-triggered strobe，每次事件都要重新 `set_next_event_tick()`）。

### E16：HPET 写晚了的比较器不是"晚"，是"永远不来"

HPET 的比较器是**相等**比较——没有"小于等于"，也没有倒计时模式。所以读计数器和写比较器之间
只要挤进一个 SMI、NMI，或者某些芯片组内部那两个 HPET 周期的写延迟，这次中断就彻底丢了，
下一次要等计数器回绕，64 位 14 MHz 下等于永远。

`set_next_event_tick()` 现在照 Linux `hpet_next_event()` 写完回读计数器，判据也照抄：不是
"是否已经越过"，而是"距离目标是否已经不足 `HPET_MIN_CYCLES`"，因为写可能还在芯片组里飞。
不满足就返回 `Status::TimeOut`（即 -ETIME）。

**检测没有响应就没有意义**，所以配套在 `ClockEventDevice::program_event()` 后面加了
`program_min_delta()`，即 Linux 的 `clockevents_program_min_delta()`：被拒绝就往后挪一点再试，
最多 10 次，每次多加一个 `m_min_delta`，每次重读时钟（上一次失败正是因为时间在走，用旧的 now
量会以同样的方式再失败）。为什么是每次加一个最小值而不是翻倍：把一次尝试挤过界的是中断或 SMI，
那是个固定开销，不随 delta 缩放，加一个最小值足够跨过它，同时让 tick 尽量准时。上面没有人会
重试——定时器队列编程完就走，周期 tick 那条路是直接断言——所以重试必须在这一层。

另外 `set_next_event_time()` 是个返回 `Ok` 却什么都不做的空实现。HPET 入队时没带
`Feature::TimePoint`，`program_event()` 走不到那条路，但槽位填着它就是个陷阱，改成 `nullptr`。

### E18：LAPIC 的除数编码

Divide Configuration Register 存的不是除数而是一个 4 位编码，而且编码不按数值顺序（跳过 bit 2）：
除 1 是 0xb，然后 0、1、2、3、8、9、0xa 对应 2、4、8、16、32、64、128。所以
`DIVISOR_ENCODING[]` 是按 **log2(除数)** 索引的，而代码写的是 `DIVISOR_ENCODING[divisor - 1]`——
要"除以 4"编出来的是"除以 16"。更糟的是**只有 >32 位那条分支才做编码**，普通路径把原始除数
直接写进寄存器，而原始值 1 的含义是"除以 4"。也就是说这个函数从来没有编对过。

顺带三处：钳位分支（`divisor > 128` 时设成 128）后面紧跟的 `divisor > 8` 判断必然把它判为非法，
是死分支；写寄存器的顺序反了——写初值计数器就是启动倒计时，除数配置必须在它之前，否则管不到
刚刚武装的这个间隔；`set_next_event_tick_deadline()` 里的 `delta < 0` 对无符号的 `Ticks` 是死
代码（过去的 deadline 早在 `program_event()` 就被钳到 `m_min_delta` 了）。

**没修、写成 TODO 留在代码里的**：TSC-deadline 模式下设备的"tick"是 TSC 周期，而
`ClockEventDevice` 的换算用的是入队时给的 APIC 总线频率，两者差一个平台倍数，所以这个模式下
每个 deadline 都偏。修它需要一个校准过的 TSC 频率，而全树没有任何地方测量 TSC 频率
（`grep tsc_freq/tsc_khz/calibrate_tsc` 全空）。

### E19：我判错了，代码是对的

我在第二十六节说 `arch_time_init()` 注册的是 virq 0、unmask 的是
`apic_io_isa_to_global(0)`，有 ISA override 时两者不是一根线。**不对。** 两个号故意在不同空间：

- IO-APIC 的重定向表按 **GSI** 索引，所以 mask/unmask 要 GSI（IRQ 0 常被重映射到 GSI 2）。
- 但 `platform_init_apic()` 给那个表项编的向量是 `PlatformIrqMin + isa_irq`，来自 **ISA 号**；
  `faults.cpp` 的 `irqvec_to_irqnum()` 再减掉同一个基址。所以到达
  `irq::handle_irq_generic()` 的是 ISA 号，`request_irq()` 必须用它。

改动只有可读性：字面量 `0` 换成 `ISA_IRQ_PIT`，并把这两个号空间写进注释——它看起来像 bug，
我已经为此查了一遍，别让下一个人再查一遍或者"修"坏它。同时给 `timer_interrupt()` 加了注释说明
`CLOCK_EVENT` 是谁：先是 PIT，`hpet_init()` 打开 legacy replacement route 之后变成 HPET，
而它必须和 `TickDevice` 绑定的设备一致——这正是第三十节把 HPET 的 rating 抬到 PIT 之上的原因。

### 仍是悬空声明（没动，按"空声明可能是有意的脚手架"的约定只记录）

`arch/hpet.hpp` 的 `HPET_CLKEVT`（一个 `inline` 定义，永远是空 Span）、`is_hpet_enabled()`、
`hpet_enable()`、`hpet_disable()`；`arch/time.hpp` 的 `time::hpet_init()` / `pit_init()`；
`platform/timer.hpp` 的 `current_mono_ticks()` / `current_mono_time()` /
`platform_convert_mono_time_to_ticks()` / `platform_set_oneshot_timer()` / `get_periodic_time()`
——整个文件没有一个定义，也没有一个调用者。

### 验证

`ninja -C out/bringup.x86` 通过，`clang-tidy` 改动行干净。剩下第八步：F，`Deadline` 接口、
`Ticks` 类型、命名（`entroll` → `enroll` 等）。

## 三十五、x86 / pc 时钟探测与校准的审查（2026-09-03）

只读不改。范围：`local_apic.cpp` 的 APIC 定时器校准、`time.cpp` 的 PIT 探测、
`hpet.cpp` 与 `platform/pc/hpet.cpp` 的 HPET 探测。对照 Linux
`arch/x86/kernel/apic/apic.c:calibrate_APIC_clock()` 和 `arch/x86/kernel/hpet.c`。

按严重程度排。**第 1 条是启动挂死，第 2、3 条让校准结果本身没有意义。**

### 1. HPET 接管周期 tick 之后只响一次，然后 APIC 校准死循环

HPET 的周期模式要求**写两次比较器**：置上 `Tn_VAL_SET_CNF` 后，第一次写设的是比较器
（第一次触发的绝对时刻），第二次写设的才是累加器（周期）。`hpet_init_internal()` 只写了一次：

```cpp
mmio->timers[0].conf_caps.set_mode_periodic().mask(false);
mmio->timers[0].comparator_value = HPET_FREQUENCY / time::HZ;   // 只有这一次
```

累加器留在 0，所以第一次匹配之后比较器不再前进，而 HPET 是相等比较——计数器已经越过去了，
再也不会匹配。**一次中断，然后静默。**

`HpetChannel::set_mode_periodic()` 那条路也不编程周期，它只置模式位就返回。于是
`TickDevice::setup_periodic()` 以为 `switch_mode(Periodic)` 之后设备就会每 1/HZ 打一次，
而 HPET 不满足这个约定。

后果连起来看：

```
Irq+1  PIT 入队 → 绑到 BSP 的 TickDevice，jiffies 开始走
Irq+2  HPET 入队（rating 120 > PIT 110）→ 换绑：PIT 被 shutdown，HPET 接管
       → HPET 响一次就停 → jiffies 停在某个值
Irq+4  apic_calibrate_clock() 自旋等 jiffies 前进 → 永远等不到 → 挂死
```

这个 rating 是第三十节我按"拥有中断线的设备必须是绑定的那个"定的，方向没错；错的是 HPET
的周期编程本来就不完整，换绑之前它也只会响一次，只是那时 PIT 还在替它推 jiffies。

Linux `hpet_set_periodic()` 的写法是 cfg 里带上 `HPET_TN_SETVAL`，然后
`hpet_writel(now + delta, CMP)` 再 `hpet_writel(delta, CMP)`——两次。

### 2. APIC 校准的计数器差值算反了

Local APIC 的 Current Count 寄存器是**递减**的（SDM 卷 3 §11.5.4），从 Initial Count 数到 0。
`Calibration::start()` 先读（大），`finish()` 后读（小），而返回的是

```cpp
return static_cast<u32>(calibration.apic_cycle_last - calibration.apic_cycle_first);
```

`last < first`，无符号相减再截断成 u32，得到的是 `2^32 - 真实经过的计数`。
Linux 写的是 `tapic = lapic_cal_t1 - lapic_cal_t2`，方向相反。

再往下：`apic_period = (best[1] - best[0]) / (periods[1] - periods[0])`，两个都是
`2^32 - e_i` 的话差值等于 `e0 - e1`，而 `e1 > e0`，u32 下又回绕一次。**`APIC_PERIOD` 是纯粹的
垃圾数**。

### 3. 支持 TSC-deadline 的机器上，校准的是一个没在走的计数器

`setup_timer(clocks, oneshot=true, masked=true)` 里：

```cpp
if (!oneshot) { enable_tsc(..., Periodic); }
else if (support_tsc_deadline) { enable_tsc(..., Deadline); }
```

校准用 `oneshot=true`，所以只要 CPU 报 `TscDeadlineTimer`（Sandy Bridge 之后基本都报），
定时器就被切进 TSC-deadline 模式。而**该模式下 Initial Count 寄存器被忽略、Current Count 恒为
0**，于是两次读都是 0，`elapsed = 0`，`APIC_PERIOD = 0`。第三十三节加的那个"没校准就不入队"
的守卫随即生效：**local APIC 定时器在现代硬件上根本不会入队**，每颗 CPU 都只能靠全局的
PIT/HPET，而全局设备一次只能打断一颗 CPU——SMP 上其余 CPU 没有 tick。

### 4. 校准期间从没写过分频寄存器，测出来的和记下来的不是同一个分频

`setup_timer()` 不碰 `TimerDivConf`，所以校准时用的是复位值 0，即**除以 2**。
而 `DIVISOR` 被记成 `apic_advisor`（初值 1，含义是除以 1）。设总线频率为 F：

- 实际测到 `F/(2·HZ)` 每 jiffy；
- `APIC_PERIOD = F/(2·HZ)`，`DIVISOR = 1`；
- `freq = APIC_PERIOD · HZ / DIVISOR = F/2`，而 `set_next_event_tick()` 按 `DIVISOR=1` 编程，
  设备真实速率是 F。

**入队频率比真实值小一倍，每个 delta 都长一倍。**（当然，第 2 条已经让这些数没意义了。）

### 5. `apic_advisor` 是个摆设，而它的重试分支既是死代码又会死循环

- `calibrate_apic_clock_single(u32 periods, u32 apic_advisor)` 的第二个参数**函数体里一次都没
  用**（`-Wno-unused-parameter` 挡掉了警告）。
- 外层的调整逻辑：

  ```cpp
  if (elapsed_ticks < best_time[trail]) { best_time[trail] = elapsed_ticks; }
  else if (best_time[trail] == INVALID_TICK_NUM) { i -= 1; apic_advisor <<= 1; }
  ```

  `best_time` 初值是 u32 最大值，所以第一次迭代几乎必然走 if 分支；`else if` 只有在
  `elapsed_ticks` 恰好等于 u32 最大值时才可能进入——实际是死代码。
- 万一进去了：`i -= 1` 写在 `for (i = 0; i < 2; ++i)` 里，`i` 在 0 和 −1 之间反复，而
  `apic_advisor` 的改变对测量毫无影响（第 5 条第一点），**无限循环**。
- `apic_advisor = u8(apic_advisor << 1)` 把 u32 截成 u8，八次翻倍后变 0；
  `DIVISOR = static_cast<u8>(apic_advisor)` 于是可能是 0。
- `apic_advisor` 跨 trial 保留，所以不同 trial 可能用不同分频，而它们的差值又被拿来算周期。

### 6. 测了四次，只用最不准的两次

`best_time[2]` 和 `best_time[3]`（periods 4 和 8）算出来之后再没被读过。
`apic_period` 只用了 index 0 和 1，也就是 10 ms 和 20 ms 两次——间隔最短、相对误差最大的一对。
用 index 0 和 3 的话基准长 8 倍，误差小 8 倍。数组元素不会被 `-Wunused` 抓到。

### 7. 测量没有对齐到 jiffy 边界

```cpp
auto const tick_start = ClockSource::Default::read();
while (read() - tick_start < periods) {}
```

`tick_start` 采样于当前 jiffy 中间的任意位置，所以真实经过时间在 `(periods-1, periods]` 个
jiffy 之间——最短那次测 10 ms，误差可以到整整 10 ms，**100%**。Linux 的
`calibrate_APIC_clock()` 先 `wait_for_a_tick()` 对齐到 jiffy 边界再开始。

用两次不同时长相减确实能消掉一个**常数**偏置，但相位误差是每次随机的，不是常数。

顺带：日志写的是 "for duration {} ms" 和 "ticks/ms"，而 `periods` 的单位是 jiffy，HZ=100 时
是 10/20/40/80 ms。

### 8. 自旋没有超时，结果没有校验

校准循环等 jiffies 前进，**没有任何退出条件**。第 1 条那种情况下就是静默挂死。
Linux 在这里有 `if (!lapic_cal_loops) ... "APIC frequency too slow, disabling apic timer"`，
并且事后用 TSC 交叉验证测量结果，偏差超过 10% 就拒绝。这里两样都没有。

### 9. `VTABLE` 在运行期被就地改写，而它是所有 CPU 共享的

```cpp
if (support_tsc_deadline) {
    VTABLE.set_next_event_tick = Packer::pack<&Self::set_next_event_tick_deadline>();
    VTABLE.shutdown = Packer::pack<&Self::shutdown_deadline>();
}
```

`VTABLE` 是 `inline static`，一份，所有 CPU 的 `ApicTimerEvent` 都指向它；而 `setup_timer()`
会被 `set_mode_periodic()` / `set_mode_oneshot()` 在任意 CPU、任意时刻调到。
**在别的 CPU 正通过它分发的时候改写函数指针，没有任何同步。** 而且这是个只该在 init 期做一次
的选择，却每次 `setup_timer()` 都重做一遍。正确的做法是启动时按能力选定一张表。

### 10. HPET 探测缺三项校验

- **没有验证计数器在走。** Linux 在注册 HPET 时钟源之前会前后读两次计数器，不动就拒绝。
  这里没有，而 HPET 目前是全树**唯一**的时钟源：一个坏的或不存在的 HPET 会得到一个读数恒定的
  时钟源，`Clock::now()` 永不前进，于是每个 deadline 都永远在未来。
- **没有校验 period 的范围。** 只查了 `period == 0`。Linux 拒绝
  `period < 100000` 或 `period > 100000000` 飞秒（即频率不在 10 MHz–10 GHz）。固件填个荒唐值
  就会算出荒唐的 `HPET_FREQUENCY`，而它现在是时钟源和事件设备两边的换算基准。
- **`num_channels < 2` 就拒绝，但代码只用 channel 0。** 这个 2 没有出处也没有注释。

### 11. 其余（不影响正确性，或影响面较小）

- **PIT 完全没有探测**：`setup_pit()` 直接 `i8253_init(false)` 并假定它在。没有 PIT 的机器上
  这些 outb 落空，tick 永不到来，然后撞上第 8 条的无超时自旋。`i8253_init(bool oneshot)` 的参数
  从头到尾没被用过。
- **32 位 HPET 被直接拒绝**（`support_64bit()` 不满足就返回 `Unsupported`）。Linux 支持，用回绕
  处理。在这类芯片组上结果是全树没有任何时钟源。
- **HPET 抢走 IRQ0 之后 PIT 没有停**，只是输出被断开。Linux 会显式关掉它。
- **`hpet_init_internal()` 在框架之外手工编程了设备，随后又把它入队**，同一个设备有两个主人；
  上面第 1 条的周期编程就卡在这个缝里——框架那条路（`set_mode_periodic()`）什么都不编程，
  手工那条路编程得不完整。
- `apic_mask_irq(apic_io_isa_to_global(0))` 用的是字面量 0，现在有 `ISA_IRQ_PIT` 常量了。
- `HPET_LEGACY_IRQ_ENABLED` 只写不读（挂着 `[[maybe_unused]]`）；`arch/hpet.hpp` 的
  `HPET_CLKEVT` 是个永远为空的 Span。
- **`Calibration` 采集了 TSC 却扔掉。** `tsc_cycle_first/last` 记了但从不使用。这恰恰是第三十三节
  那个 TSC-deadline 频率错配的 TODO 所缺的数据——同一个测量窗口里的 TSC 差值除以经过时间就是
  TSC 频率。数据已经在手上了，只差用它。
- 全树没有 invariant-TSC 检测，也没有把 TSC 作为时钟源。

### 修复建议的顺序

1. 第 1 条（HPET 周期编程）——这是启动挂死，别的都在它后面。顺带把周期编程收回到
   `HpetChannel::set_mode_periodic()` 里，让框架那条路成为唯一的路。
2. 第 8 条（自旋加超时和失败路径）——在动校准算法之前先让失败可见，否则每次试错都是挂死。
3. 第 2、3、4 条（差值方向、TSC-deadline 下改用可测模式、校准期间显式设分频）——这三条一起
   决定 `APIC_PERIOD` 是否有意义。
4. 第 7、6 条（对齐 jiffy 边界、用最长的一对）——精度。
5. 第 5 条（删掉 `apic_advisor` 那套死逻辑）与第 9 条（vtable 只在 init 期选一次）。
6. 第 10 条（HPET 的三项校验），第 11 条里的杂项。
7. 用第 11 条最后那点已经采集的 TSC 数据算出 TSC 频率，解掉第三十三节的 TODO。

## 三十六、按第三十五节修复 x86/pc 的时钟探测与校准（2026-09-03）

分五小步做的，每步单独构建验证。（`local_apic.cpp:608` 那条 `use nullptr` 是既有的
`map_at(..., 0, ...)`，不在改动行上。）

### 1. HPET 周期编程（第三十五节第 1 条，启动挂死）

周期编程从 `hpet_init_internal()` 手写的那一处**搬进** `HpetChannel::set_mode_periodic()`，
并补齐成两次比较器写入：第一次是首次触发的绝对计数（`main_counter + period`），第二次才是周期
累加器。以前只有一次，累加器留在 0，而 HPET 是相等比较，于是响一次就再也不响。

`hpet_init_internal()` 现在只做设备级的事——挂 legacy route、清零并启动计数器；通道 0 的编程
交给框架，经 `ClockEventDevice::enroll()` → `TickDevice` → `switch_mode(Periodic)` 走到
`set_mode_periodic()`。一个设备只剩一个主人。

顺带给 HPET 补了 `shutdown()`（掩掉中断）并填进 vtable——`rebind_device()` 要 detach 旧设备，
没有它 HPET 会继续在一条 handler 已经搬走的线上打中断。`set_mode_oneshot()` 也补了 unmask。

**第三十五节第 11 条里"HPET 抢走 IRQ0 后 PIT 没停"这一条已经不成立**：第三十节重写
`rebind_device()` 之后，换绑会对旧设备 `switch_mode(Detached)` → `shutdown()`，PIT 是被停掉的。

### 2. jiffies 去掉锁（原第二十六节 C11，之前一直没做）

`ClockSource::Default::read()` 为读一个字拿 `Guard<SpinLock, IrqSave>`，而 IrqSave 是空操作
（第三十二节）。于是 tick 中断可以落在 `read()` 持锁的那几条指令里，`advance_ticks()` 随即在
**同一颗 CPU 上**自旋等待被自己中断的代码释放锁——自死锁。窗口很窄，但 APIC 校准正是在几十毫秒
里紧密轮询 `read()`，这是找出这种窗口最有效的方式。

`TICKS` 改成 `std::atomic<Ticks>`，relaxed 读写，锁删掉。Linux 的 jiffies 同样是一次普通的
volatile 读。只有 `CPUNUM` 那颗 CPU 写，所以三个字段之间的一致性不需要锁。

### 3. 校准重写（第三十五节第 2–9 条）

| 问题 | 改法 |
|---|---|
| 计数差算反（计数器递减，代码算 `last - first`） | `Calibration::apic_elapsed()` = `first - last`，TSC 用 `last - first`，各自注释说明方向 |
| TSC-deadline 机器上测的是停着的计数器 | 校准不再走 `setup_timer()`，自己置 `TscMode::OneShot` 并显式写分频 |
| 校准期间从没写过分频寄存器 | `apic_timer_set_divisor()` 提到匿名命名空间，校准和 `set_next_event_tick()` 共用；校准固定用除数 1 |
| `apic_advisor` 参数没被用、重试分支是死代码且会死循环、u8 截断 | 整套删掉 |
| 四次测量只用最不准的两对 | 只测两次，取最短（1 tick）和最长（8 tick），差值除以跨度 |
| 没对齐 jiffy 边界 | `calibrate_once()` 先 `wait_for_jiffies(edge, 1)` 落到边界，再写初值计数启动窗口 |
| 自旋没有超时 | `wait_for_jiffies()` 有 10 亿次迭代上限；超了返回 false，`apic_calibrate_clock()` 报错返回 false，`init_bsp_apic_timer()` 就不入队 |
| 结果没有校验 | 长窗口的两个计数都必须大于短窗口，商都不能为 0，否则拒绝 |
| `VTABLE` 被运行期无同步改写 | 新增 `select_timer_mode()`，只在 BSP 校准末尾调一次，那时没有 AP 在跑、也没有设备入队 |

**顺带解掉了第三十三节的 TSC-deadline TODO**：`Calibration` 本来就同时采样 TSC，只是把它扔了。
现在记为 `TSC_PERIOD`（每 tick 的 TSC 周期数），`init_apic_timer()` 按选中的模式入队对应的频率
——deadline 模式下设备的"tick"就是 TSC 周期。

命名上：`DIVISOR`（存的是除数值）改成 `DIVISOR_LOG2`，因为编码表就是按 log2 索引的，而且每次调整
都是翻倍。`APIC_PERIOD` 的定义改成"**已分频**时钟每 tick 的计数"，于是设备频率就是
`APIC_PERIOD * HZ`，不再需要在入队时又除一次分频。

### 4. HPET 的三项校验（第三十五节第 10 条）

- 新增 `hpet_counter_is_running()`：启用计数器后确认它真的在走。HPET 目前是全树唯一的时钟源，
  一个停着的计数器会让 `SteadyClock::now()` 恒定，于是每个 deadline 永远在未来。
- period 按 Linux 的范围校验（1e5–1e8 飞秒，即 10 MHz–10 GHz），原来只查了 0。
- `num_channels < 2` 放宽成 `< 1` 并说明理由：只用 channel 0，那个 2 没有出处，一个编造的门槛
  可能拒掉能用的设备。

### 5. 杂项

- `i8253_init(bool oneshot)` 的参数从头到尾没被读过——模式是 `TickDevice` 通过 `switch_mode()`
  决定的。参数删掉，头文件里写明原因。
- `apic_mask_irq(apic_io_isa_to_global(0))` 的字面量换成 `ISA_IRQ_PIT`。

### 仍未做

- **PIT 存在性探测**。Linux 的 `timer_irq_works()` 会验证定时器中断确实在推 jiffies，不行就换路。
  现在没有这个检查；不过第 3 步的超时至少让"tick 没起来"变成一条错误日志而不是静默挂死。
- 32 位 HPET 仍然直接拒绝（Linux 用回绕处理支持它）。
- `HPET_LEGACY_IRQ_ENABLED` 只写不读；`arch/hpet.hpp` 的 `HPET_CLKEVT` 仍是空 Span。
- **`ClockEventDevice::Vtable` 的手工打包方式待重构**，已另行提出设计方案。

## 三十八、在 QEMU 上实测：tick 到底有没有正确产生（2026-09-03）

用 `scripts/qemu-boot.sh` 实际启动（q35 / Nehalem / 1 CPU / 512M，`KERNEL_DEBUG_LEVEL=0`，
所以 `DEBUG_ASSERT` 全部编译掉了）。

### 结论

**时间子系统本身是对的，周期 tick 以正确频率在跑。** 但**调度器还收不到 tick**，原因不在
kernel/time：启动在 `task::init()` 里就崩了，比 tick 被切到 Legacy 模式还早。

### 时间侧的实测证据

```
[info] HPET Frequency = 100000000
[info] switched to clock-source HPET-CLKSRC (100000000 Hz, rating 250)
[info] APIC timer: 10593142 counts/tick at divisor 1, TSC 10593142 cycles/tick
[info] CPU[0] keeps tick device HPET over LVT
```

1. **HPET 通过了新加的 period 范围校验**（QEMU 的 HPET 是 10 ns 周期 = 100 MHz），
   计数器在走的检查也过了，时钟源入队并被 `SteadyClock` 采纳。
2. **`APIC timer: ...` 这一行本身就是周期 tick 工作的证明。** 校准要等 1（对齐边界）+ 1 +
   1（对齐边界）+ 8 = 11 个 tick 才能完成，而在第三十六节之前它会永远等下去——HPET 接管后
   只响一次。现在它跑完了，说明 `set_mode_periodic()` 的两次比较器写入是对的、中断在
   `handle_periodic_event()` 里正常推进 jiffies。
3. **频率对得上。** 日志时间戳是 TSC（`logz4/kernel.cpp:read_time`）。上面两行之间是
   114,380,000 个 TSC 周期，除以实测的 10,593,142 TSC/tick = **10.8 个 tick**，
   而校准实际要等 11 个。误差 2%，来自两条日志行本身的位置。也就是说**一个独立的计数器
   （TSC）证实了 jiffies 是以预期速率前进的**。
4. `CPU[0] keeps tick device HPET over LVT`：单核机器上 `cpu_possible_mask()` 就是 {0}，
   和 LAPIC 的 per-CPU mask 相等，于是走到比 rating，HPET 的 120 大于 LAPIC 的 100，HPET 留任。
   在单核上这没问题（只有一颗 CPU，全局设备够用）；多核上 mask 不同，per-CPU 规则会让每颗
   CPU 换到自己的 LAPIC。这是第三十节把 HPET rating 抬到 120 的副作用，行为正确但值得记着。

### 崩在哪（不在 kernel/time）

```
Unhandled page fault: vector=14, error_code=0x11    // present + 取指 → 跳到了空指针
rip=0x0  rax=0x0  rdi=0x0  rsi=0xffffff8800012498   // rsi 是 per-CPU 地址
```

没有调试器，所以临时在 `panic_exception()` 里把栈上几个字打了出来（已还原），
用 `llvm-nm` 对着**实际启动的**二进制符号化——注意 `kernel.dev.obi` 装的是
`vmours.with-tests`（入口 0x2f08b），不是 `vmours`：

```
stack[0] → Scheduler::on_thread_init(Thread*, BaseProfile const&) +0x28
stack[4] → IdleThread::init(CpuNum)::$_1::__invoke()
stack[6] → Thread::spawn_etc(...) +0x15c
```

即 `task::init()` → `init_idle_task_percpu()` → `Thread::spawn_etc()` →
`Scheduler::on_thread_init()`，崩在 `scheduler.cpp:120`：

```cpp
context.schedule()->on_thread_init(self.m_runtime, thread);
```

`context.schedule()` 是空的（`rdi=0`，而 `rsi` 正是 per-CPU 的 `m_runtime`）。
`Scheduler::m_schedules[]` 由 `Scheduler::init()` 从 `SCHEDULES_START/END` 这个 linker
section 填充，而 `fair.cpp:490` 那条注释早就写着「linker collects nothing, and
Scheduler::init() finds an empty section」。**这是调度器侧的问题，不是时间侧的。**

顺带看到的另一条：`Scheduler::init()` 末尾写 `m_runtime.which_cpu = Cpu::id()`，
而 `task::init()` 是从 BSP 上替每颗 CPU 调它的——于是每个 scheduler 都认为自己在 CPU 0。

### 因此对"能否为调度器提供正确的 tick"的回答

链路上从硬件到 `TickSched::on_tick()` 全部走通并且频率正确；`on_tick()` 里那三步
（`Scheduler::Current::tick()`、profiler、`TimerQueue::run_expired_timers()`）目前还到不了，
因为 `TickSched` 要在 `InitLevel::Threading` 才被切到 `Legacy`，而启动在 `task::init()` 中途
就崩了。把上面那个空的 schedule 修好之后就能直接观察到。

## 三十九、怎么验证 tick 与 time 的切换、以及测出来的频率是对的（2026-09-03）

你问的两件事其实是同一个方法：**拿一个独立的时钟做交叉验证**。Linux
`calibrate_APIC_clock()` 就是这么做的——它用 TSC 校验 APIC 的结果，偏差超过 10% 就告警。
我们原来一个校验都没有，只是把测出来的数直接用掉。

### 加的检查

`Calibration` 现在在窗口两端多采一次 `SteadyClock::now()`（即 HPET，和 APIC 计数器、TSC 都不是
同一块硬件、不是同一个振荡器）。于是校准末尾可以问一个此前问不出来的问题：

```
tick period: N ns measured by the clock source, 10000000 ns expected
```

这一行同时验证三件事：**tick 设备是不是真的每 1/HZ 响一次**、**时钟源的 tick→ns 换算对不对**、
以及**由这个窗口导出的两个频率是否有意义**。

阈值分两级，因为该抓的错和该容忍的噪声差一个数量级。tick 设备能出的错都是**倍数级**的：
PIT 的重载值留在 65536 会让 tick 长 5.5 倍，重复计数会让它减半，设备停了则是超时。
而测量噪声只有几个百分点——TCG 下虚拟时钟跟着宿主调度走，两次启动之间就差了 7%。
所以 **10% 告警**（Linux 告警的那个数），**2 倍才拒绝**。

### 实测结果

两次启动（单核、双核），都在 QEMU/TCG 上：

| 运行 | 测得 tick 周期 | APIC 计数/tick | 推出的 APIC 频率 | 推出的 TSC 频率 |
|---|---|---|---|---|
| 1 CPU | 9,866,714 ns | 9,866,714 | 1.0000000 GHz | 1.0000145 GHz |
| 2 CPU | 10,100,857 ns | 10,101,000 | 1.0000142 GHz | 1.0000142 GHz |

对 10,000,000 ns 的偏差分别是 **1.33%** 和 **1.01%**。

**这张表就是答案。** 两次运行的**原始计数**相差 2.37%（TCG 抖动），但它们**推出的频率**只差
**14 ppm**，而且落在 1.0000 GHz 这个整数上——正是 QEMU 对 APIC 总线和 TSC 建模的速率。
计数和时长同步变化、导出的物理量稳定且是个整数，这才是"测量是对的"的证据；只看
「10593142 counts/tick」这个数本身，是没法判断对错的。

### 切换的验证

**tick 设备切换（PIT → HPET）**：同一行就是证据，但要说清楚推理。jiffies 只由当前绑定的那个
设备推进（`handle_periodic_event` 在 `CPUNUM` 上）。上面测的是「HPET 计数器看到的每 jiffy 多少
纳秒」，而 jiffies 是被**换绑之后的** HPET 推的。9.87 / 10.10 ms 对 10.00 ms 说明换绑既没丢
tick 也没多算：丢了会超时，多算一倍会得到 5 ms，PIT 的 latch bug 会得到 54.9 ms。1% 的偏差
只能是测量噪声。

**per-CPU 设备选择（HPET vs LAPIC）**：单核和双核跑出的差别直接验证了第三十节的规则。

```
1 CPU: CPU[0] keeps tick device HPET over LVT     ← 被拒
2 CPU: （没有这一行）                              ← LAPIC 被接受
```

单核上 `cpu_possible_mask()` 就是 {0}，和 LAPIC 的 per-CPU mask 相等，于是落到比 rating，
HPET 的 120 赢。双核上两者 mask 不同，`check_preferred()` 的「mask 不同则优先」分支生效，
LAPIC 被接受。**这正是设计意图**，而且是一个可以随时复现的判别实验。

**时钟源切换（无 → HPET）**：连续性是构造保证的——`Clock::override_source()` 在 seqlock 的写
临界区里先用旧锚点算出此刻的时间，再把它钉到新 source 的当前计数上，`now()` 不可能跳变。
可观察的旁证是：切换之后紧接着做的这次交叉验证给出了 9.87 ms/tick 这个合理值，说明换源之后
的时钟是能用且换算正确的。

### 还没被验证到的

- **LAPIC 作为 tick 设备实际发中断**：双核下它被接受了，但启动随后就崩在调度器里
  （第三十八节），没跑到能观察它产生 tick 的地方。
- **oneshot 路径和 `TimerQueue`**：目前没有任何 timer 被武装。
- **两个真实时钟源之间的切换**：全程只有 HPET 一个源入队过。jiffies 作为第二个源
  （`Default::init()`）还没做，做完之后就能观察到一次真实的换源。
- 日志时间戳是 TSC（`logz4/kernel.cpp:read_time`），不是 `SteadyClock`，所以不能直接拿来
  当时间正确性的证据。

---


## 频率改用比值法，窗口拉长到 200 ms（2026-09-03）

**文件**
- `kernel/arch/x86/apic/local_apic.cpp` — `Calibration` 新增 `hz_of()` 与
  `counter_ran_out()`；`apic_calibrate_clock()` 改用比值法、窗口 1→20 个 tick；
  `APIC_PERIOD`/`TSC_PERIOD` 改名为 `APIC_HZ`/`TSC_HZ`（前者保留为派生值）；
  `init_apic_timer()` 直接用频率入队。

**症状**
潜伏，当前触发不到——精度不够但还没有使用者在意。可观察的表现是**不可重复**：
同一台 QEMU 两次启动算出的 APIC 频率差 2.37%（986,671,400 与 1,010,100,000 Hz）。

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
  自己约掉，必须在两个不同长度的窗口之间抵消。
- 窗口 70 ms → 200 ms（跨度 7 → 19 个 tick），误差降到约三分之一，启动多花 130 毫秒。
- 拉长窗口带出一个新风险：定时器是 one-shot、从 `0xFFFFFFFF` 往下数、**数到 0 就停**，
  窗口超过量程时"经过的计数"变成上限而不是测量值。唯一可观察的症状是末值为 0，
  故加 `counter_ran_out()`，为真则拒绝校准。
- 命名跟着语义改：被测量的是频率（`APIC_HZ`），每 tick 计数成了派生值，
  只给 `setup_timer()` 装初值用。
- 没有时钟源时回落到旧的数 tick 法并 `log::warn`——没有 HPET 的机器上没有独立的尺子。

**参照**
`Linux calibrate_APIC_clock()`：窗口用 `HZ/5`，也是 200 ms；它同样拿 TSC 交叉验证 APIC
的结果。不同之处是 Linux 仍以「每个校准循环多少计数」为主结果，我们直接取比值。

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

构建通过，`clang-tidy` 改动行干净。

**故意没做**
重复测量取中位数、线性回归代替两点相减、运行期持续修正。理由和标准误公式记在
`docs/repairs/260903/x86-clock.md` 第 4 节。现在的精度（ppm 级）已经远超使用需求——
定时器最小 delta 是 0xF 个 tick。
