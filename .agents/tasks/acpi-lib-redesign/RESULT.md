# kernel/lib/acpi 重新设计 —— 完成记录

日期：2026-08-18。配套：[REVIEW.md](REVIEW.md)（审核）、[DESIGN.md](DESIGN.md)（提案）。

用户确认的范围：库 + 调用方 + host 测试；TableSet 保持惰性、持有 mapper 加缓存；
HPET MMIO 寄存器块迁到 arch/x86。

## 改动清单

### kernel/lib/acpi（重写）
| 文件 | 说明 |
|---|---|
| `include/acpi/signature.hpp` | 新增。Signature 改为 POD 聚合体，`u32` 单成员 + `from_chars()` 工厂 |
| `include/acpi/types.hpp` | 瘦身，不再重复定义 prelude 已有的整数别名 |
| `include/acpi/table_set.hpp` | 新增。`PhysMapper` + `TableSet` + `entries<E>()` |
| `include/acpi/numa.hpp` | 重写。`for_each_memory_region` / `for_each_cpu_binding` / `DistanceMatrix` |
| `include/acpi/debug.hpp` | 重写。`DebugPort` / `parse_debug_port` / `find_debug_port` |
| `include/acpi/structures/root.hpp` | 原 `rdst.hpp`（拼写错误）|
| `include/acpi/structures/dbg2.hpp` | 原 `dbg2t.hpp`，改到 `acpi::dbg2` 命名空间 |
| `include/acpi/structures/{header,madt,srat,slit,hpet}.hpp` | 按 ACPI 6.5 修正，全部加 `static_assert(sizeof)` |
| `table_set.cpp` / `numa.cpp` / `debug.cpp` | 原 `parser.cpp` / `apic.cpp` / `numa.cpp` / 空的 `debug.cpp` |
| `test/*` | 新增 4 个 host 测试 + 合成表镜像的 fixture |
| 删除 | `parser.hpp` `apic.hpp` `parser.cpp` `apic.cpp` `rdst.hpp` `dbg2t.hpp` |

### 库外
- `kernel/arch/x86/kernel/include/ours/arch/hpet-regs.hpp` 新增（HpetRegs/HpetTimerRegs 从 acpi 迁出）
- `kernel/arch/x86/kernel/time/hpet.cpp`：改用 `arch::HpetRegs`，去掉那句 "This is a very inconvenient way"
- `kernel/phys/acpi.cpp` + `include/ours/phys/acpi.hpp`：`make_acpi_table_set`
- `kernel/platform/include/ours/platform/acpi.hpp`：`get_acpi_parser` → `get_acpi_tables`
- `kernel/platform/pc/{acpi,hpet,interrupt}.cpp`：适配新 API + 修陈旧的 `mem`→`mm` 拼写
- `kernel/arch/x86/mm/srat.cpp`：改用 `for_each_*` / `DistanceMatrix`
- BUILD.gn：`kernel/lib/acpi`、`kernel/platform`、`kernel/platform/pc`、`kernel/arch/x86/mm`、
  `kernel/arch/x86/kernel`、根 `BUILD.gn`

## 审核问题的处理

| 编号 | 处理 | 回归防护 |
|---|---|---|
| C1 RSDP 签名 `\|\|` | 改 `&&` | `test_rsdp_signature_must_match_both_halves` |
| C2/C3 SLIT 越界读 | `find<T>()` 统一长度检查 + `DistanceMatrix` 先验证后索引 | `test_table_shorter_than_struct_is_rejected`、`test_distance_matrix_rejects_lying_count` |
| C4 disabled CPU 当致命错误 | 改 `continue` | `test_cpu_bindings_skip_disabled` |
| C5 未初始化默认构造 | 构造函数私有 + 全员 NSDMI | 类型层面消除 |
| C6 MADT 类型枚举错位 | 按 ACPI 6.5 Table 5.20 重排 | `structures_test` 逐个 static_assert |
| C7 entry 布局错误 | 逐个按规范重写；捏造的 GIC 系列直接删除 | `structures_test` 的 sizeof + offsetof |
| C8 debug.cpp 空文件 | 实现 | `debug_test`（5 个用例）|
| C9 调用方用已改名的 API | 全部适配 | 见下方"未能验证"|
| C10 Signature union 双关 | 改 POD 聚合体 | `static_assert(is_trivial_v)` |
| D1 重复查表/映射 | 按签名 memoise | `test_lookup_is_memoised` 断言 mapper 调用次数不增 |
| D2/D3 六份重复循环 + 回调 | 一个 `entries<E>()` range | `test_lookup_and_entries` 验证 `break` 可用 |
| D4 `IAcpiParser` 假接缝 | 删除；`PhysMapper` 保留（两个真实 adapter）| — |
| D5 mapper 契约没写 | 写进接口注释 | fixture 的 mapper 带边界检查，越界读会直接失败 |
| D6 hpet.hpp 混装 | 寄存器块迁到 arch/x86；顺带修 `set_mode_oneshot` 的错误断言 | — |
| D7 零测试 | 4 个 host 测试，挂到 `//:default` | — |

## 顺带发现并修掉的（原审核没列到）

1. **`kernel/platform/pc/acpi.cpp` 根本不在 `sources` 里** —— `g_acpi_parser` 没有定义，
   `AcpiInit` 钩子从未被链接进去。
2. **`kernel/platform/pc` 整个 target 没有任何 `deps`** —— 所有源文件都死在 "file not found"。
3. **`ai_virt` 这个注解在全树都不存在**，`platform/pc/acpi.cpp` 却在用。
4. **`global_handoff()` 只存在于 `//kernel/phys`**，kernel proper 侧应该用 `boot::handoff()`。
5. **`ours/platform/init.hpp` 用了 `ustl::traits::UnderlyingTypeT` 却没 include** 对应头文件。
6. **`srat.cpp` 的 `bind_node_to_cpu` 无边界写** —— `apicids[count]` 在 `count >= MAX_CPU`
   时越界，SRAT 列出的 CPU 数超过 MAX_CPU 就会踩内存。已加上限判断。
7. **`.clang-format` 有一个不存在的键 `BreakAfterNamespace`** —— clang-format 22 会直接
   拒绝整份配置，也就是说这个仓库的 clang-format **从来没有真正跑起来过**。已删掉该行。

## 验证结果

全部在 `dev-common` 容器内执行。

```
host 测试（4/4 通过）
  structures-test    PASS
  table-set-test     PASS
  numa-test          PASS
  debug-test         PASS

编译通过
  kernel_x86.debug_level_0/obj/kernel/lib/acpi/acpi.stamp
  kernel.phys_x86/obj/kernel/lib/acpi/header.stamp
  kernel_x86.debug_level_0/obj/kernel/platform/pc/pc.acpi.cpp.o
  kernel_x86.debug_level_0/obj/kernel/platform/pc/pc.hpet.cpp.o
  kernel/phys/acpi.cpp（-fsyntax-only，用 kernel.phys_x86 的真实 flags）
  kernel/phys:physload

clang-format  30 个文件全部格式化
clang-tidy    改动文件上只剩 readability-identifier-naming 的 kFoo 告警
```

### clang-tidy 剩余告警的说明

`.clang-tidy` 里 `ConstexprVariableCase: UPPER_CASE`，要求 `kSignature` 写成 `K_SIGNATURE`。
但整个仓库用的都是 `kFoo`（`kAlignment`、`kAreaMagic`、`kBitsPerWord`…）；
拿一个完全没动过的文件 `kernel/arch/x86/kernel/time/time.cpp` 去跑，会报 **99 条**同样的告警。
也就是说这是配置与全仓库实际约定的冲突，不是本次改动引入的。
按"跟周围代码保持一致"的原则保留了 `kFoo`；要不要改配置或改全仓库，需要单独决定。

私有成员的 `m_` 前缀是配置和 acpi 库原有写法一致的，已按此改。

## 未能验证的部分（与本次改动无关的既有故障）

这三个文件的 ACPI 相关部分已经改好，但**当前树里编不过**，原因都不在 ACPI：

1. **`kernel/arch/x86/mm/srat.cpp`** —— `kernel/mm/include/ours/mm/pm-node.hpp`
   include 了 `kmrd/damon.hpp`，这个文件全树不存在。
2. **`kernel/platform/pc/interrupt.cpp`** —— include 了 `arch/x86/pic.hpp`，全树不存在；
   而且 `ktl/new.hpp` 引用了不存在的 `ktl::usize`。
   （用一个临时 PIC stub 试过，卡在 ktl 上。）
   其 MADT 解码路径改为在 `test_interrupt_source_override_decoding` 里固化。
3. **kernel proper 整体** —— `ustl/rc.hpp` 的 `RefCounted<VmAspace>` 推导错误，
   这是进入本次任务之前就存在的状态。

`srat.cpp` 用到的 ACPI 接口（`for_each_memory_region`、`for_each_cpu_binding`、
`DistanceMatrix`、`find<srat::Table>`）全部被 `numa-test` 覆盖。
