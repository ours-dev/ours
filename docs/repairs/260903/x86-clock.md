# x86 时钟探测与校准 — 待修

历史与已修部分见 `docs/repairs-20260830.md` 第三十五、三十六、三十九节。

现状：PIT 的 latch、HPET 的周期编程与错过检测、LAPIC 的除数编码都已修。校准改用
「同一时刻读两个计数器再相除」的比值法、窗口 200 ms，QEMU 上两次启动给出**逐位相同**的
1,000,000,000 Hz（旧的 jiffy 法两次差 2.37%）；剩余噪声只体现在 tick 周期本身（偏离 10 ms
约 0.4%–1%），那是 tick 设备的真实抖动，本该留在那里。

---

## 1. PIT 没有存在性探测

`setup_pit()`（`kernel/arch/x86/kernel/time/time.cpp`）直接 `i8253_init()` 并假定 PIT 在。
没有 PIT 的机器上这些 `outb` 落空，tick 永不到来。

现在不会静默挂死了——`wait_for_jiffies()` 有迭代上限，校准会报错返回——但仍然没有 Linux
`timer_irq_works()` 那种"验证定时器中断确实在推 jiffies，不行就换一条路"的机制。

## 2. 32 位 HPET 被直接拒绝

`hpet_init_internal()` 要求 `general_caps.support_64bit()`，否则返回 `Unsupported`。
Linux 支持 32 位 HPET，用回绕处理。在这类芯片组上目前的结果是**全树没有任何时钟源**
（jiffies 还没入队，见 `kernel-time.md` 第 2 条），`SteadyClock::now()` 恒为 0。

## 3. TSC 没有被当作时钟源，也没有 invariant-TSC 检测

`arch::Tick::get().tsc` 被校准和 TSC-deadline 模式读取，但：

- 全树没有 invariant/constant TSC 的检测（`grep tsc_freq|tsc_khz|calibrate_tsc` 全空）。
- TSC 没有作为 `ClockSource` 入队。Linux 给它 rating 300，高于 HPET 的 250，因为读它便宜得多
  （一条 `rdtsc` vs 一次 MMIO）。
- 校准已经算出了 `ApicTimerEvent::TSC_PERIOD`（每 tick 的 TSC 周期数），所以入队所需的频率
  **数据已经有了**，缺的是 invariant 检测和一个 `ClockSource` 包装。

## 4. 校准精度还能再进一步（都不急）

比值法之后误差量级已经是 ppm，远超使用需求（定时器最小 delta 是 0xF 个 tick），所以下面三条
都不急，按性价比排：

- **重复测量取中位数。** TCG 下的噪声是离群型的——宿主调度打个嗝就有一个窗口异常地长。
  做 3 次取中位数是便宜的保险。注意噪声方向：对「每 tick 计数」这类量，采样延迟只会让窗口变长，
  所以取**最小值**比平均好（Linux 的 `best_time` 就是这个意思）；但比值法的误差是双边小抖动，
  中位数更合适。
- **线性回归代替两点相减。** 在每个 jiffy 边沿采一对 `(mono_ns, count)`，对 `count = a + f·t`
  做最小二乘，常数偏置被截距吸收，比两点相减更彻底。斜率标准误是
  `σ / sqrt(Σ(t_i - t̄)²)`，K 个点均匀铺在跨度 T 上时约为 `σ·sqrt(12)/(T·sqrt(K))`——
  **跨度比样本数重要得多**，把跨度翻倍等于把样本数翻四倍，所以"长跨度少点"优于"短跨度密采"。
- **运行期持续修正。** Linux 对 TSC 就是这么做的：启动时粗校，起来后用
  `tsc_refine_calibration_work` 对着参考时钟源重校一秒，之后 `clocksource_watchdog` 持续盯。
  我们有天然落点——每个 tick 本来就要进 `TickSched::on_tick()`，在那里采一对样、更新一个滑动
  最小二乘即可。这条同时也是第 3 条（TSC 作为时钟源）的前提。

另外，六个原始采样和两个窗口各自的差值目前只在**失败路径**打印，成功时看不到，所以还没法审查
短窗口的固定开销占比是否合理。加三行日志的事。

## 5. `HpetClockSource::m_cache` 不是原子的

`hpet.cpp` 的 `HpetClockSource::read()` 用一把自旋锁把并发读者分成两路：拿到锁的去读 MMIO
并把值写进 `m_cache`，没拿到的在 `m_cache` 上自旋等它变化。

问题是 `m_cache` 是个普通的 `usize`：持锁方裸写、竞争方在**别的 CPU 上**裸读，
还靠 `while (oldvalue == newvalue && m_spinlock.is_locked())` 这个循环观察它——
编译器完全可以把这个读提到循环外。改成 `std::atomic<u64>`，
读写用 relaxed 就够（它不携带别的数据）。

Linux 的 `read_hpet()` 用 `union hpet_lock` 的 cmpxchg 做同一件事。
（审查编号 C12，八步计划里漏了。）

## 6. LAPIC 作为 tick 设备没有被实测验证

双核 QEMU 下 `check_new_device()` 确实接受了 LAPIC（单核下按 rating 保留 HPET，符合设计），
但启动随后崩在调度器里（见 `scheduler.md`），没跑到能观察 LAPIC 产生 tick 的地方。
解掉那个阻塞之后应当复跑 `SMP=2` 确认。

## 7. 杂项

- `HPET_LEGACY_IRQ_ENABLED`（`hpet.cpp`）只写不读，挂着 `[[maybe_unused]]`。
- `kernel/arch/x86/kernel/include/ours/arch/hpet.hpp` 的 `HPET_CLKEVT` 是个
  `inline` 定义、永远为空的 Span；同文件的 `is_hpet_enabled()` / `hpet_enable()` /
  `hpet_disable()` 只有声明。
- `arch/time.hpp` 的 `time::hpet_init()` / `pit_init()` 同样是悬空声明。
