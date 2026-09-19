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

## 两处 ScopedGuard 换成嵌套 Guard（2026-09-17）

**文件**
- `kernel/kernel/task/sched/scheduler.cpp` — `tick_thread`、`Current::yield_to_thread`

**症状**

`Scheduler::tick_thread()` 是每个 tick 都要走的函数，它用 `ScopedGuard` 同时取线程的
PI 锁和运行队列锁。而 `ScopedGuard` 对**两种检查器都是隐形的**——guard.hpp 在类上就写着：

> Invisible to the thread-safety analysis: a capability attribute cannot name a
> parameter pack ... callers who need the analysis to see the locks should nest
> single Guards instead.
>
> Invisible to lockdep's validation for the same practical reason, and on its way out.

`Current::yield_to_thread()` 那处更直接：它用 `ScopedGuard` 只包了**一把**锁，而同一个
文件的 943、993 行包一把锁用的都是 `Guard`。

**原因**

`ScopedGuard` 的构造函数收的是参数包，而能力属性写不出 `TA_ACQUIRE(locks...)`，所以它
整个被排除在分析之外。代价是：用它取的锁，等于没有人知道被取过。这对每 tick 必经的那个
函数来说尤其贵——后面要给 `ISchedule` 的 16 个入口点加 `TA_REQUIRES(runtime.lock())`，
而调用它们的 `tick_thread` 如果自己都看不见持有了什么锁，那些注解就无从检查起。

**改法**

```cpp
// 改前
ScopedGuard lock_guard{current->get_pi_lock(), get_lock()};
// 改后
Guard pi_guard{current->get_pi_lock()};
Guard rq_guard{get_lock()};
```

**不是换顺序**：`ScopedGuard` 本来就按参数从左到右获取、从右到左释放，嵌套写法与它
逐字等价。换的是「谁能看见这个顺序」。

**验证，以及一处必须说清楚的更正**

我原本打算说「这不是零行为，因为 pi_lock → rqlock 的定序会第一次被 lockdep 校验」。
**那是错的。** 查证结果：

1. 本构建的编译命令里没有 `CONFIG_LOCK_DEP`——`lockdep_enable` 默认 `false`，
   所以 lockdep 处于 `Disabled`，什么都不校验。
2. 更进一步，把它打开也没用：`lockdep_enable = true` 配出来的树**链接失败**，
   `lockdep::current_lock_context()` 与 `lockdep::report_violation()` 是 lockdep 交给
   使用方实现的钩子，而内核从未实现它们（详见 `docs/repairs/260917/`）。

所以这次改动今天**确实是零行为**，真实收益只有一条：clang 的编译期 thread-safety
分析（它一直开着）现在能看见这两把锁了。lockdep 那份收益要等有人把钩子实现出来。

全量构建通过、无新增告警；QEMU 两个镜像：`kernel.dev.dbg_1.obi` 40 秒，线程交替
244599 次，断言/panic/异常计数 0，日志里无任何 lockdep/定序相关输出；
`kernel.dev.obi` userboot 正常，计数 0。

**故意没做**

- 没有实现 lockdep 的那两个钩子。那是独立的功能。
- 没有给 `tick_thread` / `yield_to_thread` 加 `TA_REQUIRES`。这一步只是让锁可见，
  注解是下一步。

## 把「选下一个线程」和「交接 CPU」拆开（2026-09-17）

**文件**
- `kernel/include/ours/task/sched/scheduler.hpp` — 新增 `pick_next_thread()` 声明
- `kernel/kernel/task/sched/scheduler.cpp` — 拆分函数体，去掉 `gktl/defer.hpp`

**症状**

`evaluate_next_thread()` 把两件事塞在一个函数里，靠一个 `gktl::defer` 让第二件事在中间
那六个 `return` 的每一个上都发生：

```cpp
Thread *next;                                    // ← 未初始化
auto const deferred = gktl::defer([this, curr, &next] FORCE_INLINE_ATTR() {
    put_prev_thread(curr);
    set_next_thread(next);                       // ← 按引用读它
});
if (next = m_runtime.next_thread; next != nullptr) { ... return next; }
...
```

两个后果，都不是假设：

1. **「每条路径都在返回前写了 `next` 吗」变成了读者必须逐个 `return` 去核对的问题**，
   而且**已经错过一次**——原代码里那段 `next = curr; return next;` 的注释记着：曾经有一条
   路径直接 `return curr` 而没写 `next`，于是 `set_next_thread()` 收到的是 null，
   把一个空线程标记成了 Running。
2. **运行队列最常被调用的两个 mutator 永远加不上锁契约**。clang 的线程安全分析把 lambda
   体当作独立函数、锁集为空（`guard.hpp` 里那条注释就是记这件事的），所以只要
   `put_prev_thread` / `set_next_thread` 是从 defer 里被调用的，给它们写
   `TA_REQUIRES` 就没有意义。

**原因**

一个函数装了两件事：**选谁**和**把 CPU 交给谁**。`defer` 是用来让第二件事跟上第一件事的
六个出口的——也就是说，那个未初始化变量加引用捕获的写法，是「两件事没分开」的直接产物。

**改法**

```cpp
auto Scheduler::evaluate_next_thread(Thread *curr) -> Thread * {
    auto *const next = pick_next_thread(curr);
    put_prev_thread(curr);
    set_next_thread(next);
    return next;
}
```

`pick_next_thread()` 是纯选择：六个 `return` 全部变成直接返回值，不再有共享变量，
所以「有没有写」这个问题**在语法上不存在了**，那段警告注释也随之删掉。
`Thread *next;` 连同 `gktl::defer` 和它的 include 一起消失。

顺手把 `if (next = m_runtime.next_thread; ...) { m_runtime.next_thread = nullptr; ... }`
收成 `std::exchange(m_runtime.next_thread, nullptr)`——取和清本来就是一个动作。

**参照**

`Linux` 拆的是同一条缝：`pick_task()` 负责选，`put_prev_set_next_task()` 负责交接
（`kernel/sched/core.c`）。

**验证**

全量构建通过、无新增告警。QEMU 两个镜像：`kernel.dev.dbg_1.obi` 35 秒，线程交替
214070 次，断言/panic/异常计数 0；`kernel.dev.obi` userboot 正常，计数 0。

**故意没做**

- **没有加 `TA_REQUIRES`。** 这一步只是让它们可被注解；注解是下一步，单独一个提交。
- 没有碰 `scheduler.cpp:491-497` 那个快路径判据（`num_queued(m_runtime) ==
  m_runtime.num_queued_threads`）。它在 RR 下恒假——`RoundRobin::set_next_thread()`
  把运行线程从 FIFO 上摘走，而 `num_queued_threads` 仍在数它，两个计数器数的是不同人群。
  但守卫和它守着的循环**并不等价**（守卫问线程自己那个 discipline，循环按数组序遍历所有
  已注册 discipline），所以这不是删一条死分支，是修一个用错计数器的判据。另立一条。

## 给运行队列锁保护的入口点写上契约（2026-09-17）

**文件**
- `kernel/include/ours/task/sched/schedule.hpp` — 9 个 `ISchedule` 虚函数加
  `TA_REQUIRES(runtime.rqlock)`（原有 2 条，合计 11 条 + `Schedule<>` 不涉及）
- `kernel/include/ours/task/sched/rr.hpp` — `RoundRobin` 的 7 个覆写各自重复契约
- `kernel/include/ours/task/sched/scheduler.hpp` — `pick_next_thread`、
  `evaluate_next_thread`、`put_prev_thread`、`set_next_thread`、`unpark_preempt`
  加 `TA_REQUIRES(get_lock())`

**症状**

18 个入口点里只有 2 个说了要在哪把锁下被调用。其余在同一把锁下运行的，读者只能靠 grep
调用链才知道——而这棵树已经在「哪个 CPU、哪把锁」上栽过两次。

**原因**

前两个提交（形参具名、`ScopedGuard` 换嵌套 `Guard`）已经把「写不出来」和「看不见」这两个
障碍拆掉了，这一条才做得成。

**改法**

按调用点逐个核对后加注解，而**不是**给所有虚函数一律加上。加的 9 个，它们的调用点都在
`Guard{get_lock()}` 的作用域内（`enqueue_thread`/`dequeue_thread` 本来就有）：
`yield_thread`、`yield_to_thread`、`unpark_preempt`、`evaluate_next_thread`、
`put_prev_thread`、`set_next_thread`、`update_thread`、`on_thread_unpark`、`num_queued`。

**刻意没加的**，每个都有理由：

- `on_thread_init` —— scheduler.cpp:171/194 **不持锁**调它，而且 :157 拿到的 `self`
  可能是远程 CPU 的。注解诚实之后这件事会自己显形，但那是另一件事。
- `on_thread_dead` —— 唯一的调用点是注释掉的（scheduler.cpp:213）。
- `on_thread_tick`、`override_thread_base_profile` —— 全树零调用点，按存根规矩原封不动。
- `select_target_cpu`、`migrate_thread`、`discipline` —— 不收 `Runtime`。

**覆写必须各写一遍**：C++ 的覆写不继承基类的线程安全注解，所以 `rr.hpp` 的 7 个覆写
各自重复。`fair.cpp` 的覆写没加——它不在构建里，而且属于作者自留范围。

**加注解之后编译器索要了上游契约**，逐层传播了两轮：先是 `pick_next_thread`、
`unpark_preempt`、`put_prev_thread`、`set_next_thread` 被要求声明持锁，加上之后干净。
**没有发现任何实际的违例**——这些路径的加锁本来就是对的，只是从来没被写下来过。

**验证**

关键在于区分「检查过且正确」和「根本没在检查」，两者在构建输出里长得一样。所以做了
**反向验证**：故意把 `tick_thread` 里的 `Guard rq_guard{get_lock()}` 拿掉，构建立刻报

```
scheduler.cpp:439: error: calling function 'update_thread' requires holding
SpinLock 'm_runtime.rqlock' exclusively [-Werror,-Wthread-safety-analysis]
```

还原后构建干净。注解确实带牙。

全量构建通过、无告警。QEMU 两个镜像：`kernel.dev.dbg_1.obi` 35 秒，线程交替 205018 次，
断言/panic/异常计数 0；`kernel.dev.obi` userboot 正常，计数 0。

**故意没做**

- 没有加 `TA_GUARDED` 到 `Runtime` 的字段上。那会是自然的下一步，也是新增 `-Werror`
  失败的最大来源：`Scheduler::init_first_thread()` 由 BSP 为每个 CPU 写**远程** Runtime 的
  `curr_thread` 与 `curr_started_time`，只持线程 PI 锁。三条出路（启动期取远程 rqlock、
  整条豁免、`assert_held()` 伪造）各自都该有自己的决定和自己的提交。

## 两个纪律抢同一个策略槽位会被静默覆盖（2026-09-17）

**文件**
- `kernel/kernel/task/sched/scheduler.cpp` — `Scheduler::init()` 的注册循环加两条断言

**症状**

潜伏。`m_schedules` 是按 `Discipline` 枚举索引的数组，注册循环直接赋值：

```cpp
m_schedules[static_cast<usize>(entry.discipline)] = entry.instance;
```

两条注册记录若带着同一个 `Discipline`，**第二条静默覆盖第一条**，输的那个纪律从此一次
都不会被调用，而且什么提示都没有。

今天 `Fair` 和 `RoundRobin` **都注册 `Discipline::Normal`**，它们互斥只靠
`kernel/kernel/task/sched/BUILD.gn` 里的一行注释——把两个一起链进去就是这个后果。

**原因**

Linux 不需要这条检查，因为它没有这次坍缩：`DEFINE_SCHED_CLASS` 给每个 sched_class
在链接脚本里各自一个槽，唯一性是**结构性**的。把它压平成一个按枚举索引的数组，
唯一性的义务就转移到了运行期，而没人接。

顺带一提：`Discipline::Deadline` 全树零引用，所以这个数组今天是两槽一活槽。

**改法**

循环里加两条 `DEBUG_ASSERT`：槽位号在范围内、槽位此前为空。

**为什么普通断言就够**，不必写成幂等式：`init()` 由 BSP 对每个 possible CPU 各调一次，
但它开头就是 `new (this) Self()`，而 `m_schedules` 带 `{}` 默认成员初始化——每次调用都
从空数组开始，重复调用不会自己踩自己。

**参照**

`Linux DEFINE_SCHED_CLASS`（`kernel/sched/sched.h`）与 `kernel/sched/vmlinux.lds.h`
里的 `SCHED_DATA` 段。

**验证**

全量构建通过。QEMU：`kernel.dev.dbg_1.obi`（`DEBUG_ASSERT` 开启）30 秒，线程交替
172469 次，断言/panic/异常计数 0——也就是说当前单纪律构建下断言成立；
`kernel.dev.obi` userboot 正常，计数 0。

（`DEBUG_ASSERT` 在 `KERNEL_DEBUG_LEVEL=0` 下会被编掉，所以这是 debug 镜像的绊线，
不是发布镜像的保证。）

**故意没做**

- 没有给 `Fair` 换一个 `Discipline` 值。它们本来就是一个槽的两种实现（RR 是 Fair 在
  bring-up 期间的替身），而且那是作者的设计决定。

## `Driver` 改名为 `SchedClock`（2026-09-17）

**文件**
- `driver.hpp` → `sched_clock.hpp`，`driver.cpp` → `sched_clock.cpp`（`git mv`）
- `runtime.hpp` — 成员 `driver` → `clock_source`
- `scheduler.cpp` — 11 处成员访问跟着改
- `kernel/kernel/task/sched/BUILD.gn` — 源文件名

**症状**

在内核里 `Driver` 意味着**设备驱动**。这个类是「本 CPU 的调度时钟 + 抢占定时器」，
名字既不指时钟也不指定时器，什么都没说。而且它埋在 `Scheduler::m_runtime.driver` 两层
之下，想找「调度器的时钟在哪」的人，一个词都搜不到。

**原因**

这不只是整洁问题。同一天早些时候在这个类里发现的时钟交叉缺陷
（`m_prev_real_clock = m_sched_clock`，见本文件相应一节）**正是这种名字帮忙藏住的东西**：
类里两个时钟同类型、名字相近、在同一个三行块里读写，而类名对「这里有两个时钟、别搞混」
一个字都不提。

**改法**

类改叫 `SchedClock`，成员改叫 `clock_source`，文件跟着改名（`git mv`，保留历史）。
同时补上类文档注释，写清三件事：它是 steady clock 的一个**可冻结**视图（被跳过的那次
更新所覆盖的区间折进下一次，而不是丢失）；对照物是 Linux 的 `rq->clock` /
`update_rq_clock()` 和 Zircon 的 `Scheduler::UpdateTimeline()`；以及 `reset_preempt()`
是唯一一个跟报时无关的成员——它留在这里是因为「下次什么时候被打断」和「现在几点」是同一口
气里问答的（Linux 也把 `rq->clock` 和 `hrtick_start()` 放在一起），但**真要拆这个类，
先看它**。

**参照**

`Linux rq->clock` / `update_rq_clock()` / `hrtick_start()`；
`Zircon Scheduler::UpdateTimeline()`。

**验证**

一处自己踩的坑记下来：第一次替换只改了 `.driver`，漏掉 `runtime.hpp` 里两处**裸**
`driver`（在 `Runtime` 自己的方法体内），**构建是失败的**，而我当时的检查脚本用
`grep` 过滤输出、退出码没查，差点把失败当成功、还拿旧镜像跑出了「通过」的启动结果。
改用显式的构建退出码判断之后：构建成功、无告警；`fair.cpp` 单独语法检查 0 错误
（它不引用这个类型，未受影响）。QEMU 两个镜像各 30 秒：`kernel.dev.dbg_1.obi`
线程交替 170560 次、断言/panic/异常计数 0；`kernel.dev.obi` userboot 正常、计数 0。

**故意没做**

- 没有拆这个类。它确实装着两件事，但拆类是重构，而这次只是让名字说真话。

## `Context` 改名为 `SchedEntity`（2026-09-18）

**文件**
- `sched/context.hpp` → `sched/sched_entity.hpp`（`git mv`），类 `Context` → `SchedEntity`
- `Thread::scheduling_context()` → `scheduling_entity()`
- `rr.*`、`scheduler.*`、`scheduler_inline.hpp`、`scheduler_pi.cpp`、`thread.*`、
  `wait_queue.cpp`、`owned_wait_queue.cpp` 共 73 处跟随
- 新的 `sched/context.hpp` —— 6 行迁移垫片

**症状**

`Context` 这个名字什么都没说。**什么的上下文？** 它是每线程的调度状态——亲和性、
队列归属、profile、时间片、各 discipline 的私有字段——而「上下文」这个词在内核里
还同时指上下文切换，于是 `switch_context`、`complete_context_switch` 和
`scheduling_context` 三个名字里的 context 指的不是一回事。

另有一层：下一步要把 `SchedParams` 按 discipline 拆成实体子结构，如果容器仍叫
`Context`、里面再放一个 `SchedEntity`，就是同一个概念两个名字——多一层没有内容的包装。

**原因**

命名。参照物两边都不叫 context：Linux 的每 discipline 部分叫 `sched_entity`
（`task_struct` 里并列放 `se`/`rt`/`dl`），Zircon 的整体叫 `SchedulerState`
（`Thread::scheduler_state()`）。这个对象**就是**线程的调度实体。

**改法**

`Context` → `SchedEntity`，文件跟着改名（`git mv` 保留历史），73 处引用跟随。

**`fair.hpp` 和 `fair.cpp` 一个字没改。** 它们是作者自留的，而 `fair.hpp` 又在构建里
（`scheduler.hpp` 以值成员持有 `RunQueue`），所以不能既不改它又让它编不过。解法是两个
**具名的**迁移垫片，而不是留个烂摊子：

- `sched/context.hpp` 变成 6 行转发头，含 `using Context = SchedEntity;`
- `Thread::scheduling_context()` 保留为转发到 `scheduling_entity()` 的内联

两处注释都写明了它们为谁存在、以及**删除条件**：作者把 Fair 拿回去改完名，这两样一起删，
届时不会再有任何引用。逐行适配清单写在 `docs/design/260918/kernel-task-sched.md`。

**参照**

`Linux sched_entity`（`include/linux/sched.h`）、`Zircon SchedulerState` 与
`Thread::scheduler_state()`（`kernel/include/kernel/thread.h:1169`）。

**验证**

改名用的是词边界正则（`(?<![A-Za-z0-9_])Context(?![A-Za-z0-9_])`），所以
`LockContext`、`switch_context`、`complete_context_switch` 都不会被误伤——改完专门查过。

构建成功（按退出码判定，不是按 grep 输出）；`fair.cpp` 单独语法检查 **0 错误**，
`git diff` 确认 `fair.hpp`/`fair.cpp` 未改动。QEMU 两个镜像各 30 秒：
`kernel.dev.dbg_1.obi` 线程交替 172372 次、断言/panic/异常计数 0；
`kernel.dev.obi` userboot 正常、计数 0。

**故意没做**

- 没有顺手拆 `SchedParams`。那是下一步（F1），单独一个提交。
- 没有碰 `RunQueueState`、`CpuAffinity`、`Metric`、`Timeslice` 这些嵌套类型的名字。

## F1：`SchedParams` 按所有者拆开（2026-09-18）

**文件**
- `kernel/include/ours/task/sched/sched_entity.hpp` — `SchedParams` → `FairState`，
  `runtime`/`prev_runtime` 上提为 `SchedEntity` 的直接成员
- `kernel/kernel/task/sched/rr.cpp` — 4 处跟随

**症状**

一个扁平结构体装着两批互不相干的字段，七个字段全是同一个类型：

```cpp
struct SchedParams {
    SchedTime     deadline;       // 零写入，三个读者
    SchedDuration prev_runtime;   // 框架记账
    SchedDuration runtime;        // 框架记账
    SchedDuration vruntime;       // EEVDF
    SchedDuration vdeadline;      // 四个写入，零读者
    SchedDuration vlag;           // EEVDF
    usize         generations;    // 零使用者
};
```

`deadline` 和 `vdeadline` 就是这么并排坐着的：同型、同名族、同一层，写错哪个编译器
都不吭声。这不是 Fair 的算法问题——**是一个结构体同时装着通用时间记账和 Fair 的虚拟
时间代数**，两批字段的读者互不相干，却共享一个名字和一种类型。

**原因**

前提是「`Discipline` 会长大」。那么每加一个 discipline，它的字段就并进同一个扁平结构体，
下一对 `x` / `vx` 迟早会发生。所有权必须变成类型，而不是命名约定。

**改法**

```cpp
// 框架的，每个 discipline 都这样记账
SchedDuration m_exec_runtime;
SchedDuration m_prev_exec_runtime;

// Fair 私有的
struct FairState {
    SchedTime deadline;
    SchedDuration vruntime, vdeadline, vlag;
    usize generations;
};
FairState m_fair;
```

字段叫 `m_exec_runtime` 而不是 `m_runtime`，因为 `m_runtime` 已经被 `Scheduler` 占了，
指的是每 CPU 的运行队列对象——毫无关系的两样东西。访问器 `runtime()` 暂时保留旧名
（改它要动 `fair.*`），已在设计文档里列为待你决定的一项。

**RoundRobin 没有 `RrState`**，因为它确实没有私有的每线程字段——它唯一的每线程数据是
队列钩子。**而钩子搬不进子结构**：侵入式容器要的是指向成员的指针，没有办法拼出一个
穿进嵌套结构体的成员指针。真要搬，队列就得改成持有子结构、再从子结构反推实体
（Linux 的 `task_of(se)`）——那是改每个 discipline 自己的运行队列，是它们各自的事。
这一条写进了成员声明的注释里，免得下一个人以为是漏掉了。

**参照**

`Linux struct sched_entity` / `sched_rt_entity` / `sched_dl_entity`——`task_struct` 里
一个 discipline 一个子结构，各自由读它的 discipline 拥有。

**验证**

构建成功（按退出码），无告警。`fair.hpp` 仍然编译通过——它只用访问器和钩子 traits。
QEMU 两个镜像各 30 秒：`kernel.dev.dbg_1.obi` 线程交替 158767 次、断言/panic/异常
计数 0；`kernel.dev.obi` userboot 正常、计数 0。

**`fair.cpp` 从这一步起语法检查不再通过，这是设计如此。** 那 11 处直接读写 `m_params`
的地方正是所有权收归 Fair 的那条线。逐行清单写在
`docs/design/260918/kernel-task-sched.md`，`git diff` 确认 `fair.hpp`/`fair.cpp` 未改动。
代价要说明：从此我失去了对 `fair.cpp` 意外损坏的检测能力，直到作者适配为止。

**故意没做**

- 没有删 `generations`（零使用者，按存根规矩保留，随 Fair 的状态一起搬）。
- 没有建空的 `RrState`。RR 今天确实没有私有每线程字段，造一个空结构体是发明结构。
- 没有改访问器 `runtime()` 的名字。

## `EnqueueControl` / `DequeueControl` 改名为 `*Options`（2026-09-18）

**文件**
- `schedule.hpp`、`scheduler.hpp`、`rr.hpp`、`types.hpp`、`scheduler.cpp`、`rr.cpp`、
  `scheduler_migrator.cpp`、`scheduler_pi.cpp` — 类型名 41 处，形参与局部名 13 处

**症状**

命名。这两个是**位掩码**——`SkipClock | WakeUp | Front`，调用方用它们说「这次入队/出队
要按哪些方式做」。「Control」听起来像一个控制器、一个句柄、或者某种会去施加影响的东西；
它们不是，它们是一组选项。

**改法**

```cpp
// 改前
enum class EnqueueControl { None, SkipClock = 0_bit, WakeUp = 1_bit, ... };
auto enqueue_thread(Thread *thread, EnqueueControl control) -> Status;

// 改后
enum class EnqueueOptions { None, SkipClock = 0_bit, WakeUp = 1_bit, ... };
auto enqueue_thread(Thread *thread, EnqueueOptions options) -> Status;
```

形参名一并从 `control` 改成 `options`——一个 `Options` 类型的形参叫 `control` 读起来是
拧的。只改了类型确实是这两个的那些函数：`enqueue_thread`、`dequeue_thread`、
`activate_thread`、`deactivate_thread`。`UnparkControl control` 原样未动，它不在这次
改名的范围里。

**参照**

无，是本项目自身的命名。（Linux 的对应物是 `enqueue_task(rq, p, int flags)` 的裸 int
flags；这里是类型化的位掩码，比它强，只是名字选错了。）

**验证**

构建成功（按退出码），无告警。QEMU 两个镜像各 30 秒：`kernel.dev.dbg_1.obi` 线程交替
170040 次、断言/panic/异常计数 0；`kernel.dev.obi` userboot 正常、计数 0。
`git diff` 确认 `fair.hpp`/`fair.cpp` 未改动——`fair.hpp` 根本不引用这两个类型，
所以这次不需要任何垫片；`fair.cpp` 的 8 处已加进设计文档的适配清单。

**故意没做**

- 没有动 `UnparkControl`。它是同族里剩下的一个，但不在你点名的范围里；要不要一起改
  由你定。（`NotifyControl`、`MapControl`、`UnmapControl`、`HarvestControl`、
  `AllocationControl` 在别的子系统里，更不在范围内。）

## `UnparkControl` 改名为 `UnparkOptions`（2026-09-18）

**文件**
- `types.hpp`（定义）、`schedule.hpp`、`sched/scheduler.hpp`、`task/scheduler.hpp`、
  `scheduler.cpp`、`scheduler_migrator.cpp`、`thread.cpp` — 类型 18 处、形参/局部 10 处

**症状**

同族里最后一个。理由和上一节一样：它是位掩码（`Initial | ThisCpu`），调用方用它说
这次唤醒要按哪些方式做，不是一个「控制器」。

**改法**

`UnparkControl` → `UnparkOptions`，形参 `control` → `options`（`unpark_thread` 两个重载、
`do_unpark_thread`、`unpark_preempt`）。

**验证**

构建成功（按退出码），无告警。QEMU：`kernel.dev.dbg_1.obi` 30 秒，线程交替 173657 次、
断言/panic/异常计数 0；`kernel.dev.obi` userboot 正常、计数 0。
`fair.hpp` 不引用它（`grep -c` 为 0），所以仍不需要垫片；`fair.cpp` 的 2 处已进适配清单。

**故意没做**

- 没有动别的子系统里的 `*Control`：`NotifyControl`（task/owned_wait_queue.hpp）、
  `MapControl`/`UnmapControl`/`HarvestControl`（kernel/lib/arch）、
  `AllocationControl`（kernel/mm）。它们同病，但不在这次点名范围内。

## F2：运行队列搬进 `Runtime`，按 `Discipline` 选取（2026-09-18）

**文件**
- `kernel/include/ours/task/sched/rr_run_queue.hpp` — 新文件，`RrRunQueue` 从 `rr.hpp` 拆出
- `kernel/include/ours/task/sched/rr.hpp` — 只剩 `RoundRobin`
- `kernel/include/ours/task/sched/runtime.hpp` — 持有两条队列，新增 `queue<Disci>()`
- `kernel/include/ours/task/sched/scheduler.hpp` — 卸下两个队列成员，访问器改为转发
- `kernel/kernel/task/sched/rr.cpp` — 删掉 `rq(runtime)` 辅助函数

**症状**

队列住在 `Scheduler` 上，而 discipline 手里拿到的是 `Runtime &`。于是它要取**自己的**队列，
得先把整个 `Scheduler` 反查出来：

```cpp
// rr.cpp，改前
auto rq(Runtime &runtime) -> RrRunQueue & {
    return Scheduler::of(runtime).rr_run_queue();   // container_of 往返
}
```

这条路不只是绕。它把整个 `Scheduler` 交到了 discipline 手上——`Scheduler::of()` 一旦拿到，
`num_runnable()`、`get_lock()`、`init()`、所有静态入口点都在射程之内。**为了拿一样本来就
该在手边的东西，交出了全部。**

**原因**

拥有权放错了层。队列是每 CPU 的调度状态，而 `Runtime` 就是那个「每 CPU 的调度状态」对象，
也正是框架递给 discipline 的那个。Linux 也是这么嵌的：`struct rq` 内联 `cfs_rq`、`rt_rq`、
`dl_rq`，而 `sched_class` 的钩子收的就是 `struct rq *`。

**改法**

队列按值搬进 `Runtime`，并由**框架**按 `Discipline` 选取：

```cpp
template <Discipline Disci>
FORCE_INLINE auto queue() -> auto & {
    if constexpr (Disci == Discipline::Normal) {
        return m_fair_queue;
    } else {
        static_assert(Disci != Disci, "no run queue is registered for this discipline");
    }
}
```

**键是框架自己的键**，这一点是要紧的。`Disci` 是 discipline 已经传给
`Schedule<Derived, Discipline>`、也已经用来注册的那个模板实参——身份只声明一次，
队列选取和注册槽位都从它导出。另一种写法是让框架去问 `typename Derived::Queue`，
那等于把闸门的钥匙交给被闸的一方：discipline 写一行 `using Queue = 别人的队列;`，
框架就照办。

**拆出 `rr_run_queue.hpp` 是必需的，不是整理**：`rr.hpp` include 了 `schedule.hpp`，
而 `schedule.hpp` include `runtime.hpp`，所以 `Runtime` 按值持有 `RrRunQueue` 会成环。
`fair.hpp` 不 include `schedule.hpp`，没有这个问题。

**一处如实说明**：`RoundRobin` 够不到 `queue<>()`。它是 Fair 在 bring-up 期间的替身，
注册在同一个 `Discipline::Normal` 槽位上，而两者队列类型不同——枚举分不开它们。
所以 `rr_run_queue()` 仍是具名访问器，注释里写明了原因，并指向 `Scheduler::init()` 里
那条拒绝两个 discipline 抢同一槽位的断言。要让枚举分得开，得先决定 `Discipline` 怎么
表达「RR 顶替 Fair」——那是设计决定，不是这次改动。

**参照**

`Linux struct rq`（`kernel/sched/sched.h`）内联 `cfs_rq cfs; rt_rq rt; dl_rq dl;`，
以及 `sched_class` 钩子收 `struct rq *` 的形状。

**验证**

构建成功（按退出码），无告警。**`Scheduler::of()` 现在在构建里零调用者**——只剩
fair.cpp:135 一处，而它不在构建里。QEMU 两个镜像各 30 秒：`kernel.dev.dbg_1.obi`
线程交替 170709 次、断言/panic/异常计数 0；`kernel.dev.obi` userboot 正常、计数 0。
`git diff` 确认 `fair.hpp`/`fair.cpp` 未改动；19 处适配已列进设计文档。

**故意没做**

- 没有删 `Scheduler::fair_run_queue()` / `rr_run_queue()`，也没把 `of()` 转私有。
  那是 F3，单独一个提交——这一步只是把队列挪到该在的地方。
- 没有引入 CRTP 转发层。队列既然已经在 discipline 手里那个对象上，就不需要有人再递一次。

## 哪个 discipline 占 `Normal` 槽位成为构建选项；`Scheduler` 卸下队列访问器（2026-09-18）

**文件**
- `kernel/kernel_params.gni` — 新 GN 参数 `sched_normal_discipline`（默认 `"rr"`）
- `kernel/products/BUILD.gn` — 变成 `SCHED_NORMAL_DISCIPLINE_FAIR` 宏，带合法性断言
- `kernel/kernel/task/sched/BUILD.gn` — 按参数编 `rr.cpp` 或 `fair.cpp`
- `kernel/include/ours/task/sched/runtime.hpp` — `NormalRunQueue` 别名跟着构建走，
  两条队列合成一条；删掉 `rr_run_queue()`
- `kernel/include/ours/task/sched/scheduler.hpp` — 删掉两个队列访问器
- `kernel/kernel/task/sched/rr.cpp` — 9 处改走 `queue<Discipline::Normal>()`

**症状**

上一步给 `RoundRobin` 单挖了一个具名 `rr_run_queue()`，理由是「它和 Fair 共用
`Discipline::Normal` 槽位但队列类型不同，枚举分不开」。

那是把症状当成了结构。作者澄清：**RR 是过渡实现，存在的意义是让内核其余部分先活着，
它一定会被 Fair 换掉。** 既然两者永远只链接一个，正确的表达不是给替身开后门，
而是让槽位的队列类型跟着构建走。

顺带一提，旧的选择方式是 `BUILD.gn` 里一行注释：「Put it back here and remove rr.cpp
to run on Fair again」——一次手工编辑源文件列表。

**改法**

```gn
# kernel_params.gni
sched_normal_discipline = "rr"   # 或 "fair"
```

```cpp
// runtime.hpp
#if SCHED_NORMAL_DISCIPLINE_FAIR
using NormalRunQueue = RunQueue;
#else
using NormalRunQueue = RrRunQueue;
#endif

NormalRunQueue m_normal_queue;   // 改前是并列的 m_fair_queue + m_rr_queue
```

于是 `queue<Discipline::Normal>()` 对**当班的那个** discipline 都成立，替身不再是二等公民。
对照物是本仓自己的 `MemMap`——同样是「两个互斥实现，构建期择一」。

`Scheduler` 那两个队列访问器随之删除（零调用者），因为它们的返回类型是写死的
`RunQueue` / `RrRunQueue`，而现在只有一个类型存在。

**一个本来没打算要的好处**：`fair.cpp` 的适配清单不用我手工维护了。
把参数切成 `"fair"`，fair.cpp 就进构建，**编译器自己把清单打出来**——41 条，
已生成写进 `docs/design/260918/kernel-task-sched.md`，不是抄的。

**参照**

`Linux` 的 sched_class 是全部编进去的，不是互斥的——这一条**不照 Linux**，
因为这里的互斥是过渡状态，不是长期结构。本仓自己的 `MemMap` 才是对照物。

**验证**

两个方向都验了：

- `sched_normal_discipline = "rr"`（默认）：构建成功、无告警；QEMU 两个镜像各 30 秒，
  `kernel.dev.dbg_1.obi` 线程交替 171795 次、断言/panic/异常计数 0；
  `kernel.dev.obi` userboot 正常、计数 0。
- `sched_normal_discipline = "fair"`：`gn gen` 成功，`fair.cpp` 确实进了构建并报出
  那 41 条待适配项——开关是真的在起作用，不是摆设。（临时构建树已删。）

`git diff` 确认 `fair.hpp` / `fair.cpp` 未改动。

**故意没做**

- 没有把 `Discipline::Deadline` 也接进开关。它今天零引用，是存根。
- 没有改 `Scheduler::of()` 的可见性——它现在零调用者，但转私有是独立一件事。

## F4（改过的）：封装 `Runtime`，删掉 `Scheduler::of()`（2026-09-18）

**文件**
- `runtime.hpp` — `struct` → `class`，14 个字段加 `m_` 前缀并转私有，
  `friend class Scheduler`；新增 `lock()`、`which_cpu()`、`current_thread()`
- `types.hpp` — 前向声明的 tag 跟着改（`-Wmismatched-tags` 是 error）
- `schedule.hpp`、`rr.hpp` — 19 条 `TA_REQUIRES` 改指 `runtime.lock()`
- `rr.cpp` — 2 处改用 `current_thread()`
- `scheduler.hpp` — 删掉 `of()`
- `scheduler.hpp`/`scheduler_inline.hpp`/`scheduler.cpp`/`scheduler_migrator.cpp` — 58 处字段名跟随

**原计划是错的，先说这个**

F4 原本写的是「`Runtime` 解散并入 `Scheduler`」，理由是 `Scheduler::of()` 用
`container_of` 能把它还原成 `Scheduler`，证明它没有独立身份。

**F2 之后这个理由不成立了。** 队列搬进 `Runtime` 之后，两边的分工变成了：

- `Scheduler`：派发层——`m_schedules` 注册表、idle 线程、状态掩码、上下文切换
- `Runtime`：运行队列本身——CPU 号、锁、时钟、队列、计数、线程三元组、迁移链表

这正是 Linux 的 `struct rq` 与驱动它的那层之间的分界。解散它等于把「纪律可以看的」
和「纪律绝不能看的」重新拌回一起——恰好是这一整串改动要消除的东西。

所以 F4 改成：**切断回边，把已经成型的边界封住。**

**改法**

`Scheduler::of()` 删除（构建里零调用者）。`Runtime` 变成 `class`，字段私有，
唯一 friend 是 `Scheduler`，公开面是实测出来的需要：

- `lock()`、`which_cpu()`、`current_thread()`、`clock()`、`queue<Disci>()`
- `charge_current()`、`restart_current_slice()`、`nominate_next_thread()`、
  `arm_preempt_timer()`、`mark_preempt_pending()`

`lock()` 带 `TA_RETURN_CAPABILITY`。**这一条不是风格问题**：clang 解析 thread-safety
属性的实参时**是过访问控制的**，所以 `TA_REQUIRES(runtime.m_rqlock)` 会是硬错误而不是
警告。19 条注解全部改指访问器，和 `Scheduler::get_lock()` 已有的做法一致。

**封住的边界，也说清没封住的**

改完之后**靠 C++ 访问控制硬编译失败**的：`runtime.clock_source.update_clock()`
（绕开调用方的 SkipClock 决定）、`runtime.total_weight -= w`（破坏 Scheduler 的聚合
记账）、`runtime.prev_thread = p`（上下文切换交接槽）、`runtime.migrating_threads_lock`、
以及 `Scheduler::of(runtime)` 本身。

**没封住的，如实列**：`runtime.lock().acquire()` 仍然编得过。能力必须公开可命名才能被
注解命名，所以任何写 `TA_REQUIRES(param.x)` 的设计都堵不住这一条——注释里写明了。

**刻意不 friend 任何 discipline**。C++ 的友元是整类的，`friend class Fair` 会把每个私有
成员都交出去，上面每一句「纪律碰不到」就全成了假话。

**参照**

`Linux struct rq`；`Zircon Thread::get_lock()` 同样是公开访问器套私有锁，
理由相同（属性实参要过访问控制）。

**验证**

反向验证过，不是只看构建通过：写了一个探针 TU 从纪律的位置访问 `rt.m_total_weight`，

```
error: 'm_total_weight' is a private member of 'ours::task::sched::Runtime'
```

全量构建成功（按退出码）、无告警。QEMU 两个镜像各 30 秒：`kernel.dev.dbg_1.obi`
线程交替 177397 次、断言/panic/异常计数 0；`kernel.dev.obi` userboot 正常、计数 0。
`fair.cpp` 适配清单已重新生成，44 条（上一轮 41 条，新增的是 `which_cpu`、
`curr_thread`、`of()` 三类）。

**故意没做**

- 没有解散 `Runtime`，理由见上。
- 没有给 `Runtime` 改名。它现在装的是 Linux 叫 `struct rq` 的东西，而 `Runtime`
  这个名字同样没说清是什么的 runtime；但 `RunQueue` 被 Fair 占着，改名要先定名字。
- 没有给字段加 `TA_GUARDED`。那是新增 `-Werror` 失败的最大来源——
  `Scheduler::init_first_thread()` 由 BSP 只持线程 PI 锁写**远程** Runtime 的
  `m_curr_thread`——每条出路都该有自己的决定和提交。

## `Runtime` 改名为 `RunQueue`（2026-09-19）

**文件**
- `sched/runtime.hpp` → `sched/run_queue.hpp`（`git mv`），类 `Runtime` → `RunQueue`
- 形参与局部 `runtime` → `run_queue`，`Scheduler::m_runtime` → `m_run_queue`
- `schedule.hpp`、`rr.hpp`、`rr.cpp`、`scheduler.*`、`scheduler_inline.hpp`、
  `scheduler_pi.cpp`、`scheduler_migrator.cpp`、`sched_entity.hpp`、`types.hpp`、
  `common.hpp` 共 231 处
- `scheduler.hpp` — 摘掉 F3 之后就不再需要的 `fair.hpp` include

**症状**

`Runtime` 什么都没说——**什么的 runtime？** 这个词在系统编程里通常指「运行时库」或
「运行期」，而这个对象是**每 CPU 的运行队列**：CPU 号、队列锁、调度时钟、当班 discipline
的队列、权重与线程计数、上下文切换的线程三元组、迁移链表。Linux 管这一整包叫
`struct rq`，而且它同样内含各 discipline 的子队列（`rq` 含 `cfs_rq`）——所以
「run queue」这个名字扛得住它的全部内容，不是勉强套上去的。

**改法**

框架层拿回 `RunQueue` 这个最正的名字，纪律的叫 `FairRunQueue` / `RrRunQueue`——
名字和层级完全对应。文件跟着改名（`git mv` 保留历史）。

`scheduler.hpp` 里那个 `fair.hpp` 的 include 必须摘掉：F3 之后 `Scheduler` 不再持有
Fair 的队列，而留着它会让 Fair 的 `RunQueue` 和框架的新名字在同一命名空间里撞车。

**`fair.hpp` 和 `fair.cpp` 一个字符没动**，代价是**默认（RR）构建下两个名字不冲突，
切到 Fair 构建就冲突**——因为 Fair 的类还叫 `RunQueue`。`NormalRunQueue` 的 Fair 分支
现在写的是 `FairRunQueue`（Fair 还没有这个名字），注释里写明了这就是适配清单里的一条。
这是作者本人点名要的走法：框架先拿正名，Fair 那半由作者改。

**参照**

`Linux struct rq`（`kernel/sched/sched.h`），以及它与 `cfs_rq` / `rt_rq` / `dl_rq`
的包含关系。

**验证**

构建成功（按退出码）、无告警。QEMU 两个镜像各 30 秒：`kernel.dev.dbg_1.obi` 线程交替
168015 次、断言/panic/异常计数 0；`kernel.dev.obi` userboot 正常、计数 0。
`git diff` 确认 `fair.*` 未改动；适配清单已重生成。

**故意没做**

- 没有把形参缩写成 `rq`。Linux 用 `rq`，但这棵树几乎不用缩写，而作者在选名字时也正是
  因为这一点否掉了把类型叫 `Rq` 的选项。
- 没有改 `SchedEntity::m_exec_runtime`——那是「执行时长」，和这个对象无关。

## 修上一条改名扫过头的地方，并补回被截断的适配清单（2026-09-19）

**文件**
- `kernel/include/ours/task/sched/sched_entity.hpp` — `run_queue()` 还原为 `runtime()`
- `docs/design/260918/kernel-task-sched.md` — 适配清单从 0 条补回 69 条

**症状一：改名扫到了不该扫的东西**

`Runtime` → `RunQueue` 那一轮，我把标识符 `runtime` 一律替换成了 `run_queue`，
于是 `SchedEntity::runtime()` ——**线程已执行时长**的访问器，和运行队列毫无关系——
被改成了 `run_queue()`。

**构建没有发现它。** 因为同一个提交里刚把 `fair.hpp` 移出构建（`scheduler.hpp` 不再
需要它），而 `fair.hpp:45` 的 `object.runtime()` 正是全树唯一的调用者。一次改动同时
制造了错误和移走了唯一的检查者。

**症状二：那张「0 条」的适配清单是假的**

同一轮我把清单重生成写进了设计文档，结果是 0 条。不是真没有——`fair.cpp:5` include 的
`runtime.hpp` 已随改名消失，那是 **fatal error**，编译在那一行就停了，后面 67 条根本
没机会被报出来。我把一张空表当成结论提交了。

**改法**

访问器还原成 `runtime()`（不趁机改名——那要动 `fair.*`，是清单里的一条，不是这次修复
的内容）。

清单改用一份**把那行 include 改好的 fair.cpp 副本**来生成，绕过 fatal 截断；
`fair.cpp:5` 本身作为第一条手工列在表首，并注明「不先修它，下面 67 条一条都看不到」。
`fair.cpp` 本体一个字符没动。

**教训**

两条都指向同一件事：**「构建通过」这个信号，在你同时移走了检查者的时候是空的。**
`fair.hpp` 出构建和误改访问器发生在同一个提交里，所以通过只说明剩下的代码自洽。
清单那条同理——`-ferror-limit=0` 挡不住 fatal error，出现 fatal 时错误计数根本不是
「还剩多少」。

**验证**

构建成功（按退出码）。QEMU 两个镜像各 28 秒：`kernel.dev.dbg_1.obi` 线程交替 153883 次、
断言/panic/异常计数 0；`kernel.dev.obi` userboot 正常、计数 0。
适配清单 69 条（1 条 include + 68 条编译器报出）。

## `RunQueue` 的形参叫 `rq`（2026-09-19）

**文件**
- `run_queue.hpp`、`rr.hpp`、`schedule.hpp`、`scheduler.hpp`、`rr.cpp`、`scheduler.cpp`
  共 86 处

**症状**

上一条改名时我把形参定成了 `run_queue`，理由是「这棵树几乎不用缩写，作者也正因此否掉了
把类型叫 `Rq`」。作者纠正：**形参可以用 `rq`，类名不能用 `Rq`。**

这个区分是对的，而我把它抹平了。类名是给读者建立概念的——`Rq` 对没读过 Linux 调度器的人
什么都没说。形参是在一个几行长的作用域里反复出现的局部名字，那里简短比展开更清楚，
而且 `rq` 在调度器语境里是通用词汇。

**改法**

```cpp
// 改前
TA_REQUIRES(run_queue.lock())
virtual auto enqueue_thread(RunQueue &run_queue, Thread *thread, EnqueueOptions options) -> Status = 0;
// 改后
TA_REQUIRES(rq.lock())
virtual auto enqueue_thread(RunQueue &rq, Thread *thread, EnqueueOptions options) -> Status = 0;
```

类型 `RunQueue` / `RrRunQueue` / `NormalRunQueue` 和成员 `Scheduler::m_run_queue`
都不动——只改形参和引用形参的局部名。替换用的词边界正则天然跳过 `m_run_queue`、
`rr_run_queue`、`fair_run_queue`（前面都是 `_`），`#include` 行整行跳过，免得把
`sched/run_queue.hpp` 改成 `sched/rq.hpp`；改完专门查过这两类误伤。

**验证**

构建成功（按退出码）、无告警。QEMU 两个镜像各 28 秒：`kernel.dev.dbg_1.obi` 线程交替
162994 次、断言/panic/异常计数 0；`kernel.dev.obi` userboot 正常、计数 0。
`git diff` 确认 `fair.*` 未改动；适配清单已重生成。

**故意没做**

- 没有把 `Scheduler::m_run_queue` 改成 `m_rq`。作者说的是形参；成员不在同一个作用域
  压力下，展开的名字在那里更清楚。

## `SchedEntity::runtime()` 改名为 `exec_runtime()`（2026-09-19）

**文件**
- `kernel/include/ours/task/sched/sched_entity.hpp` — 一个访问器

**症状**

字段早已叫 `m_exec_runtime`（因为 `m_runtime` 当时属于每 CPU 对象），访问器却还叫
`runtime()`，名实不符。而且那个撞名的对象现在叫 `RunQueue` 了，`runtime()` 这个名字
失去了它仅剩的解释。

**原因**

更要紧的是另一重歧义：它旁边就是 Fair 的 `vruntime`——**同一个量，用虚拟时间度量**。
`exec_` 这个前缀是承重的，不是装饰。

顺带记一条观察（不动它，Fair 是作者的）：这个访问器的调用者**全部**在 `fair.*` 里，
而且每一处都用在周围算术分明指虚拟量的地方——

```cpp
avg_vruntime() - object.runtime()          // fair.hpp:45
current.runtime() < min_vruntime_          // fair.cpp:78
ctx.deadline() <= ctx.runtime()            // fair.cpp:212
```

Fair 同时维护着 `vruntime`（它自己写了四处），却在比较时读实际执行时长。
和 `deadline`/`vdeadline` 那一对是同一个模式的两张面孔。改名不修它，但至少框架不再
暗示这两个量可以互换。

**验证**

构建成功——框架里零调用者，所以对构建免费。QEMU 两个镜像各 26 秒，断言/panic/异常
计数均为 0，userboot 正常。适配清单已重生成。
