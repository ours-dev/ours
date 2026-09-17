# kernel/task/sched 改动记录（2026-09-17）

## 加上倒数权重表，把按权重缩放变成乘加移位（2026-09-17）

**文件**
- `kernel/include/ours/task/sched/common.hpp` — 新增 `PRIORITY_TO_WMULT_MAP`、
  `WMULT_SHIFT`、`Wmult`、`priority_to_wmult()`、`weight_to_wmult()`、
  `scale_by_wmult()`，以及三条对表本身的 `static_assert`
- `build/switch/kernel_switchset.gni` — 把 `//system/lib/ffl:headers.config`
  加进 kernel-proper 的 prefix_configs

**症状**

潜伏。把一段时长按权重比例缩放，现在只能写成除法：

```cpp
// kernel/kernel/task/sched/fair.cpp（未改动，仅作说明）
auto const rate = weight / MID_WEIGHT;   // 整数除法
return duration * rate;
```

两个问题叠在一起。近的那个是整数除法：权重比 MID_WEIGHT 小的线程算出 `rate == 0`，
跑多久都不积累加权时间。远的那个是，即使换成定点数把比值算对了，`from_ratio`
内部仍是一次 128 位除法——`__divti3`，libclang_rt.builtins 里的软件循环——
而它坐在每个调度 tick 都要走的路上。

**原因**

权重是编译期已知的 32 个常量。除以一个常量从来不必真的做除法：预先算好
`2^32 / weight`，之后每次缩放就是一次乘法加一次移位。除法只在建表时发生一次，
而建表发生在编译期，运行期一次都没有。

移位取 32 有两个方向的约束，缺一不可：要足够大，让倒数的量化误差相对权重可以忽略
（最大权重 65536 对应最小倒数 2^16，相对误差 2^-16 以内）；又要足够小，让每个倒数
仍然装得进 32 位——这才使缩放保持为 64×32 的乘法。Linux 的 `WMULT_SHIFT` 取同一个值，
理由也是这两条。

**改法**

```cpp
constexpr std::array<Wmult, NUM_PRIORITIES> PRIORITY_TO_WMULT_MAP = [] {
    std::array<Wmult, NUM_PRIORITIES> map{};
    for (usize i = 0; i < NUM_PRIORITIES; ++i) {
        map[i] = static_cast<Wmult>(ffl::reciprocal<u64>(PRIORITY_TO_WEIGHT_MAP[i], WMULT_SHIFT));
    }
    return map;
}();
```

**表是算出来的，不是抄出来的。** Linux 把 `sched_prio_to_wmult[]` 和
`sched_prio_to_weight[]` 并列写成两张字面量表，于是它们有可能对不上——改了一张忘了
另一张，没有任何东西会报错。这里只有一张表，另一张是它的函数，对不上这件事不存在。

`Wmult` 刻意用 `u32` 而不是 `usize`：它只配做加宽乘法的右操作数，一个装不下权重的
类型，就不会被误传到要权重的地方去。

`scale_by_wmult()` 里两次乘法的顺序不是随便排的：

```cpp
auto const factor = static_cast<u64>(weight) * static_cast<u64>(wmult);
return ffl::mul_shr_round<u64>(value, factor, WMULT_SHIFT);
```

先算 `weight * wmult`——它至多是 2^32 乘上两者所表示的比值，64 位装得下——再让
`value` 去乘它，在 128 位里成积。反过来先算 `value * weight` 会在加宽发生之前就在
64 位里溢出。Linux 的 `__calc_delta()` 是同样的顺序；它没有 128 位类型，所以还得先
把操作数循环右移到安全范围（`while (unlikely(w >= WMULT_CONST))`），用低位精度换这份
安全，而这里不必。

三条 `static_assert` 钉住表本身，而不是钉住用表的代码：表严格递增（不递增则高优先级
不再意味着更多 CPU，而这种错只会表现为没人会追回表里的调度异常）；每个倒数装得进
`Wmult`（调低 MIN_WEIGHT 会构建失败，而不是静默截断）；权重与其倒数往返精确
（32 项全部 `weight * 1000` 缩放回来等于 1000）。

**参照**

`Linux sched_prio_to_wmult[]`、`__calc_delta()`、`update_load_add()`、
`kernel/sched/fair.c` 的 `WMULT_SHIFT`。一处刻意分歧：Linux 在末次移位处截断，
本实现用 `ffl::mul_shr_round` 就近舍入，因为这些结果是要跨 tick 累加的，截断的偏差
单向累积而不抵消。

**验证**

构建全过（32 个 target），`static_assert` 即随之通过。QEMU 实测两个镜像：
`kernel.dev.dbg_1.obi`（DEBUG_ASSERT 打开）四核跑满，线程正常抢占轮转，无断言无异常；
`kernel.dev.obi` 启动到用户态 shell，命令表枚举正常。

**故意没做**

- **没有改 `fair.cpp`**。Fair/EEVDF 是作者自留的设计，会重构；上面那段除法留在原处，
  这次只把它将来要用的工具备好。
- 没有加 weight→priority 的反查。目前没有调用方，等有了再说。
- `weight_to_wmult()` 保留了运行期除法，因为运行队列的总负载是权重之和，不在表里。
  它被设计成与权重一起缓存、只在权重变化时重算——和 Linux 的 `struct load_weight`
  把 `weight` 与 `inv_weight` 并排放在一起是同一个道理。

## 撤回倒数权重表，改成 Zircon 的做法：权重本身就是定点数（2026-09-17）

**文件**
- `kernel/include/ours/task/sched/types.hpp` — `Weight` 由 `usize` 改为
  `ffl::Fixed<i64, 16>`
- `kernel/include/ours/task/sched/common.hpp` — 表改为归一化到 1.0 的定点数；
  删掉 `WMULT_SHIFT`、`Wmult`、`PRIORITY_TO_WMULT_MAP`、`MID_WMULT`、
  `priority_to_wmult()`、`weight_to_wmult()`、`scale_by_wmult()`；新增
  `SchedDuration` 与 `Weight` 之间的两个桥接运算符

**症状**

上一节做的东西方向是错的。作者的原话：「这太屎了，你看一下这块儿 Zircon 咋做的。」

**原因**

Zircon 根本没有倒数表。它的做法是让权重本身成为定点数：

```cpp
// zircon/kernel/include/kernel/scheduler_state.h
using SchedWeight = ffl::Fixed<int64_t, 10>;

struct WeightTableEntry {
  constexpr WeightTableEntry(int64_t value)
      : value{ffl::FromRatio<int64_t>(value, SchedWeight::Format::Power)} {}
  ...
};
inline constexpr WeightTableEntry kPriorityToWeightTable[] = {
    53, 58, 64, ... 930, 1024,
};
```

表里存的是 1024 分之几，最高优先级正好是 1.0。于是权重之比就是定点数除以定点数，
`scheduler.cc` 里直接写 `ep.weight() / weight_total_`——没有 wmult，没有
`mul_shr`，没有任何额外机制。

我做的是 Linux 的构造：整数权重 + 第二张 `sched_prio_to_wmult[]` 倒数表 +
`__calc_delta()` 式的乘移位。它本身没算错，但**放在一个已经有 ffl 的项目里就是多余的
第二套答案**。更难堪的是这个项目早就选了 Zircon 的模型——`profiles.hpp` 里
`BaseProfile` 的注释自己就写着「Zircon's BaseProfile(int priority) does the same
conversion in the same place, via `SchedulerState::ConvertPriorityToWeight()`」——
我没去读就照 Linux 搬了。

**改法**

```cpp
// 改前（types.hpp）
using Weight = usize;

// 改后
using Weight = ffl::Fixed<i64, 16>;
```

十六位小数位不是挑的，是现有表本来就有的刻度：表的最大项是 65536 = 2^16，所以那 32 个
数字原样就是这个格式的 raw 值，一个都不用重编。表改成按分数写：

```cpp
// 改前
constexpr Weight PRIORITY_TO_WEIGHT_MAP[] = {121, 149, ..., 65536};

// 改后
constexpr std::array<Weight, NUM_PRIORITIES> PRIORITY_TO_WEIGHT_MAP = [] {
    std::array<Weight, NUM_PRIORITIES> map{};
    for (usize i = 0; i < NUM_PRIORITIES; ++i) {
        map[i] = Weight::from_ratio(detail::WEIGHT_NUMERATORS[i], Weight::Format::ONE);
    }
    return map;
}();
```

`Weight` 跟着 Zircon 用有符号，尽管权重不会为负：它要和有符号的时长、时刻一起参与
运算，一个无符号操作数会把整个表达式提升为无符号，本该略微为负的减法于是变成巨大的
正数。保留符号位比逐条审查每个表达式便宜。

`static_assert` 换了三条，钉的是这个模型真正依赖的性质：最大权重恰为 1.0、表严格递增、
最小权重不为零。

**唯一的遗留缝隙**：Zircon 的 `SchedDuration` 也是 `ffl::Fixed<…, 0>`，所以时长与权重
同处一套算术；本项目的 `SchedDuration` 还是 `std::chrono::nanoseconds`，于是
`fair.hpp` 里的 `total_vruntime / weight_sum` 编译不过。补了两个自由运算符把这条缝
显式写出来（128 位中间量，乘法就近舍入，除以零饱和），并在注释里写明**它们存在就是
为了被删掉**——等 `SchedDuration` 也变成 `Fixed`，它们自然不可达。没有为此改
`fair.hpp` 一行。

**参照**

`Zircon SchedWeight` / `kPriorityToWeightTable` / `WeightTableEntry` /
`SchedulerState::ConvertPriorityToWeight()`（`kernel/include/kernel/scheduler_state.h`），
以及 `scheduler.cc` 里 `ep.weight() / weight_total_` 的用法。
被否掉的是 `Linux sched_prio_to_wmult[]` + `__calc_delta()`。

**验证**

全量构建通过。QEMU 实测两个镜像：`kernel.dev.dbg_1.obi` 四核抢占轮转，无断言无异常；
`kernel.dev.obi` 启动到用户态 shell，命令表枚举正常。

**故意没做**

- **没有把 `SchedDuration`/`SchedTime` 也改成 `ffl::Fixed`**。那才是 Zircon 的完整形态，
  但它牵动 44 处以及时钟、定时器的每一个边界，而且 `scheduler.cpp` 眼下有别的会话在改。
  单独一件事做。
- 没有改 `fair.cpp` / `fair.hpp`。
- `ffl/mul_shr.hpp` 留着——上面的乘法桥接在用它。但它当初的动机（避开 tick 路径上的
  除法）是我自己加的，不是这个项目的方向；如果 `SchedDuration` 将来也变成 `Fixed`，
  它会一起失去用户。

## 时长与时刻也改成定点数，删掉桥接运算符（2026-09-17）

**文件**
- `kernel/include/ours/task/sched/types.hpp` — `SchedDuration`/`SchedTime` 改为
  `ffl::Fixed<i64, 0>`；新增 `to_sched_time()`、`to_sched_duration()`、`to_time_point()`
- `kernel/include/ours/task/sched/common.hpp` — 删掉上一节那两个桥接运算符
- `kernel/include/ours/task/sched/runtime.hpp` — 常量走 `to_sched_duration()`
- `kernel/kernel/task/sched/driver.cpp` — 时钟边界显式化
- `kernel/kernel/task/sched/rr.cpp` — `::zero()`/`.count()` 改 `Fixed` 的写法
- `kernel/kernel/time/tick_sched.cpp` — tick 入口处转换一次

**症状**

上一节留的缝隙：权重成了定点数、时长还是 `std::chrono::nanoseconds`，两者不在同一套
算术里，于是 `fair.hpp` 的 `total_vruntime / weight_sum` 要靠两个专门写的自由运算符
才编得过。那两个运算符当时就是写来被删的。

**原因**

Zircon 不需要它们，因为它把三者放在同一套算术里：

```cpp
// zircon/kernel/include/kernel/scheduler_state.h
using SchedWeight      = ffl::Fixed<int64_t, 10>;
using SchedDuration    = ffl::Fixed<zx_duration_mono_t, 0>;
using SchedTime        = ffl::Fixed<zx_instant_mono_t, 0>;
```

零位小数的定点数听起来是 `i64` 的啰嗦写法，其实不是。要点在于它和 `Weight` 共用一套
运算：`duration * weight`、`vruntime / weight_sum` 于是是普通值上的普通运算符，
加宽、舍入、饱和都已经在一个地方定好了，而不是每个调用点各想各的。

**改法**

```cpp
// 改前
using SchedDuration = Duration;      // std::chrono::nanoseconds
using SchedTime = TimePoint<>;

// 改后
using SchedDuration = ffl::Fixed<i64, 0>;
using SchedTime = ffl::Fixed<i64, 0>;
```

前置条件是 ffl 的乘除要支持跨格式（见 `docs/changes/260917/ffl.md` 同日一节）。
做完之后 `fair.hpp` 一行没改就编过了——那正是判断这个方向对不对的检验。

**代价，明说**：时刻和时长现在是同一个类型，`time + time` 能编过，而被它替换掉的
那对 chrono 类型会拒绝。这是真的损失，是权衡后收下的：另一条路是给 `Fixed` 加一个
幽灵标签参数，而 ffl 没有这个东西。Zircon 也付同样的代价。

**时钟边界**收到三个具名函数里。内核时钟是带类型的（`TimePoint<SteadyClock>`），
这份类型在调度器外面值得保留、在里面留不住，所以跨越它的地方必须写出来，一共三处：
`Driver::update_clock()`、`Driver::reset_preempt()`、`TickSched::on_tick()`。

**参照**

`Zircon SchedDuration`/`SchedTime`（`kernel/include/kernel/scheduler_state.h`），
以及 `scheduler.cc` 里时长与权重直接混算的写法。

**验证**

全量构建通过。QEMU 实测两个镜像，各约 50 秒：

- `kernel.dev.dbg_1.obi`：四核，两个测试线程交替 141688 / 141749 次，抢占正常；
  断言/panic/异常计数为 0。
- `kernel.dev.obi`：userboot 装载 shell、映射 vDSO、命令表枚举正常；断言计数为 0。

（一个自己踩的坑记在这里：串口日志里混有二进制字节，`grep` 会因此判定为 binary 而
不输出，先前几次「没有断言」的检查其实是空的。要加 `-a`。）

**故意没做**

- 没有改 `fair.cpp` / `fair.hpp`（`fair.hpp` 本来就不需要改）。
- 没有改 `scheduler.hpp` / `scheduler.cpp`：它们只是把 `SchedTime` 透传，不做算术；
  而且此刻有别的会话在改这两个文件。
- 没有给 `Fixed` 加幽灵标签来重建时刻/时长的区分。真要做是 ffl 的事，不是调度器的事。

## 权重表改为由比值算出，不再打表（2026-09-17）

**文件**
- `kernel/include/ours/task/sched/common.hpp` — 删掉 32 个字面量，改为从
  `WEIGHT_RATIO_NUMERATOR` / `WEIGHT_RATIO_DENOMINATOR` / `NUM_PRIORITIES` 生成

**症状**

表是 32 个手写的数。它们之间的关系——「每档比下一档重 22.5%」——只存在于注释里：
没有任何东西检查第 17 项确实是第 16 项的 1.225 倍。想加几档优先级，就得手算 32 个数
然后祈祷。

**原因**

搞反了输入和输出。真正说明「一档优先级意味着什么」的是那个比值，它才该是输入；
32 个数是它的结果。而且一旦参数化，参数的选择本身也要想清楚：**参数必须是每档的比值，
不能是整段的跨度**——否则加优先级档数会悄悄改变一档的含义，而一档的含义正是这张表
唯一要表达的东西。

**改法**

先验证原表到底是不是几何级数。是的，而且分毫不差：

```
round(65536 × (40/49)^(31-p))   // 32 项，与原字面量逐项相同
```

于是改成：

```cpp
// 改前
constexpr i64 WEIGHT_NUMERATORS[] = {121, 149, 182, ..., 53499, 65536};

// 改后
constexpr i64 WEIGHT_RATIO_NUMERATOR = 49;
constexpr i64 WEIGHT_RATIO_DENOMINATOR = 40;
constexpr usize NUM_PRIORITIES = 32;

constexpr std::array<Weight, NUM_PRIORITIES> PRIORITY_TO_WEIGHT_MAP = [] {
    using Accumulator = ffl::Wider<i64>;
    constexpr usize GUARD_BITS = 48;
    ...
    auto accumulator = Accumulator{Weight::Format::ONE} << GUARD_BITS;
    map[NUM_PRIORITIES - 1] = Weight::from_raw(Weight::Format::ONE);
    for (usize i = NUM_PRIORITIES - 1; i > 0; --i) {
        accumulator = (accumulator * WEIGHT_RATIO_DENOMINATOR + WEIGHT_RATIO_NUMERATOR / 2)
                      / WEIGHT_RATIO_NUMERATOR;
        map[i - 1] = Weight::from_raw(static_cast<i64>((accumulator + GUARD_HALF) >> GUARD_BITS));
    }
    return map;
}();
```

两个决定值得说明。

**向下生成，不向上生成**。表顶是其余每一项所占份额的基准，它必须精确；从最小权重往上
累乘，会让它差一个舍入误差到不了 1.0，「最高优先级恰为 1.0」这条不变量就悄悄破了。

**多带 48 位保护位**。每一步都除，如果每步都舍入到存储格式的 16 位小数，误差会累积，
而且恰恰在最难受的地方累积最狠：表底一个 ulp 将近 1%，相邻低优先级之间的比值就不再是
上面声明的那个比值了。48 位保护位把每步误差压到远低于最终舍入，最宽的中间量 70 位，
在 128 位累加器里绰绰有余。

用比值写而不是写小数，因为内核没有浮点（`-mno-sse`），而且精确有理数让生成结果可复现，
不依赖某个常量怎么折叠。

**断言也跟着换**，从「钉数值」换成「钉性质」，这样扩展表不会踩到自己的断言：
表顶恰为 1.0；相邻项确实成声明的比值（偏差不超过分子，且这个界不随表长变化）；
严格递增。最后一条同时是扩展的闸门，而且是最先触发的那条——每档除以 1.225，
项在几何缩小而格式分辨率不变，16 位小数下大约 50 档之后相邻两项会舍入成同一个数。
断言消息直接说了该怎么办：给 `Weight` 更多小数位，而不是把表改短。

**参照**

`Zircon kPriorityToWeightTable`（`Weight = 1.1^(Priority - 31)`，但表是手写的）、
`Linux sched_prio_to_weight[]`（1.25 每档，也是手写的）。两边都把比值写在注释里、
把结果写进代码；这里反过来。

**验证**

一次性迁移校验：把生成器与原来那 32 个字面量放进同一个翻译单元做 `static_assert`
逐项比较，通过——所以这次改动可证明不改变任何数值。
（校验用的临时文件没有留下，因为留着就等于把字面量表又请回来了。）

全量构建通过。QEMU 实测两个镜像，各约 40 秒：`kernel.dev.dbg_1.obi` 两线程交替
74294 / 73867 次，断言/panic/异常计数 0；`kernel.dev.obi` userboot 正常，计数 0。

**故意没做**

- 没有改比值和档数。这次只换表达方式，数值一个没动。
- 没有把生成器放进 ffl。它是「按固定比值生成一张定点数表」，通用性存在但目前只有
  一个用户，等出现第二个再说。
