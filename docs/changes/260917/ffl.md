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

## 补齐 Fixed 的接口缺口，并加上 mul_shr 一族（2026-09-17）

**文件**
- `system/lib/ffl/include/ffl/fixed.hpp` — 加跨格式转换构造、`operator/(Integer)`、
  四个复合赋值运算符、`absolute()`
- `system/lib/ffl/include/ffl/mul_shr.hpp` — 新文件，`mul_shr` / `mul_shr_round` /
  `reciprocal`
- `system/lib/ffl/BUILD.gn`、`system/lib/ffl/test/fixed_test.cpp` — 接上

**症状**

两件事，都是潜伏的。

其一，`Fixed<i64, 16>` 和 `Fixed<i64, 8>` 之间没有任何转换通道。调度器里
虚拟时间要高精度、权重比值只需要低精度，两种格式必然共存；没有转换就只能
`from_raw(other.raw())` —— 那是按位重解释，把 1.5 读成 384，编译器一声不吭。

其二，也是更要紧的一件：把比值乘回时长这条路上有一次 128 位除法。

```cpp
// 反汇编 Q::from_ratio(w, MID_WEIGHT) * Q{d}，-O2
bl  ___divti3
```

`__divti3` 是 libclang_rt.builtins 里的软件除法循环，几十到几百个周期。它坐在
每个调度 tick 都要走的路上。

**原因**

`from_ratio` 内部是 `(numerator << FractionalBits) / denominator`。被除数被提到
128 位以后，这个除法在 x86-64 上没有单条指令可用，只能落到编译器运行时。
比值本身却是常量——权重表是编译期已知的 32 个值——除法完全可以在编译期做掉，
运行期只留一次乘法和一次移位。Linux 的结论一样：`sched_prio_to_wmult[]` 存的是
`2^32 / weight`，`__calc_delta()` 用 `mul_u64_u32_shr()` 缩放。

**改法**

跨格式转换写成 `explicit` 构造：加宽小数位是精确的左移，缩窄要舍入——直接右移
是截断，而截断的偏差会被后面每一次使用继承下去，所以按 `round()` 同一套规则
半入偶。`explicit` 是刻意的：两个格式在同样的位里装着不同的数，隐式转换会让
一个被当成另一个传进去而没有任何诊断。

`mul_shr` 和 `mul_shr_round` 分成两个名字，而不是让 `mul_shr` 悄悄舍入。
写第一版时 `static_assert(mul_shr(1000 * 3122, reciprocal(3122, 32), 32) == 1000)`
失败，实际是 999——最后那次移位截掉了尾巴。把 `mul_shr` 改成舍入能让断言过，
但那样这个名字就在说谎；累加场景该用哪个，是调用点要自己决定的事。两个变体的
语义各有一条 `static_assert` 钉住。

`reciprocal` 舍入而不截断，理由不同：倒数一次算好、用一辈子，它的偏差是系统性的，
不是随机的。

`absolute()` 对最小值饱和而不是回绕：二补数的最小值没有对应的正数，取负会原样返回,
一个负的"绝对值"比饱和糟得多。

**参照**

`Linux __calc_delta()` / `mul_u64_u32_shr()` / `sched_prio_to_wmult[]`。
一处刻意的分歧：Linux 在这里截断并接受那点偏差，本库对 `Fixed` 的舍入已经选了
另一边，一个库不该对同一个问题持两种意见——所以提供了舍入变体并推荐它。

**验证**

语义主体仍是 `static_assert`，构建通过即已验证。运行期新增 2 项：

- `MatchesDivisionAcrossAWideRangeOfWeights` —— 512 个权重 × 6 个量级，乘倒数
  与真除法的误差都在千分之一以内。
- `TruncationBiasIsWhatRoundingRemoves` —— 同一个值累加一万次，截断变体的误差
  单向累积，舍入变体的误差小于它。

`out/bringup.x86/host/obj/system/lib/ffl/test/fixed`：7/7 通过。

**故意没做**

- 没有改 `fair.cpp`。同上一节，接入由作者决定。
- `reciprocal` 没有做成运行期可用的形态。除法正是要被挪走的东西，运行期算一个
  倒数等于把它放回去；需要的调用方应当建编译期表。

## 乘除推广到跨格式，左操作数的格式取胜（2026-09-17）

**文件**
- `system/lib/ffl/include/ffl/fixed.hpp` — `operator*` / `operator/` / `operator*=`
  / `operator/=` 改为接受任意小数位的 `Fixed`；新增自由函数 `operator*(Integer, Fixed)`
- `system/lib/ffl/test/fixed_test.cpp` — 跨格式语义的 `static_assert`

**症状**

`Fixed<i64, 0>` 乘 `Fixed<i64, 16>` 编译不过。这不是个抽象的缺口：调度器里
`duration * weight`（时长乘以权重比例）正是这个形状，而 Zircon 的
`SchedDuration`/`SchedWeight` 就是这两个格式。

**原因**

原来的 `operator*(Self)` 只接受同格式。同格式是特例不是通例：两个分别按 2^f1、2^f2
缩放的数，乘出来按 2^(f1+f2) 缩放，回到哪个格式是一个必须回答的问题，而原实现把它
回避成了「只许 f1 == f2」。

**改法**

推广成模板，**结果取左操作数的格式**：

```cpp
template <usize OtherFractionalBits>
constexpr auto operator*(Fixed<Integer, OtherFractionalBits> rhs) const -> Self {
    return from_raw(mul_shr_round<Integer>(m_raw, rhs.raw(), OtherFractionalBits));
}
```

这条规则不是随手定的，是被 `operator*=` 逼出来的：复合赋值只可能产出被赋值对象的
格式，而 `a * b` 必须和 `a *= b` 意思一致，否则两种写法就成了两件事。它也正好是调用点
想说的：`duration * weight` 是「把这段时长按这个比例缩放」，答案是一段时长。

Zircon 的 ffl 对同一个问题给出的答案更好——它根本不急着回答：表达式模板把乘法推迟到
赋值处，由目标类型挑中间精度。那正是本文件开头声明不移植的 492 行。所以这里在注释里
写明了代价：链式表达式、或中间量需要比两个操作数都多的小数位时，要显式转换并说明理由。

**加法和减法刻意没有推广**。跨格式相乘是*缩放*，在不同量纲之间有意义；跨格式相加意味着
两者是同一种量、只是精度不同，那是一次转换，应该由调用者写出来。`duration + weight`
在任何程序里都是 bug，隐式改比例只会让它编译通过。

**参照**

`Zircon ffl::Fixed` 的表达式模板与 `Precision`/`Resolution` 解析（被否掉的方案），
`kernel/include/kernel/scheduler_state.h` 里 `SchedDuration`/`SchedWeight` 的实际用法。

**验证**

主机单测 7/7 通过，含新增的跨格式 `static_assert`：结果类型取左操作数、
`Whole{1000} * Frac(1/4) == 250`、`a * b` 与 `a *= b` 一致、整数可写在任一侧。

**故意没做**

- 没有加跨格式的 `+` / `-`（理由同上）。
- 没有加跨*整数类型*的运算（`Fixed<i32,…>` 乘 `Fixed<i64,…>`）。目前没有调用方。
