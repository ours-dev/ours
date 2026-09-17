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
