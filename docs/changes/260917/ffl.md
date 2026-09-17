# system/lib/ffl — 改动记录

格式见 `docs/changes/TEMPLATE.md`。

---

## 1. 新增定点算术库 `ffl`（2026-09-17）

**文件**
- `system/lib/ffl/**` — 新库（4 个头文件 + 宿主单测），用 `tools/templates/cpp_library`
  的 copier 模板生成骨架。
- `system/lib/BUILD.gn`、`BUILD.gn` — 挂进单测汇总。

**症状**

`kernel/kernel/task/sched/fair.cpp:26`：

```cpp
auto const rate = weight / MID_WEIGHT;   // Weight = usize，整数除法
return duration * rate;                  // weight < MID_WEIGHT ⇒ rate == 0
```

**任何轻于平均权重的线程，加权时间恒为零**——无论它实际跑了多久。这是公平性直接失效，
不是精度损失。

**原因**

内核不能用浮点（`//system/BUILD.gn` 的 `-msoft-float -mno-sse`），而比值天然是分数。
`Weight` 是 `usize`，`820 / 1024` 就是 0。

**改法**

新增 `ffl`，四层：

```
saturating.hpp   饱和加减乘 + 钳位窄化
wider.hpp        乘除的加宽中间类型（i64 → __int128）
format.hpp       位宽拆分 + 收敛舍入
fixed.hpp        Fixed<Integer, FractionalBits>
```

关键的一个操作是 `from_ratio()`——**先把分子左移进小数域再除**，所以小于一的比值不会归零：

```cpp
auto const scaled = static_cast<Wider<Integer>>(numerator)
                    << static_cast<Wider<Integer>>(FractionalBits);
return from_raw(clamping_cast<Integer>(scaled / static_cast<Wider<Integer>>(denominator)));
```

移位在**更宽**的类型里做，因为 `numerator << FractionalBits` 正是加宽要吸收的那个溢出。

调用方变成：

```cpp
auto const rate = Weight::from_ratio(weight, MID_WEIGHT);
auto const weighted = (rate * Weight{duration}).round();
```

**参照**

`Zircon ffl`（`zircon/system/ulib/ffl`）。名字沿用，**命名按本仓库规范重写**：
`from_ratio()` 而不是 `FromRatio()`，`UPPER_CASE` 常量而不是 `kCamelCase`。

**两个刻意的取舍**

1. **收敛舍入（ties-to-even）而不是就近远零**。这些值被反复累加；朝固定方向断开平局，
   误差随累加次数线性增长。单测 `AccumulatingHalvesDoesNotDrift` 钉住了这条性质。
2. **不做表达式模板**。ffl 用惰性求值按赋值目标挑中间精度，占它 1823 行中的 492 行。
   本调度器的算术是二元的，显式加宽就够。出现三元链再补——而不是让每个读者先理解一棵
   惰性求值树。

**故意没做**

- **没有改 `fair.cpp`**。Fair/EEVDF 是作者自留的设计，这个库是交给它用的工具，
  接入由作者决定。
- 没有移植 ffl 的 `string`（内核用 logz4 格式化）和 `exponential_average`
  （等 EEVDF 真的需要时再说）。

**验证**

绝大部分语义是 `static_assert`，写在单测的翻译单元里——**构建通过即已验证**，
不依赖有没有人去跑它。运行期 5 项覆盖编译器看不穿的值与循环：收敛舍入的方向、
累加不漂移、饱和不回绕、加宽中间类型保精度、比值序与值序一致。全部通过。
