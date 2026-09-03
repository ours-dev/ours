# platform / cpu — 改动记录

格式见 `docs/changes/TEMPLATE.md`。这两条都是 `kernel/time` 那批工作顺带需要的公共设施，
各自独立成提交，因为它们纯增量、不依赖其他任何改动。

---

## 1. ISA IRQ 号从宏改成带类型的常量（2026-09-03）

**文件**
- `kernel/platform/include/ours/platform/isa.hpp` — 十个 `#define` 改成
  `namespace ours` 里的 `constexpr IsaIrqNum`

**症状**
无功能缺陷。

**原因**
`ISA_IRQ_PIT` 这类名字是裸 `#define`：没有类型（所以会不加抗议地参与它落进的任何算术）、
没有作用域、并且在每个 transitively include 这个头的翻译单元里占着自己的名字。
这正是 `.agents/skills/language/modern-cpp-development` 里「`constexpr` 定义常量、
宏只负责把编译期配置送进来」那条要禁的形状。

**改法**
`using IsaIrqNum = u8;` 加十个 `constexpr`，放在 `namespace ours`。两个调用点
（`time.cpp` 在 `ours::time`、`keyboard.cpp` 在 `ours`）都在这个命名空间之内，
所以一行不用改。

顺带把它们**是什么**写进了注释：这些是 **ISA** 号，即 ACPI interrupt source override 所用的
编号，`apic_io_isa_to_global()` 把一个映射成索引 IO-APIC 重定向表的 GSI，两者经常不同
（IRQ 0 常被投递到 GSI 2）。这就是 `arch_time_init()` 把 ISA 号交给 `request_irq()`、
把 GSI 交给 `apic_unmask_irq()` 的原因——它看起来像不一致，而不是。
我在审查里为此判错过一次（见 `x86-clock.md` 第 1 条），注释是为了别让下一个人再判错。

**参照** 无，是本仓库的编码约定。

**验证** 构建通过，clang-tidy 干净。这一条单独成提交（`7f69f00`）并单独构建过。

**故意没做**
没有做成 `enum class`：这些值要作为整数传给 `apic_io_isa_to_global(u8)` 和
`request_irq(VIrqNum)`，包成 enum class 只会在每个调用点添一次 cast。

---

## 2. 新增 `cpu_none_mask()` 与 `cpu_all_mask()`（2026-09-03）

**文件**
- `kernel/include/ours/cpu.hpp` — 与 `cpu_possible_mask()` 并列的两个常量掩码

**症状**
无。这是 `kernel/time` 需要的一块公共设施：`TickDevice` 的占位设备需要一个"不含任何 CPU"
的掩码（见 `kernel-time.md` 第 2 条——它靠输掉每一个比较而被替换，而不是靠调用点特判）。

**原因**
原来没有这样的常量，占位设备只能带 `nullptr` 掩码，于是每个比较点都要先判空。

**改法**
`cpu_none_mask()` / `cpu_all_mask()`，返回的是对**静态对象**的引用而不是临时对象，
因为调用方存的是指针——`ClockEventDevice` 记 `CpuMask const *` 作为亲和性，
临时对象会悬空。

`cpu_all_mask()` 是位集的全部容量（内核编译时的 CPU 上限），**比 `cpu_possible_mask()` 宽**
（后者是固件报告的实际存在的），这个区别和 Linux 一致。

**参照** `Linux cpu_none_mask` / `cpu_all_mask`（`include/linux/cpumask.h`）。

**验证** 构建通过。这一条单独成提交（`7f09d2f`）。

**故意没做**
一度做成 `CpuMask::NONE` / `CpuMask::ALL` 静态成员——`CpuMask` 是 `ustl::BitSet` 的别名，
要为此改 ustl（`none()` 又已经是"是否为空"的成员函数，名字会撞）。已撤回，
放在 `ours/cpu.hpp` 里和其他掩码访问器并列。
