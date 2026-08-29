# perf-instrument：内核采样剖析器（PMU + Dwarf 展开 + host 火焰图）

状态：已确认的意图（interview-me 复述于 2026-08-30，用户逐项确认）。

## 目标

perf 风格的内核采样剖析器：

- x86 PMU（unhalted core cycles）溢出中断周期性采样；无 PMU 环境（QEMU TCG）退化为
  scheduler tick 采样。
- 每个样本在内核内用 `.eh_frame` 的 Dwarf CFI 展开完整调用栈（用户明确要求 v1 就要
  Dwarf 展开，不做 FP-only 折衷）。
- 样本落入预分配 per-CPU 缓冲；中断上下文零堆分配、有界解析。
- 原始记录（未符号化 IP 栈 + tid/cpu/tsc）以十六进制行导出到串口；host 工具
  `prof-fold` 用内核 ELF 符号化并输出 folded stacks（可直接喂 flamegraph.pl）。

## 成功标准

QEMU（bringup.x86）→ `prof.start` 命令 → 跑负载 → `prof.dump` 导出 →
`prof-fold` 产出符号化火焰图数据，能定位函数与调用路径。

## 模块

### kernel/lib/unwind（namespace `unwind`，纯逻辑、host 可测）

- `cursor.hpp`：字节游标，ULEB128/SLEB128/DW-EH-PE 指针编码读取。
- `cfi.hpp/cpp`：CIE/FDE 解析 + DW_CFA 指令求值（def_cfa(_sf/_r/_offset)、
  offset(_extended/_sf)、val_offset、restore(_extended)、same_value、register、
  remember/restore_state、advance_loc1/2/4、set_loc；表达式类规则 bail = 截断该栈）。
- `fde_index.hpp/cpp`：FDE 索引（pc 范围 → FDE 偏移），构建用调用方提供的
  push-back 回调（内核用 ktl::Vec/kmalloc，host 测试用 std::vector —— 核心零分配）。
- `registers.hpp` / `unwinder.hpp/cpp`：`Registers{pc,sp,bp}`；`step()` 单帧推进，
  单调性/对齐/深度校验，最多 N 步。

数据源：内核 `.eh_frame`（pcrel 编码，加载即用，无需重定位；`.debug_frame` 的
absptr 编码需要重定位，v1 不进内存 —— 采样本来也不会展开到 asm 帧）。

### kernel/arch/x86：PMU 支撑

- `msr.hpp`：MsrRegAddr 增补 IA32PerfEvtSel0/IA32Pmc0/IA32PerfGlobalCtrl/
  IA32PerfGlobalStatus/IA32PerfGlobalOvfCtrl。
- 新 `arch/x86/pmu.hpp`：CPUID.0xA 检测（version/计数器数/位宽）、rdpmc 内联、
  架构事件常量（UnhaltedCoreCycles 0x3C）。
- `kernel/arch/x86/kernel/prof_pmu.cpp`：LVT PMI 编程（LvtPmi=0x340，向量
  ApicPmi，已有 IrqVec + IDT case）、PERFEVTSEL0/PMC0/GLOBAL_CTRL 编程、
  溢出处理（清 OVF、重装周期、采样）、跨 CPU 启停经 IpiGeneric 多播
  （faults.cpp 现有 case 挂钩）、`arch_handle_pmi(IrqFrame*)`。

### kernel/lib/prof（namespace `prof`）

- `format.hpp`（内核/host 共享 POD）：文件头 + `SampleRecord{kind,depth,cpu,tid,
  flags,tsc,ips[16]}`，固定 144B。
- `buffer.hpp/cpp`：per-CPU `SampleBuffer`（`CpuLocal::allocate<SampleBuffer>`），
  原子写游标 + 定长槽位，满则 `dropped++`（原子），`total` 计数。停采后消费，
  无撕裂。
- `sampler.hpp/cpp`：`start(period)/stop()/dump()`；状态机 Stopped/Starting/Running/
  Stopping（原子）；`on_scheduler_tick()` 定时采样回退；GKTL_INIT_HOOK 建索引
  （`Platform` 级）+ 分配 per-CPU 缓冲；GKTL_COMMAND prof.start/stop/dump。
- `dump.cpp`：停采后逐 CPU 遍历，COM1 直写十六进制行（无 logger 开销）。

### 挂点改动（内核核心，最小侵入）

- `faults.cpp`：ApicPmi case → `arch_handle_pmi(frame)`；IpiGeneric case 追加
  `x86_handle_prof_ipi()`。
- `tick_sched.cpp`：`TickSched::handle_event` → `prof::on_scheduler_tick()`。
- `kernel.ld`：`.eh_frame` 作为分配 section 放进 rodata 段（KEEP + 边界符号）。
- `kernel/products/BUILD.gn`：`config("unwind-tables")` =
  `-fasynchronous-unwind-tables`（clang 对 x86_64-unknown-none 默认关表，
  原策略是 debug-only-unwind-tables 只给 asm 发 .debug_frame）。
- `build/switch/kernel_switchset.gni`：configs 追加 unwind-tables（kernel.config
  之后，后置 flag 覆盖其 -fno-unwind-tables）。

### host 工具 kernel/lib/prof/cli/prof-fold

- 依赖 vendor/cxxopt（obit 先例）；host 工具链允许 std::vector/std::string。
- 内置最小 ELF64 解析（symtab + strtab，地址→符号，函数区间排序二分）。
  不用 eftl：它面向加载/重定位，符号转储只需百行自包含解析。
- 输入：`--elf <kernel.elf> --samples <dump.txt|bin>`；输出 folded stacks
  `tid;frame1;...;frameN count`（未解析地址用 `0x…` 占位）。

## 明确不做（v1）

- 用户态栈回溯（uspace 样本只记 ip + 标志位）。
- 多 PMU 事件复用（单事件 cycles）。
- always-on 生产化开销优化。
- GUI 火焰图（folded stacks + flamegraph.pl 即可）。

## 风险与对策

- PMU 在 QEMU TCG 下不存在 → tick 回退保证可演示。
- 中断上下文安全：采样处理全程关抢占/零分配/有界步数；处理期间清 GLOBAL_CTRL
  防递归 PMI。
- `.eh_frame` 进入镜像的体积成本（数百 KB 级，-g2 内核可接受）；GN arg
  `kernel_unwind_tables_enabled` 可关。
- unwind 坏数据 → 全部读取带边界检查，任何异常路径截断记录已收集的帧。
