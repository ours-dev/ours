# ffl

定点算术：精确比值、确定的舍入、不用浮点。

名字取自 Fuchsia 的 FFL（Fixed-point Fixed-width Library），实现是本仓库自己的，
**命名一律按本仓库的规范**——类型 `CamelCase`，方法/参数/局部量 `lower_case`，
`constexpr` 常量 `UPPER_CASE`，私有成员 `m_` 前缀。所以这里是 `from_ratio()` 而不是
ffl 的 `FromRatio()`。

## 为什么存在

内核没有浮点（见 `//system/BUILD.gn` 里 `-msoft-float` 那段注释），而整数除法丢掉的
恰好是调度器需要的那部分：

```cpp
auto const rate = weight / MID_WEIGHT;   // Weight = usize，整除
return duration * rate;                  // weight < MID_WEIGHT ⇒ rate == 0
```

任何轻于平均权重的线程，加权时间恒为零——不管它跑了多久。

## 结构

    include/ffl/saturating.hpp   饱和加减乘、钳位窄化
    include/ffl/wider.hpp        乘除用的加宽中间类型
    include/ffl/format.hpp       位宽拆分与收敛舍入
    include/ffl/fixed.hpp        值类型 Fixed<Integer, FractionalBits>

头文件库，全部 `constexpr`，没有 `src/`。

## 用法

```cpp
#include <ffl/fixed.hpp>

using Weight = ffl::Fixed<i64, 16>;          // Q47.16

auto const rate = Weight::from_ratio(weight, MID_WEIGHT);
auto const weighted = (rate * Weight{duration}).round();
```

## 两个刻意的选择

**收敛舍入（ties-to-even）**。这些值是被反复累加的——调度器每秒把加权时长累进虚拟时间
上千次。任何朝固定方向断开平局的规则，误差都会随累加次数线性增长；ties-to-even 两个方向
各半，误差有界。`AccumulatingHalvesDoesNotDrift` 这个单测就是钉这一条的。
IEEE 754 的默认舍入、ffl 的选择，都是同一个理由。

**没有表达式模板**。ffl 用惰性求值让复合表达式按赋值目标的精度挑中间精度，那是它 1823 行
里的 492 行。本内核调度器的算术是二元的，显式的加宽中间类型（`wider.hpp`）就够了，
不必要求每个读代码的人先理解一棵惰性求值树。等出现三元链条时再回头补，而不是拿嵌套强制
类型转换硬凑。
