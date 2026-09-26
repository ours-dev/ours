# arch/x86 apic 改动记录（2026-09-26）

## LAPIC 定时器中断按当前 CPU 取设备，删掉全局 `LAPIC_TIMER_EVENT`（2026-09-26）

**文件**
- `kernel/arch/x86/apic/local_apic.cpp` — `apic_handle_timer_irq()` 改为按当前 CPU 取 `CLKDEV_STORAGE`；删掉全局指针
  `LAPIC_TIMER_EVENT`；`init_apic_timer()` 直接 enroll 本 CPU 的 `self.m_clkevt`
- `docs/repairs/260926/x86-apic.md` — 唯一一条待修项修掉，文件删除

**症状**
多核下每个 CPU 的 tick 都只有 `HZ / N`。4 核 QEMU、`HZ = 100` 实测每个 CPU 每 2 s 只有 50 个 tick（25 Hz），
四个 CPU 加起来恰好是一个 CPU 该有的 200 个——整台机器只有一份 tick，被 N 个 CPU 轮流分掉。jiffies 由 BSP
推进，所以它也慢了 N 倍；`TimerQueue` 的到期检查同样慢 N 倍。没有任何断言会报，只表现为时间走慢、超时变长。
debug 镜像里 BSP 分到的份额更不均，某些 2 s 窗口里只有 5 个 tick。

**原因**
修改前：

```cpp
CPU_LOCAL
static ApicTimerEvent CLKDEV_STORAGE;

static ClockEventDevice *LAPIC_TIMER_EVENT;

auto apic_handle_timer_irq() -> void {
    DEBUG_ASSERT(LAPIC_TIMER_EVENT);

    apic_issue_eoi();
    LAPIC_TIMER_EVENT->handle_event();
}

// init_apic_timer()，每个 CPU 各跑一次：
    auto &self = CpuLocal::access(CLKDEV_STORAGE);
    self.m_cpumask = CpuMask::of(Cpu::id());
    self.m_clkevt.set_cpumask(&self.m_cpumask);
    LAPIC_TIMER_EVENT = &self.m_clkevt;
    ...
    ClockEventDevice::enroll(*LAPIC_TIMER_EVENT, usize(freq), 0xF, 0x7FFFFFFF);
```

设备本身是 per-CPU 的（`CLKDEV_STORAGE` 带 `CPU_LOCAL`），指向它的指针却是一个普通全局。每个 CPU 在
`init_apic_timer()` 里把它改成指向自己那份，BSP 先写、AP 后写，bring-up 结束后它指向最后一个 AP 的设备。
而 `ApicTimer` 向量是所有 CPU 共用的入口：每个 CPU 的 LAPIC 定时器都只打断自己，打进来却都去处理同一个设备。

光是"处理了别人的设备"为什么会让频率掉成 `HZ / N`，要看 tick 是怎么周期起来的。LAPIC 设备只声明
`Feature::Oneshot`，`TickDevice::setup_periodic()` 就用 oneshot 模拟周期：`handle_periodic_event()` 每次触发后读
**设备自己的** `next_event()`，加一个周期再 `program_event()`。截止时间存在设备对象里，编程却经 `XAPIC_STORAGE`
写进**当前 CPU** 的 LAPIC 寄存器（LAPIC 的 MMIO/MSR 在硬件上就是按 CPU 分开的）。于是：

- CPU0 触发，读共享设备的截止时间 X = t，写回 X = t+P，给 CPU0 的 LAPIC 定在 t+P；
- CPU1 在 t+P 附近触发，读到 X = t+P，写回 X = t+2P，给 CPU1 定在 t+2P；
- ……第 N 个 CPU 把 X 推到 t+NP；CPU0 在 t+P 再触发时读到的已经是 t+NP，于是把自己定到 t+(N+1)P。

每个 CPU 下一次触发都排在 N 个周期之后。N 个 CPU 合起来推进的是**一个**截止时间，每个周期只推一步，所以全机总共
`HZ` 个 tick，每个 CPU 分到 `HZ / N`。这也解释了实测里"四个加起来正好 200"。

bring-up 窗口里还有两件事更糟，这次一并消失：
- AP 在写全局指针之后、`setup_periodic()` 装好处理函数之前，要走完 GCED 的 Mutex、configure、rebind 一整段。这段时间
  BSP 或更早的 AP 来一个 LAPIC 中断，处理的是这个半成品设备：`m_event_handler` 为空（什么都不做），或者设备还没进
  Oneshot（`handle_periodic_event()` 直接返回）。两种情况下触发中断的那个 CPU 的 oneshot LAPIC 都没人再编程，
  它的 tick 就永久停了。本次实测没有撞上，但窗口是真实存在的。
- AP 写 `std::function` 类型的 `m_event_handler` 时，别的 CPU 正在中断里读同一个对象，是一个没有同步的跨 CPU 数据竞争。

**改法**
修改后：

```cpp
CPU_LOCAL
static ApicTimerEvent CLKDEV_STORAGE;

auto apic_handle_timer_irq() -> void {
    auto &self = CpuLocal::access(CLKDEV_STORAGE);
    DEBUG_ASSERT(self.m_cpumask.test(Cpu::id()),
                 "APIC timer interrupt on CPU[{}] before its device was bound", Cpu::id());

    apic_issue_eoi();
    self.m_clkevt.handle_event();
}

// init_apic_timer()：
    auto &self = CpuLocal::access(CLKDEV_STORAGE);
    self.m_cpumask = CpuMask::of(Cpu::id());
    self.m_clkevt.set_cpumask(&self.m_cpumask);
    ...
    ClockEventDevice::enroll(self.m_clkevt, usize(freq), 0xF, 0x7FFFFFFF);
```

- 中断里用 `CpuLocal::access()` 取当前 CPU 那份，不需要 `CpuLocal::Pinned`：中断上下文里中断是关着的，读 per-CPU
  偏移和使用它指向的对象之间不可能被迁移。
- 原来的 `DEBUG_ASSERT(LAPIC_TIMER_EVENT)` 是"至少有一个 CPU 初始化过"，对 per-CPU 设备已经没有意义（对象总在那里）。
  换成它的 per-CPU 版本：本 CPU 的设备已经被 `init_apic_timer()` 绑定到本 CPU。模板里的 `m_cpumask` 是空的，没经过
  `init_apic_timer()` 的副本会断言失败。正常路径上它不会失败：LVT timer 复位后是屏蔽的，`init_apic_timer()` 设置完也保持屏蔽，
  这个向量只能在绑定之后到来。
- `init_apic_timer()` 本来就拿着 `self`，直接 enroll `self.m_clkevt`，不再绕全局。

被否掉的方案：把 `LAPIC_TIMER_EVENT` 加上 `CPU_LOCAL`。能修好，但它只是 `&CLKDEV_STORAGE.m_clkevt` 的一份冗余缓存，
多一个"设备和指向设备的指针"必须同步的不变量，还是在中断里多一次解引用；直接从 `CLKDEV_STORAGE` 取就没有这个问题。

**参照**
Linux `local_apic_timer_interrupt()`（`arch/x86/kernel/apic/apic.c`）：
`struct clock_event_device *evt = this_cpu_ptr(&lapic_events);`，设备 `lapic_events` 是
`DEFINE_PER_CPU`，中断里按当前 CPU 取，没有任何全局指针。`setup_APIC_timer()` 同样是
`struct clock_event_device *levt = this_cpu_ptr(&lapic_events);` 再 `clockevents_config_and_register(levt, ...)`，
对应这里 `init_apic_timer()` 直接 enroll `self.m_clkevt`。
Linux 在 `local_apic_timer_interrupt()` 里对 `evt->event_handler` 为空的情况会 `pr_warn` 并 `lapic_timer_shutdown()`；
这里 `ClockEventDevice::handle_event()` 对空处理函数是静默跳过，没有跟进。

**验证**
- `clang-format` 无改动；`clang-tidy -p out/bringup.x86` 在改动行上无新告警（364 行 `CLKDEV_STORAGE` 的命名告警是既有的）。
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过。
- QEMU（`-smp 4`，Nehalem，无 TSC-deadline，走计数模式；`HZ = 100`）启动 `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi`，各跑约 50 s，
  两个镜像都进入 shell、跑完用户态测试，没有 panic / 断言；dbg_1 的 54 条 warn 与修改前逐条同类（用户态 #DE 测试的寄存器转储）。
- 每 CPU tick 频率：临时在 `TickDevice::handle_periodic_event()` 里加 per-CPU 计数，每 2 s 打印一次（打点没有提交）。
  取稳定后的窗口：

  | 镜像 | 修改前 cpu0 / cpu1 / cpu2 / cpu3（每 2 s） | 修改后 |
  |---|---|---|
  | `kernel.dev.obi` | 50 / 50 / 50 / 50（25 Hz，四个合计 ≈ 200） | 200 / 200 / 200 / 200（100 Hz） |
  | `kernel.dev.dbg_1.obi` | 5–50 / 49–65 / 49–65 / 49–65（四个合计 183–206） | cpu0 140–227（均值 193），AP 198–204 |

  debug 镜像里 BSP 的抖动高低交替、均值接近 200，像是长时间关中断丢了 tick、再由 `program_event()` 把过期截止时间
  钳到最小间隔逐个补回（推测，未查证）；修改前 BSP 同样是四个里最少的一个，不是本项引入的。
- 去掉打点后重新构建，两个镜像再各启动一次，结果同上（无 panic / 断言）。
- bring-up 窗口里 tick 永久停掉的情况未实测复现（本次几次启动都没撞上）；修改后它在结构上不再可能，因为每个 CPU 只碰自己的设备。

**故意没做**
- `ClockEventDevice::handle_event()` 对空处理函数不像 Linux 那样告警并关掉定时器——那是 `kernel/time` 的行为，
  而且修改后本 CPU 的 LAPIC 中断只可能在本 CPU 设备装好之后才被打开，不在本项范围。
- debug 镜像里 BSP tick 抖动没有追。它在修改前就存在，与 LAPIC 设备取哪一份无关。
- `CLKDEV_STORAGE`、`XAPIC_STORAGE` 的命名告警（`readability-identifier-naming`）没动，与本项无关。
