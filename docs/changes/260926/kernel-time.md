# kernel/time 改动记录（2026-09-26）

## 删除无人调用的 `calculate_mult_shift()`（2026-09-26）

**文件**
- `kernel/include/ours/time/mod.hpp` — 删掉 `calculate_mult_shift()` 的声明
- `kernel/kernel/time/mod.cpp` — 删除；它只有这一个函数
- `kernel/kernel/time/BUILD.gn` — `sources` 去掉 `mod.cpp`

**症状**
无运行时症状。`calculate_mult_shift()` 在全树没有调用者；`260903/kernel-time.md` 已经记过"留着没动（仍无人调用）"。

**原因**
修改前：

```cpp
// kernel/include/ours/time/mod.hpp
auto current_mono_time() -> Time;

auto calculate_mult_shift(usize *mult, usize *shift, usize from, usize to, usize maxsec) -> void;

// kernel/kernel/time/mod.cpp
auto calculate_mult_shift(usize *mult, usize *shift, usize from, usize to, usize maxsec) -> void {
    ...
    usize s = 32;
    for (; s > 0; --s) {
        m = ((t << s) + (f >> 1)) / f; // See (1)
        if ((m >> mbits) == 0) {
            break;
        }
    }

    *mult = m;
    *shift = s;
}
```

它是 Linux `clocks_calc_mult_shift()` 的移植：挑一个 `(mult, shift)`，让 `(count * mult) >> shift` 近似
`count * to / from`。这套办法的前提是 `count * mult` 装得进 64 位，也就是 delta 有界——Linux 靠每个 tick 一次的
`timekeeping_update()` 推进 `cycle_last` 来保证这一点，`maxsec` 参数就是这个上界。

本内核在 0830 把 `Clock::now()` 改成了纯函数（`anchor.time + source->ticks_to_time(read() - anchor.ticks)`，
读路径不写任何东西），delta 就变成"自上次换 source 以来的全部 ticks"，没有上界，`maxsec` 这个前提不成立。
所以当时 `ClockSource` 和 `ClockEventDevice` 都改用了 `mtl::Ratio<u32>`：约分后的精确分数，
`mtl::scale()` 把 64 位值拆成高低 32 位分别乘除，中间量不会溢出（理由见 `docs/repairs-20260830.md`
「为什么是 `mtl::Ratio` 而不是 `calculate_mult_shift()`」）。从那以后这个函数就没有调用者了。

留着它，下一个读代码的人会以为这是时钟换算的正路，而它依赖的「delta 有界」前提在本内核里已经不存在了。

**改法**
修改后：

```cpp
// kernel/include/ours/time/mod.hpp
auto current_mono_time() -> Time;

} // namespace ours::time
```

`mod.cpp` 整个删除，`BUILD.gn` 的 `sources` 去掉它。

**参照**
Linux `clocks_calc_mult_shift()`（`kernel/time/clocksource.c`）是它的原型；本内核的换算对应的是 Zircon 的
`affine::Ratio`（`platform_get_ticks_to_time_ratio()`），也就是现在的 `mtl::Ratio`。

**验证**
- `clang-format` / `clang-tidy -p out/bringup.x86` 对 `mod.hpp` 无告警。
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过（GN 自动重新生成）。
- QEMU 启动 `kernel.dev.obi`、`kernel.dev.dbg_1.obi` 各约 50 s：都进 shell，无 panic / 断言，
  dbg_1 的 54 条 warn 与修改前相同（用户态 #DE 测试的寄存器转储）。

**故意没做**
- `kernel/lib/clock` 的 `FixedPointRatio` 是同一个 mult/shift 思路的另一个实现（128 位中间量，不受 delta 有界的限制），
  同样没有调用者，`calibration.hpp` 还引用了不存在的 `BinaryFixedPointRatio`。它对应
  `docs/development/kernel/time.md` 里的 TODO 设计，是否删除要单独决定，不在这一条里。
- 时钟换算没有改用 ffl。`ffl::Fixed<Integer, FractionalBits>` 的小数位数是编译期模板参数，而时钟比值要到运行时
  （校准之后）才知道，范围还很宽：32 kHz 的计数器每 tick 约 30517 ns，3 GHz 的 TSC 每 tick 约 0.33 ns，反方向
  （ns → ticks）同样跨这么多个数量级。一个编译期格式只能照顾两头中的一头，这正是 Linux 要在运行时算 `shift` 的原因。
  另外，ffl 的收敛舍入是为"反复累加许多舍入值"设计的（调度器的虚拟时间），而比值常数只量化一次、之后乘以很大的
  tick 数，误差是随 tick 数线性增长的系统偏差，ties-to-even 抵消不了；`mtl::Ratio` 约分后是精确的，没有这个误差。
  Zircon 也是这样分工：ffl 只用在调度器，时间换算用 `affine::Ratio`。
- `current_mono_time()` 在 `mod.hpp` 和 `platform/timer.hpp` 各声明了一次，没有动。
