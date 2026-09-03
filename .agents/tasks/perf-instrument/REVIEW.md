# perf-instrument：实施记录与评审

日期：2026-08-30（夜间自主执行）。意图复述经用户确认（见 DESIGN.md）。

## 交付物

### 新增库

| 位置 | 内容 | 测试 |
|---|---|---|
| `kernel/lib/unwind` | DWARF CFI 解析/求值/FDE 索引/单帧步进；零分配核心，MemoryReader 读内存 seam | 9 gtest（手工 CFI 字节流 + 模拟两帧链） |
| `kernel/lib/prof` | 采样剖析器核心：format（wire format）、per-CPU SampleBuffer、start/stop 状态机、PMU/tick 双源、串口 hex 导出 | 4 gtest（wire format 契约） |
| `kernel/lib/kshell` | gktl 命令注册表缺失的前端：串口 shell 线程 | —（bringup 工具，随内核跑） |
| `kernel/lib/prof/cli/prof-fold` | host 工具：解析 PROF1/SAMP hex 转储 + 最小 ELF64 symtab 符号化 + Itanium demangle + folded stacks 输出 | 用真实 vmours 镜像验证过 |

### 架构支撑与内核接线（最小侵入）

- `arch/x86/pmu.hpp`（新）：CPUID.0xA 检测、rdpmc、PERFEVTSEL/GLOBAL_CTRL 编程。
- `msr.hpp`：MsrRegAddr 增补 5 个 perf MSR。
- `prof_pmu.cpp`（x86/kernel，新）：prof::arch seam 实现；PMI 处理（先停计数防自采样，
  采完重装周期）；跨 CPU 启停经 IrqVec::IpiGeneric 多播 + quiesce 计数等待。
- `faults.cpp`：ApicPmi case 从打 log 改为调 `x86_handle_pmi(frame)`；IpiGeneric 追加
  `x86_handle_prof_ipi()`。
- `irq/mod.cpp/hpp`：per-CPU `current_frame()`（preamble 存、postamble 清）——Linux
  per-cpu irq-regs 同款通用设施，tick 回退采样靠它拿到被中断上下文。
- `tick_sched.cpp`：调度 tick 里挂 `prof::on_scheduler_tick()`（tick 源采样钩子）。
- `kernel.ld`：`.eh_frame` 作为分配 section 并入 rodata 段（KEEP + 边界符号），
  代价约 21KB；`.debug_frame` 保持非分配（离线用）。
- `kernel/products/BUILD.gn`：`config("unwind-tables")` =
  `-fasynchronous-unwind-tables`，GN arg `kernel_unwind_tables_enabled`（默认开）可关；
  `kernel_switchset.gni` 在 kernel.config 之后追加该 config（后置 flag 覆盖其
  `-fno-unwind-tables`）。clang 对 x86_64-unknown-none 默认关 unwind 表，原策略
  （debug-only-unwind-tables）只给 asm 发 .debug_frame —— 内核内展开 C++ 帧必须改这个。

## 验证结果

- `products/bringup:bringup` 全量构建绿；镜像内 `.eh_frame`（ALLOC，0x51e0B）+
  `__eh_frame_start/end` 符号确认存在。
- 18 个 host 测试全绿（unwind 9 + unwind 链 5 + format 4）。
- 过程中修掉的真 bug：FDE range 编码剥离 PCREL 会得到非法格式 0x0B（应保留格式位）；
  parse_cie 对 DWARF v4 字段顺序错误（addr/seg size 在 aug 字符串之前）；
  evaluate() 缺失紧凑操作码 0x80|reg（DW_CFA_offset）/ 0xC0|reg（restore）—— 编译器
  最常发射的形式；x86-64 psABI R=0x1B 的格式位 0x0B 实为 4 字节 sdata（与 DWARF 表
  0x07 并存处理）。
- clang-format（llvm-23）全量跑过；clang-tidy 对全部我创建/修改的文件跑过：核心
  unwind/prof/kshell 零自有告警；prof-fold 修掉 m_ 前缀/nodiscard/结构命名；链接器
  符号（__eh_frame_start 等）按规约加窄化 NOLINT 注明原因。其余告警全部位于未触碰的
  既有头文件，不属于本变更。

## 已知问题 / 边界

1. **QEMU 端到端冒烟被预先存在的启动失败阻塞**：HEAD（含并行会话的新提交）上
   `scripts/qemu-boot.sh` 在 bootshim 加载 physload ELF 时 panic
   （"Invalid physload ELF"，phys 环境下 panic 不能格式化所以看不到状态码）。
   已用 stash 验证：与本变更无关（干净 HEAD 同样 panic）。管线各环节已分别验证
   （镜像含 .eh_frame、测试绿、prof-fold 对真实 ELF 符号化/demangle/fold 正确）。
2. prof-fold 用最小 ELF64 解析（symtab+strtab），没有走 eftl —— 它面向加载/重定位，
   符号转储用不上；百行自包含解析更省。
3. 用户态线程栈回溯、多 PMU 事件复用、always-on 低开销化：v1 明确不做（见 DESIGN.md）。
4. 采样周期/缓冲容量（128 条/CPU）为编译期常量，未做 cmdline 可调。

## 并行工作的注意

执行期间发现另一会话在同一工作树并行提交（HEAD 从 25c5002 推进到 598ebf7，含全库
clang-tidy 命名修复）。本任务的改动全程未提交、未触碰其文件；若合并时
`sampler.cpp`/`mod.cpp` 等有交叉，以功能为准手动合。

## 附：启动链路修复（2026-08-30 追加，用户要求的 bug fix）

修复 `scripts/qemu-boot.sh` 启动三重故障的过程中，逐层剥出了 **7 个相互独立的 bug**，
启动链从"bootshim 加载 physload 即 panic"推进到"内核入口开始执行自身指令"：

| # | 层 | 根因 | 修复 |
|---|---|---|---|
| 1 | bootshim→physload | eftl LoadPolicy 要求 PT_LOAD 页映射不相交（权限不同的段不能共页，W^X 的合法要求），而 physload.ld/kernel.ld 段间只有 8 字节对齐 → OverlappingPageMappings | 两个 ld 在权限边界加 `ALIGN(MAXPAGESIZE)`（与 kernel.ld 原有约定一致；内核因此从 8 段变 9 段） |
| 2 | physload 运行时 | 恒等映射只覆盖 usable RAM，VGA 文本缓冲（0xB8000，legacy 洞）不可用 → println 写屏 #PF，EARLY_IDT 全零 → 三重故障，panic 输出时有时无 | x86 `arch_setup_aspace` 补映射 [0xA0000,0xC0000) |
| 3 | physload | EARLY_STACK 只有 16K，而 physload→physboot→内核全嵌套在同一栈上；溢出踩烂 EARLY_IDT 等相邻全局 → 各种诡异静默死 | 16K→256K |
| 4 | 打包 | kernel_package("physboot") 依赖 `.module`（loadable_module，无 distribution_entries metadata）而非 group → fini 清单为空，模块从未进 BootFS | 依赖改为 group `//kernel/phys:physboot` |
| 5 | eftl×4 | BasicImage 要求 e_entry 落在可执行段（ET_DYN 模块天然无入口）；MAX_LOAD_SEGMENTS=6 < 内核 9 段；visit_relative 对带符号重定位失败（GLOB_DAT 属于绑定阶段）；RelocationObserver 一律拒绝 DT_JMPREL（模块 -fno-plt 仍会生成 1 条） | 分别：ET_EXEC 才要求入口；12；跳过带符号项；PLT 拒绝改为仅对 ET_EXEC 生效 |
| 6 | 绑定 | bind_rela_table 写 GOT 时多加了 bias（MappedMemory 以模块虚地址为基准，内部已做平移）→ OutOfRange | 去掉 `+bias` |
| 7 | physboot→内核 | (a) 静态内核无 PT_DYNAMIC，relocate 视为错误 → 改 no-op；(b) **中间层 PTE 继承叶子段的 NX**：先映射只读段使 PML4E 带 NX，后映射的 .text 全部不可执行 → 内核入口取指 #PF；(c) 内核零基址链接但被加载到高端，constexpr vtable 等绝对指针未重定位 → 跳进未重定位地址（**未修**，属内核链接模型设计决策） | (a)(b) 已修：no-op + 非终端 PTE 永不携带 NX（x86 Pagings::make） |

诊断手段沉淀：`phys::print_hex`（无格式化运行时的十六进制打印）、panic 打印调用方
地址与 fmt 指针/长度（phys panic 无法格式化参数）、physboot 跳转前的入口 PTE 走查、
QEMU `-d int` 异常追踪（直接给出 RIP/CR2/错误码，定位 #PF 級别的故障最快）。

**遗留**：内核高端加载后的绝对指针重定位（设计决策：重链接到加载基址、运行时自
重定位、或内核侧 KERNEL_BASE_ADDRESS 机制补全）—— 属并行会话 `phys-module-and-obi-tree`
的活跃施工区，未越权代做。当前链路终点：内核入口第一条指令已能执行。

## 附二：phys 打印/panic 格式化修复（用户指出"print 不可能修不了"）

此前 boot 日志全是裸 `{}`，当时的 panic.hpp 注释声称"phys 无格式化运行时，只能丢参数"
—— 这个说法不成立：内核 logz4 用同一套 libc++ <format> 头跑得很好，缺的只是phys
镜像没链 <format> 依赖的库级符号。修复（三个环境行为统一为"都真正格式化"）：

1. `ours/phys/print.hpp`：删除 PHYS_ENVIRONMENT 分支，print/println/dprint/dprintln
   在所有环境走 `vprint(fmt, make_format_args(args...))`。println 内部调用必须
   **限定** `ours::phys::print` —— 参数把 std 拖进 ADL 后 C++23 的 std::print 会
   赢下重载决议，写进 freestanding 环境不存在的 stdout（这是"改了却没生效"的元凶之一，
   靠反汇编 println 体内间接调用发现）。
2. `ours/phys/console.cpp`：补 2 参 vprint，logz4 同款 BoundedOutput + 静态
   FORMAT_BUFFER，vformat_to 后一次性写所有已注册 console。
3. `ours/panic.hpp`：删除 PHYS_ENVIRONMENT 丢参分支；panic() 全环境走
   PanicSink（栈上有界缓冲，不分配）+ std::vformat_to。
4. `kernel/lib/libc/libcxx_format_glue.cpp`（新）：`template class
   basic_string<char>` + 私有成员模板 __assign_no_alias 的显式实例化定义
   （[temp.explicit] 豁免访问检查）+ 浮点 to_chars 桩（到达即 panic，内核同款
   约定；必须包进 _LIBCPP_ABI_NAMESPACE 才能对上 mangled name）。以
   `:format-glue` 目标链入 physload.core / bootshim.core / 每个 physload_module
   —— 不用整个 `libc` 目标，其 cpp_dependencies.cpp 与 ccrt 的 __cxa_* 重复。

验证：boot 日志从裸 `{}` 变为真实值（`physload at 0x304000, entry 0x319EB0`、
`loaded by 'GRUB 2.12'`、build-id 配对检查输出真实哈希）；panic 正确格式化状态码。
原 panic 诊断（caller/fmt 指针）保留为回退诊断。18 个 host 测试全绿；我的目标
clang-format/tidy 干净。构建全绿仅剩并行会话 mutex/OwnedWaitQueue 的 WIP 报错
（其文档自述待修）。

教训：改 print.hpp 时先建错了路径（kernel/phys/print.hpp，真身在
include/ours/phys/ 下）—— "改动没生效"的第二元凶；验证语义必须以目标二进制
（strings/llvm-nm/反汇编）为准，不能只看源文件。
