# kernel/lib/clock 改动记录（2026-09-26）

## 删除 `kernel/lib/clock` 和它对应的设计草稿（2026-09-26）

**文件**
- `kernel/lib/clock/` — 整个删除：`include/clock/fixed_point_ratio.hpp`、`include/clock/calibration.hpp`、`BUILD.gn`、
  `.clang-format`、`.gitignore`
- `kernel/lib/BUILD.gn` — `_libraries` 去掉 `"clock"`
- `kernel/products/BUILD.gn` — `static-pic-link` 的注释原来说 `__udivti3` 来自 lib/clock，改成真实来源
- `docs/development/kernel/time.md` — 删除；全文就是这个库的设计草稿（TODO 标题下的 mult/shift 推导和精度分析）

**症状**
无运行时症状。这个库没有任何代码 include 过，`BUILD.gn` 里的 `source_set("clock")` 没有源文件，测试组都是空的。
`calibration.hpp` 还引用了一个不存在的类型 `BinaryFixedPointRatio`（定义的类叫 `FixedPointRatio`），谁一 include 就编译失败——
它能一直留在树里，正是因为从来没人 include。

**原因**
它是 `docs/development/kernel/time.md` 里一份 TODO 设计的实现，设计有两半，两半都已经被别的做法取代：

- **换算**：`FixedPointRatio::from_ratio()` 在运行时挑 `(multiplier, shift)`，用 128 位中间量算
  `(value * m) >> s`。树里实际走的是 `mtl::Ratio<u32>`：约分后的精确分数，`ClockSource::m_ticks_to_ns` 和
  `ClockEventDevice::m_scaler` 都用它（0830 起，理由见 `docs/repairs-20260830.md`「为什么是 `mtl::Ratio`」）。
  两者的取舍是速度对精度：mult/shift 是一次乘法加一次移位，但比值只量化一次、误差随 tick 数线性增长；
  `mtl::Ratio` 要做除法，但没有换算误差。
- **校准**：`Calibration<Adapter>` 是"在若干参考窗口里各测几次、取延迟最小的一次"的通用流程。实际的 LAPIC 校准在
  `kernel/arch/x86/apic/local_apic.cpp`，改成了在同一时刻采样两个计数器、直接求比值（`Calibration::hz_of()`），
  两次启动之间的漂移从 2.4% 降到 14 ppm，走的已经不是这条路。

两半都没有调用者，文档把它写成"要做的事"，会让读的人以为时钟换算和校准还要往这个方向迁。

`kernel/products/BUILD.gn` 的注释是另一处连带的错：它说 clang 会生成 `__udivti3` 之类的调用，来源是 lib/clock 的
128 位定点运算。可 lib/clock 从没被编译进任何东西。用 `llvm-objdump` 查 `vmours`，实际调用 `__udivti3` 的只有 libc++
`std::format` 的整数格式化，它对 `unsigned __int128` 做了实例化（`__formatter::__format_integer<unsigned __int128>` →
`__to_chars_integral<unsigned __int128>`），不管有没有代码真的打印 128 位整数都会实例化。链接 `libclang_rt.builtins.a`
的理由不变，只是来源写错了。

**改法**
修改前（`kernel/products/BUILD.gn`）：

```gn
  # The compiler runtime is a link input, not a driver option. clang emits
  # calls into it -- __udivti3 and friends, from the 128-bit fixed-point math
  # in //kernel/lib/clock -- with no call site in the source to show for it,
  # and -nostdlib below drops it along with libc.
```

修改后：

```gn
  # The compiler runtime is a link input, not a driver option. clang emits
  # calls into it -- __udivti3 and friends, today from libc++'s std::format,
  # whose integer formatter is instantiated for unsigned __int128 whether or
  # not anything prints one -- with no call site in the source to show for
  # it, and -nostdlib below drops it along with libc.
```

`kernel/lib/BUILD.gn` 修改前：

```gn
_libraries = [
  "acpi",
  "ccrt",
  "clock",
  "code_patching",
```

修改后：

```gn
_libraries = [
  "acpi",
  "ccrt",
  "code_patching",
```

库目录和设计草稿整个删除。以后真要做 mult/shift 快速路径，或者统一的校准框架，可以从 git 历史里找回。

**参照**
`FixedPointRatio::from_ratio()` 对应 Linux `clocks_calc_mult_shift()`；本内核实际采用的换算对应 Zircon `affine::Ratio`
（`platform_get_ticks_to_time_ratio()`）。

**验证**
- 删除前 `grep` 全树：除自身、`kernel/lib/BUILD.gn` 的列表和 `kernel/products/BUILD.gn` 的注释外，没有任何引用。
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过（GN 重新生成）。
- QEMU：`kernel.dev.obi` 跑完到 `--- end ---`。`kernel.dev.dbg_1.obi` 并行跑了 3 次，每次 90 s：1 次完整跑完（`probes done`、
  `bootfs checks passed`、`--- end ---`），2 次卡在 fault_probe。卡住是 `faa117f7` 把 tick 恢复到 100 Hz 之后暴露出来的
  tick 补拍活锁（`TickDevice::handle_periodic_event()`），与这次删除无关：删掉的代码从来没有被编译进内核。

**故意没做**
- 没有把换算改成 mult/shift 或 ffl。理由见 `kernel-time.md`「删除无人调用的 `calculate_mult_shift()`」一节。
