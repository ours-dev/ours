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

## 补上 fair.cpp 里被定点数改动落下的 6 处拼法（2026-09-17）

**文件**
- `kernel/kernel/task/sched/fair.cpp` — `SchedDuration::zero()` → `SchedDuration{0}`（4 处）、
  `.count()` → `.raw()`、去掉一处 `.time_since_epoch()`

**症状**

`fair.cpp` 编译不过。它不在构建里（`kernel/kernel/task/sched/BUILD.gn` 用 `rr.cpp` 顶替），
所以把 `SchedDuration` 从 `std::chrono::nanoseconds` 换成 `ffl::Fixed<i64, 0>` 那一轮，
没有任何东西报错——错误一直躺到有人把 Fair 放回构建才会出现。

**原因**

我自己的改动留下的失配，不是 Fair 的设计问题。`Fixed` 没有 `zero()`（`Fixed{0}` 就是），
没有 `count()`（叫 `raw()`），而 `SchedTime` 不再是 `TimePoint`，它本身就是纳秒计数，
所以 `deadline().time_since_epoch()` 这一步没有了对应物——去掉它是恒等变换，
不是语义改动。Fair 的逻辑一行没动。

**改法**

```cpp
// 改前
if (ctx.deadline().time_since_epoch() <= ctx.runtime()) {
// 改后
if (ctx.deadline() <= ctx.runtime()) {
```

**参照**

无，是本项目自身改动的收尾。

**验证**

`fair.cpp` 不在构建里，所以要单独查。办法是把 `rr.cpp` 的真实编译命令抠出来换掉源文件：

```
prebuilt/ninja/ninja -C out/bringup.x86 -t commands \
    kernel_x86.dbg.1/obj/kernel/kernel/task/sched/sched.rr.cpp.o | tail -1
```

把其中的 `rr.cpp` 换成 `fair.cpp`、`-o <obj>` 换成 `-fsyntax-only`、去掉 `-MD -MF`，
在 `out/bringup.x86` 下执行。改前 6 个错误，改后 0 个。整体构建不受影响（它本就不参与链接）。

**故意没做**

- 没有把 `fair.cpp` 放回构建。它还有别的问题（例如 `calc_weighted_time` 的整数除法），
  且 Fair 是作者自留的设计，回不回构建由作者决定。
- 没有碰 Fair 的任何逻辑。

## 调度周期常量从 Runtime 字段上移为 constexpr（2026-09-17）

**文件**
- `kernel/include/ours/task/sched/common.hpp` — 新增 `MIN_SCHED_GRANULARITY`、
  `TARGETED_LATENCY`、`SCHEDULING_PERIOD_GRANS`、`SCHED_GRANULARITY` 和自由函数
  `scheduling_period()`
- `kernel/include/ours/task/sched/runtime.hpp` — 删掉五个成员和两个方法
- `kernel/kernel/task/sched/rr.cpp`、`fair.cpp` — 改调自由函数

**症状**

潜伏。`Runtime` 里有三个字段（`scheduling_period_grans`、`minimal_period_grans`、
`minimal_granularity`）和两个静态常量，每个 CPU 一份，而每个 CPU 都用同一个表达式
初始化它们——它们从来没有 per-CPU 过。副作用是：纪律想问一个编译期就有答案的问题
（「一轮周期多长」），也必须先被递上整个运行队列。

`min_scheduling_period()` 零调用者，而且和 `scheduling_period()` 恒等
（两个 grans 字段的初始化表达式一模一样）。

**原因**

把全局常量伪装成 per-CPU 字段，有两重代价。每个运行队列为此多占一段缓存行，
只为了在每个 CPU 上说同一句话；更要紧的是它扩大了纪律的接口需求——
`ISchedule::on_thread_init()` 之所以要收一个 `Runtime &`，唯一的理由就是
`RoundRobin` 要读 `runtime.scheduling_period()`。常量上移之后这个理由就没了。

Linux 划的是同一条线：`sysctl_sched_base_slice` 是全局变量，不在 `struct rq` 里。

**改法**

```cpp
// 改前（runtime.hpp，每 CPU 一份）
usize scheduling_period_grans = DEFAULT_MIN_TARGETED_LATENCY / DEFAULT_MIN_SCHED_GRANULARITY;
usize minimal_period_grans    = DEFAULT_MIN_TARGETED_LATENCY / DEFAULT_MIN_SCHED_GRANULARITY;
SchedDuration minimal_granularity = to_sched_duration(DEFAULT_MIN_TARGETED_LATENCY);
auto scheduling_period() const -> SchedDuration;
auto min_scheduling_period() const -> SchedDuration;

// 改后（common.hpp，编译期）
constexpr usize SCHEDULING_PERIOD_GRANS = TARGETED_LATENCY / MIN_SCHED_GRANULARITY;
constexpr SchedDuration SCHED_GRANULARITY = to_sched_duration(TARGETED_LATENCY);
FORCE_INLINE
constexpr auto scheduling_period() -> SchedDuration;
```

`min_scheduling_period()` 直接删掉：没有调用者，而且和上面那个恒等。

**刻意保留了一个 bug。** `SCHED_GRANULARITY` 取的是 `TARGETED_LATENCY`（8ms）
而不是 `MIN_SCHED_GRANULARITY`（1ms），于是周期是 64ms 而不是 8ms——这正是原来那个
字段的值，原样搬了过来。改它会把上下文切换频率提高 8 倍，是行为变更，必须自己一个
提交、自己一份实测数字。已记入 `docs/repairs/260917/kernel-task-sched.md`。
注释里把这件事写在常量头上，而不是只记在待修文档里，因为读代码的人先遇到的是常量。

**参照**

`Linux sysctl_sched_base_slice`（全局，不在 `struct rq` 内）。

**验证**

行为中性是被证明的，不是声称的——用 `rr.cpp` 的真实编译命令编了一个只含断言的 TU：

```cpp
static_assert(SCHEDULING_PERIOD_GRANS == 8);
static_assert(scheduling_period().raw() == 64'000'000);   // 与改前逐位相同
```

全量构建通过，`fair.cpp` 单独语法检查通过。QEMU 两个镜像各约 40 秒：
`kernel.dev.dbg_1.obi` 两线程交替 119487 / 119427 次，断言/panic/异常计数 0；
`kernel.dev.obi` userboot 正常，计数 0。

**故意没做**

- 没有改 `ISchedule::on_thread_init()` 的签名。上移常量是为它铺路（RR 读周期是它
  需要 `Runtime &` 的唯一理由），但改签名要动 `scheduler.cpp`，而那个文件此刻有
  别的会话未提交的改动。等它提交后再做。
- 没有修 64ms 那个 bug，理由同上。

## Runtime 加四个具名操作，纪律不再直接写它的字段（2026-09-17）

**文件**
- `kernel/include/ours/task/sched/runtime.hpp` — 新增 `charge_current()`、
  `restart_current_slice()`、`nominate_next_thread()`、`arm_preempt_timer()`
- `kernel/kernel/task/sched/rr.cpp`、`fair.cpp` — 改用这四个

**症状**

潜伏。`Runtime` 的字段全是公有，纪律就直接读写它们。两处后果已经发生了：

其一，**同一段逻辑被抄了两份并且已经漂移**。「把运行线程从上次记账到现在的时间算出来，
顺便把标记推到现在」这三行，`RoundRobin::charge()` 和 `Fair::update_thread()` 各写一份，
而边界条件已经不一样了——RR 在 `elapsed <= 0` 提前返回，Fair 在 `elapsed < 0`，
于是 Fair 会为一段零长度区间记账，RR 不会。

其二，**同一个答案有两条返回通道**。`evaluate_next_thread()` 已经**返回**下一个线程，
而 `yield_to_thread()` 里两个纪律又都写 `runtime.next_thread = next`。

**原因**

读和写从来就是一个操作，把它拆成两个公有字段访问，等于把「这两步必须一起做」这条约束
交给每个调用点自己记住——于是它们记岔了。Linux 正是把这几行从每个 sched_class 的
`update_curr()` 里提出来，放进 `core.c` 的 `update_curr_common()`，边界也在那里定死：
`if (unlikely((s64)delta_exec <= 0)) return;`。

**改法**

```cpp
// 改前（rr.cpp 与 fair.cpp 各一份）
auto const now = runtime.clock();
auto const elapsed = now - runtime.curr_started_time;
if (elapsed <= SchedDuration{0}) { return; }     // Fair 这里写的是 < 0
runtime.curr_started_time = now;
ctx.m_params.runtime += elapsed;

// 改后
auto const elapsed = runtime.charge_current();   // 零表示时钟没走，标记也不动
ctx.m_params.runtime += elapsed;
```

`charge_current()` 返回时长而不是自己记账，因为拿到之后要干什么是纪律的事——
RR 只加到 runtime，Fair 还要加权进 vruntime。

`restart_current_slice()` 和 `charge_current()` 刻意分开：前者用在**新**线程接管 CPU 时，
此时从标记到现在的这段属于被换下的线程、且已经记给它了；合并两者会让这段被记两次。

`nominate_next_thread()` 把定向让出那条通道收成一次具名写入。

`arm_preempt_timer()` 包住 `driver.reset_preempt()`，并把一条此前没人写下来的约束
写进注释：它编程的是**执行这次调用的那个 CPU** 的定时器，不是 `which_cpu` 的——
`Driver::reset_preempt()` 走 `TickDevice::Current::get()`，根本没有 CpuNum 参数可传。
今天所有调用点都在本地重调度路径上，两者重合；但递给纪律的 `Runtime` 并不总是本 CPU 的，
一旦从远程 Runtime 上调到它，定时器会落到错误的 CPU 上而毫无提示。加了 `DEBUG_ASSERT`
作绊线，并在注释里写明它在 `KERNEL_DEBUG_LEVEL=0` 下会被编掉，所以是绊线不是保证。

**关于 Fair 的两点交代。** 这次动了 `fair.cpp`，但只动接口用法，不动它的设计：

1. 边界从 `< 0` 变成 `<= 0`，这正是共用访问器要消除的那处漂移。差别只在时钟没推进时
   是否为零长度区间记账——记零和不记零结果相同，唯一的实际差异是标记不再被无谓地重写。
2. **刻意没有**在 Fair 里加提前返回。原来的代码在 `elapsed == 0` 时仍然会走到下面的
   deadline 检查，加了提前返回就会跳过它——那是控制流改动，不是接口迁移。
   现在的写法是「加零是空操作，直接落到 deadline 检查」，与原逻辑一致。

**参照**

`Linux update_curr_common()`（`kernel/sched/core.c`）、`update_curr()` 在各 sched_class
里的调用点、`yield_to_task_fair()` 的定向让出通道。

**验证**

全量构建通过；`fair.cpp` 用 rr.cpp 的真实编译参数单独语法检查通过（无 error 无 warning）。
QEMU 两个镜像各约 40 秒：`kernel.dev.dbg_1.obi` 两线程交替 122529 / 122530 次，
断言/panic/异常计数 0；`kernel.dev.obi` userboot 正常，计数 0。

**故意没做**

- 字段**还是公有的**。这一步只建立「有一条具名的路」，把字段私有化要改 `Runtime` 的
  形状并连带改 `scheduler.cpp`，等那边的未提交改动落地后单独做。
- 没有碰 `curr_thread`、`which_cpu` 的读取方式——它们是只读访问，私有化那一步再一起收。

## 调度时钟和真实时钟被交叉了（2026-09-17）

**文件**
- `kernel/kernel/task/sched/driver.cpp` — `m_prev_real_clock = m_sched_clock` 改为 `= now`

**症状**

潜伏，而且是那种长期统计量正确、所以永远不会被发现的潜伏。

`Driver::update_clock()` 每次被调用时，调度时钟推进的量应当等于真实流逝的时间。
实际推进量以**六次调用为周期**振荡。真实时钟每拍走 10 个单位时：

```
改前:  sched = [10, 30, 50, 60, 60, 60, 70, 90, 110, 120, 120, 120]
真实:  sched = [10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120]
```

每六拍总量正好也是 60——**长期速率精确等于 1.0**。但六拍里有两拍 `delta == 0`，
运行线程在那两拍里一点都没被记账，另外两拍被记了双倍。

**原因**

```cpp
auto const now = to_sched_time(SteadyClock::now());
auto const delta = now - m_prev_real_clock;   // 和「上一次真实读数」求差

m_prev_real_clock = m_sched_clock;            // ← 却存进了虚拟时钟
```

字段名写着 previous **real** clock，它存在的唯一理由就是让上面那个 `delta` 的含义是
「距上次更新真实流逝了多久」。把调度时钟存进去，下一次的 delta 就改成了从虚拟时钟起算，
而虚拟时钟落后于真实时钟——落后多少取决于上一拍推进了多少，于是形成振荡。

这不是笔误层面的问题，是**一个类同时拿着两个时钟**的必然产物：`Driver` 里
`m_sched_clock` 和 `m_prev_real_clock` 类型相同（`ffl::Fixed<i64, 0>`）、命名相近、
都在同一个三行块里被读写，交叉它们编译器一声不吭。

**改法**

一行：`m_prev_real_clock = now;`

同时把「跳过的那一拍去哪了」写进注释——这是读这段代码必然会问的问题。被 `ActSkip`
跳过的更新**不会**走到这里，所以它覆盖的那段区间会被折进下一次的 delta，而不是丢失。
这是「推迟时钟」和「冻结时钟」的区别，`request/accept` 那套握手要的是前者。

**参照**

`Linux update_rq_clock()`：`delta = sched_clock_cpu(cpu_of(rq)) - rq->clock;`
——同样是「本次真实读数减去上次记录」，而 Linux 把上次记录就存在 `rq->clock` 里，
只有一个时钟，所以这个错误在那边写不出来。

**验证**

**当前启动负载观察不到这个 bug，这一点必须说清楚**：两个测试线程分别在 cpu2 和 cpu3 上，
每个 CPU 只有一个可运行线程，`RoundRobin::update_thread()` 里
`queue.num_queued()` 为 0，抢占路径从不触发，所以时钟抖动不会改变任何可见行为。
（之前用「线程交替次数」当指标是错的——那 289us 的间隔量的是打印循环周期，不是调度片。）

所以证明靠确定性算术：上面那张逐拍表是按这段代码的真实逻辑模拟出来的，改后与真实时钟
逐拍相等。启动只用来证明没有回归：全量构建通过；`kernel.dev.dbg_1.obi` 35 秒，
断言/panic/异常计数 0（线程计数 102356/102212，改前 105736/105670，差异在串口吞吐的
噪声范围内，与调度无关）；`kernel.dev.obi` userboot 正常，计数 0。

**故意没做**

- 没有拆 `Driver`。它确实装着两件事（时钟 + 抢占定时器），而这个 bug 正是那个二合一
  的产物；但拆类是重构，这次只修 bug。
- 没有改 `ActSkip` 的语义。现有行为是「推迟」，看上去是有意的，没有证据说明应该是
  「冻结」，所以只把它写进注释而不去动它。

## 给 ISchedule 的每个形参命名（2026-09-17）

**文件**
- `kernel/include/ours/task/sched/schedule.hpp` — 16 个虚函数的匿名形参补上名字

**症状**

18 个入口点里只有 2 个说明了自己要在哪把锁下被调用（`enqueue_thread`、`dequeue_thread`，
schedule.hpp:87/94 的 `TA_REQUIRES(runtime.rqlock)`）。其余在同一把锁下被调用的那些，
一条注解都没有。

**原因**

**不是漏写，是写不出来。** clang 的 thread-safety 属性接受的是一个**表达式**，而匿名形参
没有表达式可写——于是

```cpp
virtual auto yield_thread(Runtime &, Thread *curr) -> void {}
```

这个声明上根本无法写出 `TA_REQUIRES(runtime.rqlock)`。两个写了注解的入口点，恰恰就是
仅有的两个给 `Runtime &` 起了名字的。因果关系是反过来的：不是它们特别重要所以被注解，
而是别的没法被注解。

于是「这个 hook 在哪把锁下运行」这个问题，今天只能靠 grep 调用链回答，而这正是这棵树
栽过跟头的地方。

**改法**

16 个虚函数的匿名形参一律补名，`Runtime &` 统一叫 `runtime`。零行为——形参名不参与
重载决议，也不要求 override 与基类一致；`-Wno-unused-parameter` 已在编译参数里，
空实现体不会因此告警。

顺带把语义也写进接口注释里：声明是契约被阅读的地方，所以 Linux 的 `sched_class`
钩子也全部具名（`enqueue_task(struct rq *rq, struct task_struct *p, int flags)`）。

**参照**

`Linux struct sched_class`（`kernel/sched/sched.h`），每个钩子的形参都具名。

**验证**

全量构建通过，无新增告警；`fair.cpp` 单独语法检查通过（override 的形参名不必与基类
一致，所以 fair.cpp 和 rr.hpp 一个字都不用改）。QEMU 两个镜像各约 32 秒：
`kernel.dev.dbg_1.obi` 断言/panic/异常计数 0；`kernel.dev.obi` userboot 正常，计数 0。

**故意没做**

- **没有加任何一条 `TA_REQUIRES`。** 这一步只是让它们变得可写。真加的时候还差一个前置：
  `lockdep::ScopedGuard` 整个是 `TA_NO_THREAD_SAFETY_ANALYSIS`（guard.hpp 自己写着
  「callers who need the analysis to see the locks should nest single Guards instead」），
  而 `Scheduler::tick_thread` 和 `Current::yield_to_thread` 正是用它取的 rqlock，
  `put_prev_thread`/`set_next_thread` 又埋在 `gktl::defer` 的 lambda 里——lambda 对
  分析器是独立函数。那两处要先换成嵌套 `Guard`，而那**不是**零行为（嵌套 Guard 对
  lockdep 可见，tick 路径的 pi_lock → rqlock 定序会第一次被真正校验）。
- 没有改 `rr.hpp` / `fair.cpp` 里 override 的形参名。语言不要求一致，而那两个文件
  属于别的改动范围。

## 给 Scheduler 自己的声明补上形参名（2026-09-17）

**文件**
- `kernel/include/ours/task/sched/scheduler.hpp` — 9 处声明的匿名形参补名

**症状**

和上一节同因。`ISchedule` 补完名之后，`Scheduler` 这一侧还有 9 个声明留着匿名形参：

```cpp
static auto on_thread_init(Thread *, BaseProfile const &, CpuNum target_cpu = INVALID_CPU_NUM);
static auto set_cpu_affinity(Thread *, CpuMask const &, CpuAffinity::Kind) -> void;
auto tick_thread(Thread *, SchedTime now) -> void;
auto activate_thread(Thread *, EnqueueControl) -> void;
...
```

**原因**

同上一节：`TA_REQUIRES` 接受的是表达式，匿名形参没有表达式，所以这些声明连想写锁契约
都写不出来。而它们恰恰是要写的那一批——`tick_thread`、`activate_thread`、
`deactivate_thread`、`do_unpark_thread` 全部在 rqlock 下运行，`do_unpark_thread` 还可能
作用在**远程 CPU** 的运行队列上。

另有一层：**定义里本来就有名字**，九个全部有（scheduler.cpp:113/211/395/420/431/731/
745/778 与 :938）。所以这不是「起名字」，是让声明说出定义早就说了的话。读头文件的人
今天要跳到 .cpp 才知道第一个 `Thread *` 是谁。

**改法**

九处照抄定义里的名字，一个不改：`thread` / `current` / `affinity` / `kind` / `control` /
`profile` / `f` / `args`。零行为——形参名不参与重载决议，也不影响 ABI。

**参照**

同上一节：`Linux struct sched_class` 的钩子形参全部具名。

**验证**

工作树全量构建通过、无新增告警；QEMU 两个镜像各约 32 秒，断言/panic/异常计数均为 0，
userboot 正常。

**提交方式要说明一下**：`scheduler.hpp` 此刻同时带着另一个会话未提交的整文件重排
（739 行）。按本仓规矩不能把别人的改动扫进我的提交，而 `git commit -- <path>` 提交的是
工作树内容，做不到只取其中几行。所以这一条是用独立索引提交的：从 `HEAD` 版本取出该文件、
只施加上述九处改名、`git hash-object -w` 得到 blob，在 `GIT_INDEX_FILE` 指向的私有索引里
`read-tree HEAD` + `update-index --cacheinfo`，再 `write-tree` / `commit-tree` /
`update-ref`。全程不触碰共享索引，也不触碰工作树里的重排。

**要提交的那份 blob 是单独编译验证过的**，不是只验了工作树的叠加版：把 rr.cpp 的真实
编译参数取出来，对那份 blob 跑 `-x c++-header -fsyntax-only`，无 error 无 warning。

**故意没做**

- 仍然没有加任何一条 `TA_REQUIRES`（前置未齐，理由见上一节）。
- 没有碰那 739 行重排，也没有碰 `scheduler.cpp`。

## scheduler.hpp 补上 `<utility>`（2026-09-17）

**文件**
- `kernel/include/ours/task/sched/scheduler.hpp` — 加一行 include

**症状**

潜伏，而且有时限。scheduler.hpp:339 与 :347 用 `std::to_underlying`，而该头只 include 了
`<array>` 与 `<atomic>`。现在能编过是靠传递包含——把编译命令加上 `-H` 打印包含树，
`<utility>` 出现在第 7 层，是别的头顺带拖进来的。

**原因**

自包含是头文件的基本契约：一个头该 include 它自己用到的东西，而不是指望别人替它 include。
靠传递包含活着的头，会在**与它无关的某次改动**中突然编不过，而那次改动的作者看不出因果。

这条特别有时限：队列搬进 `Runtime` 那一步要删掉 scheduler.hpp 里的
`#include <ours/task/sched/fair.hpp>`，而那很可能正是这条链的来源。先补上，
省得那一步被一个毫不相干的错误打断。

**改法**

```cpp
#include <array>
#include <atomic>
#include <utility> // std::to_underlying
```

按根 `.clang-format` 的第二组（C++ 标准库）字母序放。

**参照**

无，是本项目自身的自包含约定。

**验证**

全量构建通过；QEMU 两个镜像各 28 秒，断言/panic/异常计数均为 0，userboot 正常。

**故意没做**

- 没有顺手把这个头里别的传递包含依赖也一并补齐。这次只修被证实、且马上要被切断的那一条。

## `complete_context_switch` 的声明把参数叫错了（2026-09-17）

**文件**
- `kernel/include/ours/task/sched/scheduler.hpp` — 形参 `next` 改为 `curr`，补文档注释

**症状**

声明和定义说的不是一回事：

```cpp
// scheduler.hpp
static auto complete_context_switch(Thread *next) -> void;
// scheduler.cpp:538
auto Scheduler::complete_context_switch(Thread *curr) -> void {
    ...
    self.set_state(State::Idling, curr == self.m_idle_thread);
}
```

两个调用点传的都是**正在运行的那个**线程：`switch_context()` 传它自己的 `curr`，
`trampoline_handoff()` 传 `m_runtime.curr_thread`。

**原因**

这是改名之前留下的旧心智模型。它原来是成员函数 `finish_switch_context()`，由切换的
**近侧**调用——在那一侧，被命名的线程确实是「即将要跑的那个」，叫 `next` 没错。
改成静态、并把线程显式传进来之后，调用点移到了切换的**远侧**，同一个线程在那边已经是
「正在跑的那个」。名字得跟着语义走。

这类失配不会被编译器发现（形参名不参与任何检查），但读头文件的人会据此建立错误的模型——
而这个函数正好坐在上下文切换交接这条最需要正确模型的路径上。

**改法**

`next` → `curr`，与定义和 `ISchedule` 里「运行中的线程叫 curr」的惯例一致。同时补上
文档注释，把「为什么是 curr 而不是 next」和「为什么是静态」写下来——后者上面
`switch_context()` 那段长注释已经解释过：完成切换的调度器属于**现在**运行该线程的那个
CPU，所以不能用 `this`。

**参照**

`Zircon Scheduler::FinalContextSwitch()` 同样在切换远侧完成收尾并同样退出线程安全分析。

**验证**

纯命名 + 注释，零行为（形参名不参与重载决议，也不影响 ABI）。全量构建通过；
QEMU 两个镜像各 28 秒，断言/panic/异常计数均为 0，userboot 正常。

**故意没做**

- 没有改 `switch_context(Thread *curr, Thread *next)`。那个在切换近侧，两个名字都是对的。

## scheduler.hpp 重排的收尾：两行超出列宽（2026-09-17）

**文件**
- `kernel/include/ours/task/sched/scheduler.hpp` — 两个声明重新折行

**症状**

上一轮把这个头从 2 空格重排成 4 空格之后，`clang-format --dry-run -Werror` 仍报三处违规，
集中在两行：`on_thread_init` 与 `set_cpu_affinity` 的声明各有 106 列左右，超过根
`.clang-format` 的 `ColumnLimit: 100`。

**原因**

那次重排不是这套 `.clang-format` 产出的——把 HEAD 版本用仓库配置格式化一遍再和它对比，
差异正落在这两行的折行方式上。也就是说重排是手工或用别的配置做的，把两个长声明拉成了单行。

**改法**

对该文件跑一次 `clang-format -i`，只动这两处，共 7 行。折行方式改回配置想要的形态：
参数续行对齐到左括号，`-> void` 跟在末参数后。

**参照**

无，是仓库自身的格式约定。

**验证**

`clang-format --dry-run -Werror` 现在无输出。全量构建通过；QEMU 两个镜像各 28 秒，
断言/panic/异常计数均为 0，userboot 正常。

**故意没做**

- **没有**顺手把 `TA_RETURN_CAPABILITY(m_runtime.rqlock)` 从 `get_lock()` 那一行拆下来。
  按仓库「属性宏独占一行」的约定它该独占一行，但 `TA_*` 一族不在 `.clang-format` 的
  `AttributeMacros` / `Macros` 里，clang-format 看不见它们，所以现在拆了下一次格式化又会
  被合回去。要真正解决得先把这些宏加进 `.clang-format`——实测会波及 14 个文件、
  横跨 mm/time/sync/lockdep/concurrent，是独立的一件事。
- 没有格式化本目录下其它文件。
