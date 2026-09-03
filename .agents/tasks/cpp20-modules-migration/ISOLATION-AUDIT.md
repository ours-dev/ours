# 审计:头文件时代的"私有实现间接暴露"有多严重

背景:立项动机确认为**终止 include 传染导致的私有实现被动暴露**。
本文用实测数据回答三个问题:暴露面有多大、问题集中在哪、modules 能不能对症。
测量方法与 ASSESSMENT.md §8 相同(2026-08-30)。

## 1. 暴露面量化

### 传递闭包:include 一个头 = 实际看见多少个头

| 分位 | 头文件的闭包大小 |
| --- | --- |
| median | 5 |
| mean | 16.3 |
| p90 | 47 |
| **max** | **173**(`kernel/object/include/ours/object/process_dispatcher.hpp`) |

最坏的十个头全部来自 task/sched 与 mm/object 枢纽:

```
173  object/process_dispatcher.hpp      167  task/idle_thread.hpp
172  task/sched/scheduler_inline.hpp    166  task/sched/runtime.hpp
171  task/scheduler.hpp                 165  task/auto_preempt_disabler.hpp
170  task/sched/scheduler.hpp           164  task/thread.hpp
168  task/sched/schedule.hpp            162  mm/vm_object_paged.hpp
```

### 一个 .cpp 被动看见的头(n=387)

median 23 / mean 49 / **p90 162 / max 204**。
典型 TU 以为 include 了三五个头,实际要处理上百个——
"超出预期的间接暴露"就是这个数字与直观之间的差。

### 好消息:公共 prelude 层是干净的

- `system/public/ours/types.hpp`:闭包 0(叶子)
- `system/public/ours/status.hpp`:闭包 1
- `kernel/lib/arch/include/arch/types.hpp`:闭包 3

即:传染不是无差别弥漫,而是**集中在枢纽头**——task/sched、mm、object
的"公共"头把各自依赖的私有实现整棵拖进来。谁 include 了 scheduler 链,
mm/object 的内部实现就全部可见。

### 直接越界引用(核心子系统互相渗透,排除库依赖)

量级不大、无恶性互缠:mm→kernel:public 18、task→kernel:public 17、
arch→mm 8、task→mm 7、kernel:public→mm 4……
说明直接 include 纪律尚可,**问题主要在传递层**,与闭包数据一致。

### 第三方内部头被直接引用(detail/ 越界)

**27 处**第一方代码直接 include `boost/intrusive/detail/*`
(mpl.hpp ×10、iterator.hpp ×7、config_begin/end ×7、其余 3 处)。
这是对第三方未承诺 API 的真实越界,modules 管不到(boost 保持文本头),
需要在头文件时代单独清偿(1–2 天,可与迁移并行,建议先做)。

### 宏泄漏(不随 modules 消失的部分)

扇入 ≥20 的头有 16 个,合计携带 **59 个非 guard 宏**——
被包含即全局可见(KERNEL_BASE、PHYS(x)、GDT 选择子一类)。
modules 不导出宏,asm 共享的宏头必须保持文本:
这部分泄漏只能靠"显式包含少数文本头"收敛,或把非 asm 共享的宏 constexpr 化。

## 2. modules 对症在哪(机制说明)

- 模块单元里**未 `export` 的声明对导入方不可达**——不是"不建议用",
  是名字根本不可见;未导出的模块单元/partition 外部根本无法 import。
  "私有实现"从约定变成硬边界。
- import 边显式化后,"谁依赖谁的私有面"在代码里可数、可评审,
  且与 GN target 边界一一对应,越界会直接编译失败。
- 枢头头的伞形 include(scheduler.hpp 一类)可以拆成 partition:
  公共 API 面导出,实现细节留在未导出 partition,消费者只见前者。

三个不随 modules 消失的边界,要如实计入:
1. **宏**:不导出,文本宏头永久存在(靠显式包含收敛);
2. **boost.intrusive detail 越界**:第三方保持文本,modules 无感,需单独清;
3. **双轨期**:迁移完成前,文本头时代的老问题继续存在。

## 3. 按此目标对方案的校准

- **总成本不变**(8–14 人周):转换是同一批头文件;
- **重心平移**:从"机械转换"移到 **export 面设计**——按子系统逐个决定
  导出面 vs 模块内部,这是"终止暴露"真正兑现的环节;
  枢空头(task/sched、mm、object)优先做伞形拆分,新增 0.5–1 周;
- **推进顺序调整**:机械转换仍须自底向上,但 export 设计和验收标准
  以本次审计的闭包榜为序,先啃 173/172/171 这一批;
- **建议立即做的止血(2–4 天,迁移开始前就有效)**:
  GN 已有 `check_includes` 机制(全库只在 1 处使用且显式关闭),
  为每个 source_set 开启它并配 `public`/`public_configs`,
  把"新增越界 include"在头文件时代先掐住,防止迁移期间边界继续恶化。

## 4. 验收指标建议

- 枢纽头闭包:迁移后 import `ours.task.scheduler` 的 TU 被动可见头数
  从 ~170 降到 0(只见导出面);
- TU 暴露面:p90 从 162 降到 ≤ 个位数(导出面 + std/文本头);
- detail/ 越界引用:27 → 0;
- 热宏头:非 asm 共享宏 59 → 0(constexpr 化),文本宏头显式清单化。
