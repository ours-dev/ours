# 评估:头文件迁移到 C++20 Module 的成本

状态:评估完成(2026-08-30)。只做了测量与分析,未改动任何源码。
范围假设:全库第一方代码(kernel + system 第一方库)迁移为 named modules;
vendored 第三方与 asm 消费的宏头保持文本头。

> 2026-08-30 增补:立项动机确认为**终止 include 传染对私有实现的间接暴露**。
> 按此目标的暴露面审计与方案校准见同目录 [ISOLATION-AUDIT.md](ISOLATION-AUDIT.md):
> 总成本不变,重心移到 export 面设计与枢纽头拆分;另有 2–4 天的
> GN check_includes 止血步骤建议在迁移前先做。

## 1. 结论

**总成本约 8–14 人周(≈2–3.5 人月),日历 6–10 周(2–3 人并行)。**
其中构建系统管道(GN 无原生 C++20 module 支持)是最大且风险最高的单项,
约占三分之一。建议先用 2–3 天在叶子库上做 spike 验证管道,再决定立项。

需要明确:**这个库现在的构建速度已经很快,模块化的构建提速收益的绝对值很小**;
立项理由应当是架构性的(显式 API 边界、终结头文件传染、宏泄漏控制),
而不是编译速度。这与 AGENTS.md "bleeding-edge 到极致"的定位一致。

## 2. 现状盘点(实测)

### 规模与形态

| 项 | 数值 |
| --- | --- |
| 头文件(第一方) | ≈ 543 个(kernel 371 + ustl 128 + system 其他第一方 ≈ 44) |
| 头文件(vendored boost.intrusive) | 117 个(保持文本头,不迁移) |
| 头文件代码量 / .cpp 代码量 | ≈ 101k 行 / ≈ 50k 行(2:1,头文件重型,大量 inline/模板) |
| .cpp 文件 | 387 个,平均 ≈ 130 行/TU(TU 很小) |
| 内部 include 边(头→头) | 2057 条,平均扇出 3.2 |
| 最长 include 链 | 16 层(task/scheduler.hpp) |
| include 循环依赖 | **0 个**(关键利好) |
| 头文件中非 include-guard 宏 | 419 个 |
| .S 汇编消费的宏头 | ≈ 15 个(macro_abi.hpp、arch/macro/*、mm/cfg.hpp 等),必须保持文本 |
| GN target | 56 source_set + 73 executable(内核、phys 三级链、host 工具、测试) |

### 工具链

- clang 23.1.0-rc3(pinned 在 prebuilt,与 `-std=c++2c` 搭配,模块支持成熟)
  - **缺口:prebuilt 里没有 clang-scan-deps**(bin/ 只有 17 个工具),
    这是模块依赖扫描(P1689)的标准工具,需要补齐。
- ninja 1.13.2:支持 dyndep,模块化构建的原生路径可用。
- GN 2530(自构建):上游 GN 至今无 C++20 named module 的原生支持。
  已有深度定制基础(ours_toolchain.gni、elf_link.py),自建管道可行但属长期负债。
- 内核侧 freestanding:`-ffreestanding -nostdinc++ -fno-exceptions -fno-rtti`,
  标准头来自极小集合(cstddef/type_traits/…),**不能用 `import std;`**,
  ustl 需要自己成为模块或保持文本头。

### 构建现状(基线,收益论证用)

实测:touch 扇入最高的 `system/public/ours/types.hpp`(被 90 个头文件引用),
bringup.x86 配置波及 **211 个 TU / 249 个构建步骤,墙钟 8.0 秒**(12 核并行,
平均 ≈ 0.35 s/TU)。全量重建估计 1–2 分钟量级。

即:当前"最坏情况"的头文件传染,代价是 8 秒,不是分钟级。
模块化后该场景预计降到 2–3 秒——绝对收益小。

(测量期间发现存量问题:`product-smoke/smoke.obi` 打包报
"item 0 must be the Executable entry point",与 obi/phys 那条线的在途工作有关,
与本次评估无关。)

## 3. 目标形态(推荐:混合双轨)

- **第一方 543 个头 → named modules**(`export module ours.xxx;`),
  按 source_set 组织模块单元;大子系统(task/sched、mm)可用 partition。
- **保持文本头**(包在导入方的 global module fragment 里):
  - boost.intrusive 117 个头(宏重型 config,第三方语义,不值得转);
  - asm 共享的 ≈ 15 个宏头(汇编无法 import);
  - ours/compiler.hpp 一类的属性/pragma 宏头、ABI/链接脚本常量头;
  - vendor(googletest/nlohmann_json/cxxopt)与 host 侧 libc++。
- ustl(128 头)是最佳首发对象:叶子库、无内核依赖、freestanding、自研,
  转成模块后全内核受益。

## 4. 工作分解与成本估算

| # | 工作项 | 内容 | 估算 |
| --- | --- | --- | --- |
| 0 | Spike(2–3 天) | prebuilt 补 clang-scan-deps;在 ustl 上手搓 P1689 scan → ninja dyndep → BMI 缓存管道;验证 clangd | 2–3 天 |
| 1 | 构建基础设施 | ours_toolchain.gni 扩展:scan 步骤、dyndep、按 flag 集隔离 BMI(freestanding vs host);module 命名规范;compile_commands/clangd 集成;CI BMI 缓存 | 2–4 周 × 1 构建工程师 |
| 2 | 头文件转换(自底向上) | ustl 128 头(1–1.5 周)→ ktl/arch/lib(1–2 周)→ kernel/include + mm/task/time/sync 模板重头(2–3 周)→ system 其他(0.5–1 周);平均每头 20–45 min + 调试 | 4–8 人周,可 2–3 人按子系统并行 |
| 3 | 宏清理 | 419 个非 guard 宏分类:asm 共享的保持文本;其余 constexpr/inline 变量化或下沉模块内部 | 0.5–1 周 |
| 4 | 稳定化 | ODR/可见性/链接错误清偿(模块会把隐藏问题显性化)、全配置 CI、构建时间对照、文档 | 1–2 周 |

**合计:约 8–14 人周(≈2–3.5 人月),日历 6–10 周(2–3 人)。**
若只迁 kernel 不迁 system 第一方,约 6–10 人周。
模块可逐 target 切换、与文本头双轨并存,迁移期间存量开发不阻塞。

## 5. 风险清单

1. **GN 无原生 module 支持(最高风险)**:自维护 dyndep 管道是长期负债,
   升级 GN 时需回归;这是与 CMake 3.28+(原生支持)相比的结构性劣势。
2. **clang-scan-deps 缺失**:需补 prebuilt(一次性成本,小)。
3. **BMI 与编译器版本强绑定**:prebuilt clang 已 pinned,可控;
   但 CI 缓存、跨机器分发要纳入考虑。sccache/ccache 对 BMI 无效。
4. **工具链体验**:clangd 的模块支持近年改进明显但仍偏实验性,需 spike 实测;
   clang-format/clang-tidy 按文本工作,不受影响。
5. **问题显性化**:模块会暴露现存 ODR/可见性/未导出依赖问题,
   在内核上调试成本偏高;已有 QEMU/bringup 测试可兜底,但仍需预留清偿时间。
6. **永远到不了 100%**:asm 宏头、boost.intrusive、ABI 头必须保持文本,
   架构上要接受"模块为主、文本为辅"的双轨长期共存。

## 6. 收益的诚实评估

- **构建时间:收益绝对值小。** TU 平均 130 行、12 核、最坏传染 8 秒——
  今天没有构建痛点。若立项动机只是编译速度,不建议做。
- **真正的收益在架构面**:export 与否成为显式 API 决策;头文件传染终结;
  宏泄漏被结构性地控制(只能靠 textual 头显式声明);模块边界与
  source_set 边界重合,倒逼 include 纪律固化。这与仓库
  "bleeding-edge 技术用到极致"的定位一致,是正当立项理由。
- 顺手得到:今后新增代码不再需要"要不要加 include guard/pragma once"这类
  讨论和评审负担。

## 7. 建议路线

1. **先 spike,再立项**(2–3 天):补齐 clang-scan-deps,在 ustl 上跑通
   scan → dyndep → BMI 全链路,同时验证 clangd 体验与 freestanding
   (-fno-exceptions)下的模块行为。spike 的产出即 Phase 1 的设计输入。
2. spike 通过 → 按 §4 分阶段推进,ustl 先行,随后 ktl/arch,再进入 kernel 核心。
3. 任一阶段保持双轨可回退:单 target 粒度切换,CI 全绿后再切下一个。
4. 决策检查点:若 spike 显示管道维护成本明显超出预期(例如需要长期 patch GN),
   降级方案是只把 ustl/ktl 模块化作为技术储备,主线维持头文件——
   当前构建速度并不强迫任何事。

## 8. 附录:测量方法

- 头文件/宏统计:`git ls-files` + `git grep`;include 图:Python 解析
  include 根(45 个 `-I` 目录)后建图,Tarjan 求 SCC(结果 0 环)。
- 重建放大:`touch system/public/ours/types.hpp` 后
  `ninja -C out/bringup.x86 -n` 计步,再实际构建计时(8.0s 墙钟)。
  未改动任何文件内容;out/ 内产物正常刷新。
- 构建失败为存量问题(obi 打包校验),与本测量无关。
