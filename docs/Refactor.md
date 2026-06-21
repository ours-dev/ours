# 内核重构设计文档

## 概述

本文档基于对 `kernel/` 下全部子系统（phys、arch、mem、sync、task、time、irq、object）及 `system/lib/` 的深入剖析，提出分层重构方案。核心原则：

1. **分层隔离**：每层仅依赖下层接口，禁止跨层调用
2. **接口先行**：先定义层间 C++ Concept 协议，后实现具体类
3. **渐进补全**：标注每个类的实施优先级 (P0 = 阻塞性, P1 = 核心功能, P2 = 优化)
4. **从 Zircon/Linux 借鉴**：命名和架构对齐成熟内核，降低学习曲线

---

## 总体分层架构

```
┌─────────────────────────────────────────────────────────────┐
│  Layer 5: 系统调用接口 (syscall)          [规划中]           │
├─────────────────────────────────────────────────────────────┤
│  Layer 4: 内核对象系统 (object)           kernel/object/     │
├─────────────────────────────────────────────────────────────┤
│  Layer 3: 内核核心服务 (main)                              │
│    ├── task/  调度与线程                                    │
│    ├── time/  时钟与定时器                                  │
│    └── irq/   中断分发                                      │
├─────────────────────────────────────────────────────────────┤
│  Layer 2: 内核基础 (main)                                  │
│    ├── mem/   内存管理 (PMM + VMM)                          │
│    └── sync/  同步原语                                      │
├─────────────────────────────────────────────────────────────┤
│  Layer 1: 平台初始化 (phys)            kernel/phys/         │
├─────────────────────────────────────────────────────────────┤
│  Layer 0: 硬件抽象 (arch)              kernel/arch/         │
├─────────────────────────────────────────────────────────────┤
│  Layer -1: 支持库                      system/lib/ +        │
│            bits / lockdep / acpi       kernel/lib/           │
└─────────────────────────────────────────────────────────────┘
```

依赖方向：**上层 → 下层** (Layer 5 只能依赖 Layer 4↓，依此类推)

---

## Layer -1: 支持库

该层是纯计算/数据变换，**不持有内核状态，不分配内存，不持有锁**。

| 类/模块 | 所属目录 | 用途 | 设计原因 | 状态 | 优先级 |
|---------|---------|------|---------|------|------|
| `bits::bitmask` | `system/lib/bits/` | 枚举位掩码运算 `\|` `&` `~` | SFINAE 探测 + 用户自定义字面量 `3_bit`，消除 C 风格 `#define` 位掩码 | 已实现 | — |
| `lockdep::LockClass` | `system/lib/lockdep/` | 锁类元数据（标志、依赖图节点） | 每个锁代码位置对应一个 LockClass 静态实例，`id()` 返回 `this` 指针作为全局唯一标识 | 已实现 | P0 |
| `lockdep::LockContext` | `system/lib/lockdep/` | 每线程持有的锁链表，acquire 时执行排序验证 | 核心锁序检查逻辑：Leaf 违规、重入检测、跨类依赖记录 | 部分实现 | P0 |
| `lockdep::LoopDetector` | `system/lib/lockdep/` | 锁依赖图环路检测 (DFS/BFS) | lockdep 的灵魂——缺少它等于没有死锁检测，仅做了排序验证 | **空壳** | P1 |
| `lockdep::Guard<T>` | `system/lib/lockdep/` | 单锁 RAII 守卫 | 编译期推导锁类型 CTAD，支持 `unlock()` 提前释放和 `unguard()` 禁用析构释放 | 已实现 | — |
| `lockdep::ScopedGuard<Ts...>` | `system/lib/lockdep/` | 多锁有序获取 RAII 守卫 | 防止 ABBA 死锁：按 LockClass ID 排序后依次获取 | **桩代码** | P1 |
| `lockdep::LockDep<Class, Lock, Tag, Flags>` | `system/lib/lockdep/` | 锁集成模板（被具体锁继承/组合） | 将 lockdep 元数据注入到 SpinLock/Mutex 等具体锁中 | **空骨架** | P1 |
| `lockdep::StateSaver` | `system/lib/lockdep/` | 锁获取/释放时的 IRQ/抢占状态保存 | ADL 查找机制 + `LOCKDEP_STATE_SAVER` 宏，生成 `SpinLockIrqSave` 等 | 已实现 | — |
| `acpi::AcpiParser` | `kernel/lib/acpi/` | ACPI 表解析器 (RSDP → XSDT/RSDT → table[]) | 偏好 XSDT 回退 RSDT；通过 `IPhysToVirt` 接口解耦物理内存映射 | 已实现 | — |
| `acpi::AcpiTable*` | `kernel/lib/acpi/` | 15 种 MADT 条目 + SRAT + SLIT + HPET + DBG2 结构定义 | 紧凑打包 (packed struct) + `downcast<T>` 类型安全下转 | 已实现 | — |
| `acpi::enumerate_*` | `kernel/lib/acpi/` | 模板化 ACPI 条目枚举器 | 回调模型 `Fn<Status(Entry const&)>`，避免内存分配 | 已实现 | — |
| `acpi::get_debug_port` | `kernel/lib/acpi/` | 从 DBG2 表提取调试端口配置 | 启动早期输出所需 | **桩代码** | P2 |

**重构要点**：
- `lockdep::LoopDetector` 是最高优先级的补充项——无环路检测等于 lockdep 残缺
- `lockdep::LockDep<T>` 模板需完成，使 `SpinLock<Monitored>` 能真正接入 lockdep 依赖追踪
- ACPI 库缺少表缓存（README 自述"反复解析同一表"），建议添加 `AcpiTableCache` 单例

---

## Layer 0: 硬件抽象层 (HAL) — `kernel/arch/`

该层定义编译时协议（C++20 Concept + `#include` Guard），上层通过 `#include <ours/arch/xxx.hpp>` 调用。每种架构提供独立的实现目录。

### 0.1 接口协议

| Concept / 协议 | 声明位置 | 约束的方法 | 设计原因 |
|---------------|---------|-----------|---------|
| `concepts::ArchVmAspace<T>` | `kernel/main/include/ours/mem/concepts/arch-vm-aspace.hpp` | `init`, `map`, `map_bulk`, `unmap`, `protect`, `query`, `mark_accessed`, `harvest_accessed`, `switch_context` | 确保架构提供完整的页表操作语义 |
| `concepts::ArchThread<T>` | `kernel/main/include/ours/task/concept/arch-thread.hpp` | `switch_thread(prev*, next*)` | 确保上下文切换可用 |
| `#include <ours/arch/xxx.hpp>` | 各通用头文件 | 头文件必须存在于架构目录 | 缺失时 `#error` 编译失败，强制架构移植完整性 |

### 0.2 x86_64 实现类

| 类 | 所属目录 | 用途 | 设计原因 | 状态 | 优先级 |
|---|---------|------|---------|------|------|
| `ArchVmAspace` | `arch/x86/main/mem/` | 管理一个 x86 地址空间的页表 (PML4) | 封装 `PageTable<...>` 实例 + PCID 分配；内核/用户/EPT 三种模式 `init()` 分支 | 完整实现 | — |
| `X86Thread` (= `ArchThread`) | `arch/x86/main/task/` | 线程寄存器上下文 (SP, FS, GS) | `switch_thread` 更新 TSS.sp0 → 交换 FS/GS → 汇编 `x86_switch_context` | 完整实现 | — |
| `ArchCpuLocal` | `arch/x86/main/include/ours/arch/` | GS 段基址读写 (`wrmsr IA32_GS_BASE`) | 每 CPU 数据存储的硬件基础，极低延迟单指令访问 | 完整实现 | — |
| `X86PerCpu` | `arch/x86/main/include/ours/arch/x86/` | 每 CPU 数据：TSS、APIC ID、IST 栈 | `CPU_LOCAL` 宏声明，链接器自动复制 | 完整实现 | — |
| `ArchIdleThread` | `arch/x86/main/task/` | CPU 空闲指令 (HLT/MWAIT) | `idle()` 函数体当前为**空**——HLT 未实现 | **桩代码** | P1 |
| `X86PageAllocator` | `arch/x86/main/mem/` | 为页表自身分配/释放物理页 | `PageTable` 的模板参数之一，调用 PMM `alloc_frames` | 完整实现 | — |
| `X86MmuPageSynchroniser` | `arch/x86/main/mem/` | TLB 无效化 (INVLPG) | 收集脏页 → 批量 invlpg；全局刷新用 `INVPCID` 或写 CR3 | 完整实现 | — |
| `X86EptPageSynchroniser` | `arch/x86/main/mem/` | EPT (扩展页表) TLB 同步 | VMX 嵌套虚拟化所需 | **空壳** | P2 |
| `ApicTimerEvent` | `arch/x86/main/apic/` | LAPIC 定时器作为 `ClockEventDevice` | 支持 one-shot / periodic / TSC-deadline 模式，校准算法借用 PIT/HPET | 完整实现 | — |
| `IoApicChip` | `arch/x86/main/apic/` | IO-APIC 中断控制器抽象 | 向通用 IRQ 层暴露 `IrqChip` 接口 (mask/unmask/startup/eoi/affinity) | 完整实现 | — |
| `HpetClockSource` | `arch/x86/main/time/` | HPET 主计数器作为 `ClockSource` | 64-bit 单调计数器，频率从 HPET 周期字段计算 | 完整实现 | — |
| `HpetChannel` | `arch/x86/main/time/` | HPET 比较器通道作为 `ClockEventDevice` | 支持 one-shot 和 periodic 模式 | 完整实现 | — |
| `BootstrapData` | `arch/x86/main/` | AP 引导数据 (PGD, GDT, 入口, 栈顶) | 传递给 16-bit → 64-bit trampoline | 完整实现 | — |
| `ArchHandOff` | `arch/x86/phys/include/` | 架构特定交接数据 | 当前为**空结构体**——应填充预配置的 GDT/TSS 地址、trampoline PGD | **空壳** | P1 |

### 0.3 架构自由函数

| 函数 | 用途 | 状态 |
|------|------|------|
| `init_arch_early()` | IDT 早期设置 + per-CPU + MMU 早期 | 完整实现 |
| `init_arch()` | GDT + IDT 完整安装 + MMU setup | 完整实现 |
| `arch_handle_exception()` | 异常/中断入口 (从汇编调用) | 完整实现 |
| `arch_mp_reschedule(cpu)` | 发送重调度 IPI | 完整实现 |
| `arch_mp_send_ipi(cpu, event)` | 发送通用 IPI | 完整实现 |
| `arch_mp_suspend(cpu)` | 挂起目标 CPU | **桩代码** |
| `arch_activate_cpu(cpu)` | CPU 上线 (热插拔) | **桩代码** |
| `arch_deactivate_cpu(cpu)` | CPU 下线 (热插拔) | **桩代码** |
| `arch_init_irq()` | 架构 IRQ 初始化 (IO-APIC 设置) | **桩代码** |
| `arch_time_init()` | 启动 PIT 作为初始时钟 | 完整实现 |
| `arch_build_uspace_irqframe()` | 构造用户态中断帧 | 完整实现 (内联) |
| `arch_enter_uspace()` | 进入用户态 (`swapgs` + `iretq`) | 完整实现 |
| `x86_handle_ipi_generic()` | 通用 IPI 处理 | **桩代码** |
| `x86_handle_ipi_interrupt()` | 中断 IPI 处理 | **桩代码** |

**重构要点**：
- `ArchHandOff` 必须填充——当前内核主体接收不到 GDT/TSS/trampoline 的物理地址
- `ArchIdleThread::idle()` 需实现真正的 HLT/MWAIT
- 五个 `arch_` 前缀桩函数阻塞了 CPU 热插拔和 IRQ 初始化

---

## Layer 1: 平台初始化 — `kernel/phys/`

该层是在内核主体运行之前的独立阶段，自包含（不链接内核主体代码）。

### 1.1 核心类

| 类 | 所属模块 | 用途 | 设计原因 | 状态 | 优先级 |
|---|---------|------|---------|------|------|
| `bootmem::MemBlock` | `phys/lib/bootmem/` | 早物理内存分配器 (region-based) | 对标 Linux `memblock`：支持 bottom-up/top-down、NUMA 感知、bounded alloc、自举扩容 | 完整实现 | — |
| `bootmem::RegionList` | `phys/lib/bootmem/` | 区域列表管理 (插入/合并/移除/对齐) | `MemBlock` 内部使用两个实例：`memories_`(可用) + `reserved_`(已分配) | 完整实现 | — |
| `bootmem::RegionVector` | `phys/lib/bootmem/` | Region 的动态数组存储 | 满时回调 `MemBlock` 获取更多空间——自举技术 | 完整实现 | — |
| `phys::Aspace` | `phys/` | 引导期临时地址空间 (恒等映射) | 双层页表模型 (Lower/Upper Paging)，支持 dual-aspace (x86 4-level+5-level) | 完整实现 | — |
| `phys::Console` | `phys/` | 早控制台抽象基类 | `REGISTER_CONSOLE` 宏自动注册 + `init_early_console()` 激活全部驱动 | 完整实现 | — |
| `phys::SerialConsole` | `arch/x86/phys/` | 16550 UART 串口驱动 | 端口 0x3F8, 波特率 115200 | 完整实现 | — |
| `phys::VgaConsole` | `arch/x86/phys/` | VGA 文本模式 (80x25, 0xB8000) | BIOS 兼容输出 | 完整实现 | — |
| `phys::KernelBoot` | `phys/` | 内核镜像加载器 | init → load → build_aspace → handoff 四阶段流水线 | 完整实现 | — |
| `phys::ElfImage` | `phys/` | ELF 镜像加载 + 重定位 | 解析 PT_LOAD 段 + `R_X86_64_RELATIVE` 重定位以支持 KASLR | 完整实现 | — |
| `phys::RawImage` | `phys/` | Raw 镜像加载 | 简单 memcpy | 完整实现 | — |
| `phys::KernelPackage` | `phys/` | 从 OBI 中解压内核包 | 使用 `obi::copy_item` 解压，第三个参数(分配器)有 TODO | 部分实现 | P2 |
| `phys::Handoff` | `phys/` | 向内核主体交接的数据结构 | ≤1 页约束；包含 `EarlyMem` 函数对象 (类型擦除的 BootMem 访问) | 完整实现 | — |
| `phys::BootOptions` | `phys/` | 启动选项 (KASLR, load_target) | 全局单例 `g_boot_options`，当前 `aslr_disabled` 强制为 true | 最小实现 | P2 |
| `phys::Symbolize` | `phys/` | 符号化支持 | 空壳——用于早期崩溃时的符号解析 | **空壳** | P2 |
| `phys::InitFs` | `phys/` | init 文件系统 | 仅 `using InitFs = Span<u8>` 别名 | **占位符** | P2 |

### 1.2 重构要点

| 问题 | 方案 | 优先级 |
|------|------|------|
| `obi_init_memory()` 中有 TODO | 实现从 OBI Memory item 解析 bootmem 区域 | P1 |
| `ElfImage::fix_mapping()` 为桩 | 实现页面权限修复 (需 huge page split 能力) | P1 |
| CpuTopologyNode 为空 | 填充 CPU 拓扑 (Cluster → Core → Thread)，供调度器使用 | P1 |
| `ArchHandOff` 为空 | 传递预配置的 GDT/TSS/trampoline PGD 地址 | P1 |
| `KernelPackage::unpack` 第三参数为 TODO | 实现解压分配器回调 | P2 |

---

## Layer 2: 内核基础 — `kernel/main/mem/` + `kernel/main/sync/`

该层是内核的"运行时基础"，提供内存分配和并发控制。Layer 3+ 的所有子系统都依赖此层。

### 2.1 内存管理 (mem)

#### 2.1.1 物理内存模型

| 类 | 所属文件 | 用途 | 设计原因 | 状态 | 优先级 |
|---|---------|------|---------|------|------|
| `MemSection` | `mem/memory-model.hpp` | 稀疏内存 Section 树节点 | 3 级 radix tree 覆盖完整物理地址空间；BitFields 压缩状态/类型/级别到 64-bit | 完整实现 | — |
| `MemoryModel` | `mem/memory-model.hpp` | 稀疏内存模型单例 (PFN→Frame 转换) | `init()` 遍历 `EarlyMem` 区域构建 section 树；`init_memmap()` 初始化 PmFrame 描述符 | 完整实现 | — |
| `PageFrame` | `mem/pm-frame.hpp` | 物理页描述符基类 | BitFields 压缩 role/flags/order/zone/node 到单个字；原子引用计数 | 完整实现 | — |
| `PmFrame` | `mem/pm-frame.hpp` | 伙伴系统管理的页面 (role=Pmm) | 侵入式链表节点，挂在 FrameSet 的空闲链表中 | 完整实现 | — |
| `VmPage` | `mem/vm_page.hpp` | VMO 管理的页面 (role=Vmm) | 携带 `m_vmo` 逆向指针 + `m_vmo_index` + 映射计数——COW 页的基础 | 完整实现 | — |
| `PmFolio` | `mem/pm-frame.hpp` | 复合页 (order>0, role=Folio) | 使用 `second_metadata` 跟踪 buddy 的高阶页 | 完整实现 | — |

#### 2.1.2 伙伴分配器

| 类 | 所属文件 | 用途 | 设计原因 | 状态 | 优先级 |
|---|---------|------|---------|------|------|
| `FrameSet` | `mem/pm-zone.hpp` | 阶位伙伴分配器核心 (order 0..MAX_ORDER) | `acquire_frame_locked` 向上搜索后分裂；`release_frame_locked` buddy 合并。SpinLock 保护 | 完整实现 | — |
| `FramePcpuCache<T>` | `mem/pm-zone.hpp` | 每 CPU 空闲帧缓存 (小 order) | 对标 Linux per-CPU page cache (`pcp`)，减少锁争用；当前 `kMaxPcpuCacheOrder=0` 未启用 | **未启用** | P2 |
| `PmZone` | `mem/pm-zone.hpp` | 单一 Zone 内的帧分配器 | 封装 FrameSet + watermark 检查；`alloc_frame` 快速路径 + `alloc_frames` 慢速路径 | 部分实现 | P0 |
| `ZoneQueues` | `mem/pm-node.hpp` | Zone 遍历队列 (Local/Shared) | 四种队列类型：本地顺序、本地连续、共享亲和性、共享顺序——NUMA 分配策略的数据基础 | 完整实现 | — |
| `PmNode` | `mem/pm-node.hpp` | NUMA 节点分配器 | `alloc_frames` 快路径遍历 ZoneQueues；慢路径 `direct_reclaim` + `direct_compact` 为**桩** | 部分实现 | P0 |
| `PmNode::Current` | `mem/pm-node.hpp` | 当前 CPU 的 NUMA 节点访问器 | `CpuLocal::Current<PmNode*>`——继承线程分配策略后委托本地节点 | 完整实现 | — |
| `PmmAllocator` | `mem/pmm.hpp` | PMM 顶层 API (全局函数集) | `alloc_frames(gaf, order)` → `PmNode::Current::alloc_frames()` | 完整实现 | — |

**P0 问题**：`PmNode::direct_reclaim` 和 `direct_compact` 为空，`PmZone::alloc_frames` 为桩。当 watermark 低的 Zone 无法满足分配时，内核无法触发回收或压缩——`NoFail` 分配将永久自旋。

#### 2.1.3 页面回收

| 类 | 所属文件 | 用途 | 设计原因 | 状态 | 优先级 |
|---|---------|------|---------|------|------|
| `PageQueues` | `mem/page-queues.hpp` | LRU/MRU 页面老化队列 | Pinned → Anonymous → Reclaimable[0..N] 多代旋转；对标 Zircon `PageQueues` 和 Linux LRU | 部分实现 | P1 |
| `PageQueues::MruRunner` | `mem/page-queues.hpp` | MRU 扫描线程 | 线程骨架存在，`run()` 为空 | **空壳** | P1 |
| `PageQueues::LruRunner` | `mem/page-queues.hpp` | LRU 回收线程 | 线程骨架存在，`run()` 为空 | **空壳** | P1 |
| `PageRequest` | `mem/page-request.hpp` | 异步页请求 (换入/换出) | `wait(suspendable)` 仅声明 | **骨架** | P2 |
| `PageSource` | `mem/page-request.hpp` | 页源抽象 (swap, file, user-pager) | `RefCounter` 空壳 | **骨架** | P2 |

#### 2.1.4 虚拟内存 (VMM)

| 类 | 所属文件 | 用途 | 设计原因 | 状态 | 优先级 |
|---|---------|------|---------|------|------|
| `VmAspace` | `mem/vm_aspace.hpp` | 地址空间 (进程地址空间或内核地址空间) | 持有 root VmArea + ArchVmAspace；`page_fault()` 入口；上下文切换 | 部分实现 | P0 |
| `VmArea` | `mem/vm_area.hpp` | 虚拟内存地址区域 (VMAR) | 树状结构 (root → sub-VMA → ... → VmMapping)；`find_spot()` 空闲地址搜索 | 基本实现 | P0 |
| `VmMapping` | `mem/vm_mapping.hpp` | VMO 到地址空间的映射 | `map()` 通过 `VmCowPages::Cursor` 获取页面 → `MappingCoalescer` 批量映射 | 基本实现 | P0 |
| `VmAreaOrMapping` | `mem/vm_area_or_mapping.hpp` | VmArea/VmMapping 共享基类 | `VmaSet` 侵入式 multiset 按基址排序；`activate()`/`destroy()` 生命周期 | 完整实现 | — |
| `MappingProtectionRanges` | `mem/vm_mapping.hpp` | 映射内子区间权限跟踪 | 区间分裂/合并算法；支持同一映射的不同子区间有不同的 MMU 保护 | 完整实现 | — |
| `MappingCoalescer<T>` | `mem/vm_mapping.hpp` | 批量页表映射优化 | 收集连续物理页 → 单次 `arch_aspace.map_bulk()` | 完整实现 | — |
| `VmObject` | `mem/vm_object.hpp` | VMO 抽象基类 | Paged / Physical 两种子类型；commit/decommit/read/write 虚方法 | 完整实现 | — |
| `VmObjectPaged` | `mem/vm_object_paged.hpp` | 可分页 VMO | 内部委托 `VmCowPages`；`commit_range` 已实现 | 部分实现 | P0 |
| `VmObjectPhysical` | `mem/vm_object_physical.hpp` | 物理内存 VMO (MMIO/设备) | 简单的 `PhysAddr + size` 包装 | 完整实现 | — |
| `VmCowPages` | `mem/vm_cow_pages.hpp` | COW 页面管理 | 页面分配器 + `Cursor` 遍历模式；写时复制逻辑的基础 | 部分实现 | P0 |
| `VmCowPages::Cursor` | `mem/vm_cow_pages.hpp` | VMO 页面遍历游标 | `require_owned_page()` 按需分配 VmPage；异步页请求为**桩** | 部分实现 | P0 |
| `VmPageMap` | `mem/vm_page_map.hpp` | VMO 内稀疏页面存储 | 基于 `ktl::XArray` 的稀疏数组，按 vmo_offset 索引 | 最小实现 | P1 |

**P0 问题清单**：

| 问题 | 必需动作 | 
|------|---------|
| `VmMapping::fault()` — 空函数体 | 实现：获取页面 → 建立页表映射 → 处理 COW |
| `VmArea::unmap()` — 返回 Unimplemented | 实现：枚举子区域 → 逐级解除映射 |
| `VmArea::protect()` — 返回 Unimplemented | 实现：更新保护区间 + arch 层 protect |
| `VmAspace::clone()` — 返回 Unimplemented | 实现：DFS 复制 VMA 树 + attach VMO |
| `VmObjectPaged::decommit/read/write` — 全部 Unimplemented | 按需逐步实现 |
| `VmCowPages::Cursor::create_read_request` — 桩 | 实现异步换入 (从 PageSource 获取页) |

#### 2.1.5 内核堆与 Slab

| 类 | 所属文件 | 用途 | 设计原因 | 状态 | 优先级 |
|---|---------|------|---------|------|------|
| `MemCache` | `mem/object-cache.hpp` | Slab 分配器缓存 | 对标 Zircon `ObjectCache` + Linux SLUB：每 CPU 活跃 slab + partial 链表；每 NUMA 节点 partial 链表 | 部分实现 | P0 |
| `Slab` | `mem/slab.hpp` | 单 Slab (一组同大小对象) | 继承 PageFrame (role=Slab)；侵入式单链表管理空闲对象 | 部分实现 | P0 |
| `ObjectCache<T>` | `mem/object-cache.hpp` | 类型安全 Slab 包装 | `static get(gaf, args...) → T*` 和 `put(T*)` | 已实现 | — |
| `BasicObjectCacheBuilder` | `mem/object-cache.hpp` | 编译期缓存注册器 | `AddInitHook` 模板在链接时注册 `GKTL_INIT_HOOK` | 已实现 | — |
| `CpuLocal` | `mem/cpu-local.hpp` | Per-CPU 存储子系统 | `Mapper` 管理静态/动态 per-CPU 区域；`Current<T>` 访问器；`PerCpu<T>` RAII 数组 | 大多数实现 | P1 |
| `CpuLocal::Mapper` | `mem/cpu-local.hpp` | Per-CPU 内存映射引擎 | `init()` + `create_first_chunk()` + `compactify_sparse_topology_ids()` | 完整实现 | — |
| `CpuLocal::Dyn` | `mem/cpu-local.hpp` | 运行时动态 per-CPU 分配 | `allocate<T>()` / `deallocate<T>()` 声明 | **桩代码** | P1 |
| `Stack` | `mem/stack.hpp` | 内核栈分配器 | `init()` 创建 VmObjectPaged + VmMapping，16KB 默认 | 完整实现 | — |
| `EarlyMem` (Layer 2 版本) | `mem/early-mem.hpp` | 封装 phys::EarlyMem，向 PMM 过渡 | `do_handoff()` 将未使用的 bootmem 区域释放到 buddy 系统 | 完整实现 | — |

**P0 问题**：`Slab::get_object()` 和 `put_object()` 为空——内核对象分配 (new/delete) 实际不可用。

#### 2.1.6 新增建议类

| 类 | 用途 | 设计原因 | 优先级 |
|---|------|---------|------|
| `VmAspacePool` | 地址空间对象池 | 避免频繁 `MemCache::allocate` 大型 VmAspace | P2 |
| `PmCompact` | 内存压缩 (compaction) | 迁移可移动页面以创建连续高阶页面——`direct_compact` 的实现 | P1 |
| `PmReclaim` | 页面回收 (reclaim) | 替换 `PmNode::direct_reclaim` 桩代码；遍历 LRU → 换出/丢弃 | P1 |
| `VmCloneContext` | COW 地址空间克隆上下文 | 封装 `VmAspace::clone()` 的 DFS 遍历状态，避免递归栈溢出 | P1 |
| `PageSwapBackend` | 交换后端 (swap file/device) | `PageSource` 的具体实现，写入/读取 swap 存储 | P2 |

### 2.2 同步原语 (sync)

| 类 | 所属文件 | 用途 | 设计原因 | 状态 | 优先级 |
|---|---------|------|---------|------|------|
| `spinlock::SpinLock<Options>` | `sync/spin-lock.hpp` | 底层自旋锁 (CAS 等待) | 锁值编码持有者 Thread*；三个预定义别名 (默认/监控/无追踪)；IRQ 保存和抢占禁用状态保存器 | 完整实现 | — |
| `WaitQueue` | `sync/wait-queue.hpp` | 阻塞/唤醒基础原语 | 侵入式 multiset 按 deadline 排序；park_thread 含超时；`lock_for_notify_*` 为**桩** | 部分实现 | P0 |
| `OwnedWaitQueue` | `sync/owned-wait-queue.hpp` | 带 PI 的等待队列 | 继承 WaitQueue + 所有者 + PI 图传播；`begin_propagate`/`complete_propagate` 骨架存在但**不完整** | 部分实现 | P0 |
| `Mutex` | `sync/mutex.hpp` | 互斥锁 (基于 OwnedWaitQueue) | 快路径 CAS + 自旋 + 慢路径 OWQ 阻塞；`HandOff` 协议避免惊群 | 部分实现 | P0 |
| `CirticalMutex` | `sync/mutex.hpp` | 临界区标记互斥锁 | protected 继承 Mutex，TA 标注 `"Cirtical-Mutex"` | 已实现 | — |
| `Event` | `sync/event.hpp` | 事件通知 (基于 WaitQueue) | AutoUnsignal 标志控制单/多唤醒；wait 条件可能**反转** | 部分实现 | P1 |
| `Semaphore` | `sync/semaphore.hpp` | 计数信号量 | `post()` 空 / `wait()` 返回 Unimplemented | **桩代码** | P1 |
| `priv::RwLock<EnablePi>` | `sync/rwlock.hpp` | 读写锁 (PI 可选) | 读写计数器 + WaitingQueue；全部方法声明但**无 .cpp 实现** | **桩代码** | P1 |
| `LocalLock` | `sync/local-lock.hpp` | Per-CPU 局部锁 | 仅声明 acquire/release，**无实现** | **桩代码** | P2 |

**P0 问题详解**：

| 问题 | 后果 | 修复路径 |
|------|------|---------|
| `WaitQueue::lock_for_notify_one/all` 返回 `none` | Event::signal 无法唤醒等待者 | 实现 lock → pick waiter(s) → unpark 序列 |
| `OwnedWaitQueue::lock_pi_chain_common` 不完整 | PI 链并发操作无锁保护 | 完成 PI 链遍历的锁获取逻辑 |
| `Mutex::acquire_slowpath` 自旋后不阻塞 | 高竞争下活锁 | 自旋超时后调用 `m_waiters.park_and_assign_owner` |
| `OwnedWaitQueue::park_and_assign_owner` 为空 | Mutex 慢路径无法将线程放入等待队列 | 实现：设置 owner + park 当前线程 |
| `Slab::get_object/put_object` 为空 | 无法分配内核对象 | 实现 freelist 弹出/压入 |

---

## Layer 3: 内核核心服务 — `kernel/main/task/` + `time/` + `irq/`

### 3.1 调度与线程 (task)

| 类 | 所属文件 | 用途 | 设计原因 | 状态 | 优先级 |
|---|---------|------|---------|------|------|
| `Thread` | `task/thread.hpp` | 内核线程抽象 | 7 状态机 + ArchThread + SchedContext + PreemptionState + WaiterState；静态全局线程列表 | 完整实现 | — |
| `Thread::Current` | `task/thread.hpp` | 当前 CPU 线程访问器 | `sleep/yield/preempt/reschedule/exit/become_idle` 等便捷方法 | 完整实现 | — |
| `TaskState` | `task/thread.hpp` | 线程任务状态 (入口/返回码/join) | 内部 WaitQueue 支持 `join()` 阻塞等待线程退出 | 完整实现 | — |
| `IdleThread` | `task/idle-thread.hpp` | 每 CPU 空闲线程 | LazyInit<Thread> + 状态机 (Idle/Running) + ArchIdleThread | 完整实现 | — |
| `Scheduler` | `task/sched/scheduler.hpp` | 每 CPU 调度器实例 | 类 Linux `struct rq`：Runtime + span<Schedule*> + migrate/steal | 完整实现 | — |
| `Scheduler::Current` | `task/sched/scheduler.hpp` | 当前 CPU 调度器访问器 | `preempt/reschedule/idle/yield/park_thread/tick` | 完整实现 | — |
| `Schedule` | `task/sched/schedule.hpp` | 调度类抽象接口 | enqueue/dequeue/evaluate_next/on_tick/select_target_cpu 等纯虚方法 | 完整实现 | — |
| `Fair` | `task/sched/fair.cpp` | EEVDF 公平调度类 | RunQueue (侵入式 MultiSet) + vlag 调整 + eligibility 检查 + delay dequeue | 完整实现 | — |
| `Runtime` | `task/sched/runtime.hpp` | 每 CPU 运行队列元数据 | curr/prev/next_thread + total_weight + Driver + migrating 列表 | 完整实现 | — |
| `Driver` | `task/sched/driver.hpp` | 调度时钟 + 抢占定时器 | `update_clock()` 虚拟调度时钟；`reset_preempt()` 编程硬件定时器 | 完整实现 | — |
| `Context` | `task/sched/context.hpp` | 每线程调度实体 | Profile (base/effective/inherited) + CpuAffinity + RunQueueState + SchedParams + Timeslice | 完整实现 | — |
| `BaseProfile` | `task/sched/profiles.hpp` | 用户指定调度参数 | weight + discipline (Normal/Deadline) | 完整实现 | — |
| `EffectiveProfile` | `task/sched/profiles.hpp` | 有效调度参数 (考虑 PI) | max(base, inherited) | 完整实现 | — |
| `InheritedProfile` | `task/sched/profiles.hpp` | 从 PI 链继承的优先级 | inherit_weight | 完整实现 | — |
| `CpuAffinity` | `task/sched/context.hpp` | CPU 亲和性 (hard/soft 掩码) | `get_available_mask()` = hard ∩ soft | 完整实现 | — |
| `PreemptionState` | `task/preemption-state.hpp` | 抢占状态机 (32-bit 位域) | 抢占禁用计数 + 急切重调度禁用计数 + IRQ 嵌套深度 + NMI 嵌套深度 + pending 标志 | 完整实现 | — |
| `AutoPreemptDisabler` | `task/auto-preempt-disabler.hpp` | RAII 抢占禁用 | 析构时 `enable_preemption()` | 完整实现 | — |
| `AutoEagerReschedDisabler` | `task/auto-preempt-disabler.hpp` | RAII 急切重调度禁用 | 同上模式 | 完整实现 | — |
| `AutoMigrateDisabler` | `task/auto-migrate-disabler.hpp` | RAII 迁移禁用 | **桩代码** (构造/析构为空) | **桩代码** | P1 |
| `DpcRunner` | `task/dpc.hpp` | 延迟过程调用 | `Queue::do_work()` 事件循环 + `enqueue()` | 完整实现 | — |
| `IpiService` | `task/ipi-service.hpp` | IPI 服务 (发送重调度/挂起 IPI) | `send_resched(cpu/mask)` 检查活跃掩码 → `arch_mp_reschedule()` | 完整实现 | — |

**新增建议类**：

| 类 | 用途 | 设计原因 | 优先级 |
|---|------|---------|------|
| `Scheduler::LoadBalancer` | 负载均衡 | 替换 `steal_work_from()` 桩代码；`find_busiest_queue()` + `detach_tasks()` + `attach_tasks()`；配置调度域以支持 NUMA | P0 |
| `Scheduler::Migrator` (完善) | 线程迁移 | 已有基础 `move_queued_thread`；需实现完整的 deactivate → activate 序列 + 亲和性重新检查 | P1 |
| `Deadline` (调度类) | SCHED_DEADLINE 实现 | 已定义 `Discipline::Deadline` 但无对应 `Schedule` 子类；实现 CBS(Constant Bandwidth Server) 算法 | P2 |
| `ThreadGroup` | 线程组 (POSIX 等效) | 信号分发、资源限制的容器；关联到 `ProcessDispatcher` | P2 |

**P0 问题**：
- `steal_work_from()` 为空桩——多核无负载均衡，空闲 CPU 无法获取工作
- `migrate_thread()` 仅有状态检查不执行迁移——线程亲和性设置无法真正调度

### 3.2 时钟与定时器 (time)

| 类 | 所属文件 | 用途 | 设计原因 | 状态 | 优先级 |
|---|---------|------|---------|------|------|
| `ClockSource` | `time/clock-source.hpp` | 硬件时钟源抽象 (只读计数器) | Vtable 模式 (非虚函数)；优先级排序的全局注册列表；`select_best_source()` | 完整实现 | — |
| `ClockSource::Default` | `time/clock-source.hpp` | 软件 jiffies 回退源 | SpinLock 保护的 `s_ticks`；在无硬件时钟时使用 | 完整实现 | — |
| `ClockEventDevice` | `time/clock-event-device.hpp` | 可编程事件设备 (到期中断) | Vtable 模式；支持 Periodic/Oneshot/TimePoint 模式；全局注册列表 | 完整实现 | — |
| `Clock` | `time/clock.hpp` | 基于 ClockSource 的累积时间 | Seqlock (`AtomicU8 m_seq`) 无锁读；`update_time()` 读周期计数器 → 计算 delta | 完整实现 | — |
| `SystemClock` | `time/clock.hpp` | 系统时钟 (可 NTP 调整) | `notify(ClockSource*)` 支持热切换源 | 完整实现 | — |
| `SteadyClock` | `time/clock.hpp` | 单调时钟 (不受 NTP 影响) | 独立的 Clock 实例 | 完整实现 | — |
| `TickDevice` | `time/tick-device.hpp` | 每 CPU Tick 设备 | 封装 ClockEventDevice；`rebind_device()` 热切换；`handle_periodic_event()` jiffy 前进 | 完整实现 | — |
| `TickSched` | `time/tick-sched.hpp` | 软件 Tick 模拟 (HP 模式) | 使用内核 Timer 模拟周期性 tick；`handle_event()` 调用 `Scheduler::timer_tick` | 部分实现 | P1 |
| `Timer` | `time/timer.hpp` | 内核定时器 (侵入式 set 节点) | Deadline + slack 容忍窗口；惰性取消 (`m_cancelled` 原子)；`forward()` 周期前移 | 部分实现 | P0 |
| `TimerQueue` | `time/timer-queue.hpp` | 每时钟类型的定时器队列 | `insert_timer` 已实现；`remove_timer` 为**桩**；`run_expired_timers()` 含 Restart 支持；**字段命名与头文件不匹配** | 部分实现 | P0 |
| `TimerScheduler` | `time/timer-scheduler.hpp` | 每 CPU 定时器调度器 | 持有 `perclock_[MaxNumClocks]` 数组；`activate_timer()` 为**空**——定时器从不入队 | **桩代码** | P0 |
| `TimerScheduler::Current` | `time/timer-scheduler.hpp` | 当前 CPU 定时器调度器访问器 | `run_local_queues()` | 已实现 | — |
| `Deadline` | `time/timer.hpp` | 定时器到期时间 + 容忍窗口 | `when()` / `earliest()` / `latest()` / `infinite()` | 完整实现 | — |

**P0 问题详解**：

| 问题 | 后果 | 修复路径 |
|------|------|---------|
| `TimerScheduler::activate_timer()` 为空 | 定时器从不被插入 TimerQueue | 实现 `m_queues[clockid].insert_timer(timer)` |
| `TimerQueue::remove_timer()` 为空 | 取消的定时器残留 | 实现从 intrusive set 中移除 |
| `TimerQueue` 命名不匹配 | 编译失败 (`.cpp` 用 `canary_` 而 `.hpp` 用 `m_canary`) | 统一为 `m_` 前缀 |
| `calculate_mult_shift()` 返回类型错误 | 编译失败 (声明 void 但返回 pair) | 修复签名 |
| `TickSched` Legacy 模式 else 分支为空 | 无 HPET 时 tick 模拟不工作 | 实现 legacy 路径 |
| `TickDevice::rebind_device` 用错 cpumask | 亲和性设置到旧设备而非新设备 | 修复为 `new_clkdev->cpumask()` |

### 3.3 中断处理 (irq)

| 类 | 所属文件 | 用途 | 设计原因 | 状态 | 优先级 |
|---|---------|------|---------|------|------|
| `IrqDispatcher` | `irq/irq_dispatcher.cpp` | IRQ 中央调度器 (全局单例) | `init(num)` 预分配 IrqObject 数组；`request_irq` 创建 IrqObserver；`handle_irq` 分发 | 基本实现 | P0 |
| `IrqObject` | `irq/irq_object.cpp` | 单条 IRQ 线的对象 | `attach`/`detach` 观察者 (共享约束)；`notify()` 调用所有观察者 | 基本实现 | P0 |
| `IrqObserver` | 头文件缺失 | 中断观察者 (单次注册) | 存储在 IrqObject 的侵入式链表中；on_event() 调用注册的处理函数 | 基本实现 | P0 |

**P0 问题**：所有 IRQ 头文件 (`ours/irq/*.hpp`) **不存在于仓库中**。`.cpp` 文件的 `#include` 路径全部无效——IRQ 子系统**当前无法编译**。

**必需的新增文件**：

| 文件 | 内容 | 优先级 |
|------|------|------|
| `kernel/main/include/ours/irq/init.hpp` | `irq::init()` / `irq::init_early()` 声明 | P0 |
| `kernel/main/include/ours/irq/irq_dispatcher.hpp` | `IrqDispatcher` 类声明 | P0 |
| `kernel/main/include/ours/irq/irq_object.hpp` | `IrqObject` + `IrqObserver` + `IrqChip` 接口声明 | P0 |
| `kernel/main/include/ours/irq/mod.hpp` | `request_irq` / `release_irq` / `set_affinity` / `handle_irq_generic` 声明 | P0 |

**新增建议类**：

| 类 | 用途 | 设计原因 | 优先级 |
|---|------|---------|------|
| `IrqChipAdapter` | 中断控制器抽象 (IO-APIC, MSI, GIC) | 统一 `mask/unmask/startup/eoi/set_affinity` 接口；当前 IO-APIC 的 `IrqChip` 函数指针集合需升级为正式的虚接口 | P1 |
| `IrqStatistics` | 中断统计 | `/proc/interrupts` 等效：每 IRQ 线计数 + 每 CPU 计数 | P2 |
| `MsiAllocator` | MSI/MSI-X 中断分配器 | PCI 设备的多个向量管理；当前 IoApicChip 无 MSI 支持 | P2 |

---

## Layer 4: 内核对象系统 — `kernel/object/`

该层将内核资源封装为用户态可见的对象，通过 Handle 机制管理权限和生命周期。

| 类 | 所属文件 | 用途 | 设计原因 | 状态 | 优先级 |
|---|---------|------|---------|------|------|
| `Dispatcher` | `object/dispatcher.hpp` | 内核对象抽象基类 | RefCounter + KoId + m_handle_count 双层引用；Signal + SignalObserver 系统 | 完整实现 | — |
| `SoloDispatcher<T>` | `object/dispatcher.hpp` | CRTP 单端对象辅助类 | 为 Process/Thread/Job/Resource/VmArea/Pager 提供默认 Right + canary 验证 | 完整实现 | — |
| `PeeredDispatcher<T>` | `object/dispatcher.hpp` | CRTP 对等对象辅助类 (Channel) | `on_zero_handles()` 标记 final —— 一端关闭时通知另一端 | 完整实现 | — |
| `Handle` | `object/handle.hpp` | 用户态句柄（Rc + Rights + KoId + baseval） | `make()` 创建，`clone()` 降权克隆 | 完整实现 | — |
| `KernelHandle<T>` | `object/handle.hpp` | 内核态类型化句柄 | Movable 独占所有权；`reset()` 调用旧对象的 `on_zero_handles()` | 完整实现 | — |
| `HandleTable` | `object/handle-table.hpp` | 每进程句柄表 | `Deque<Handle>` (每块 8) 存储；**所有 CRUD 方法均未声明** | **骨架** | P0 |
| `SignalObserver` | `object/observer.hpp` | 信号观察者（侵入式链表节点） | `on_match()` / `on_cancel()` 纯虚方法；`match_key()` 支持端口分发 | 完整实现 | — |
| `ProcessDispatcher` | `object/process-dispatcher.hpp` | 进程内核对象 | `create()` 未实现；`attach_thread` 已实现；`detach_thread` 未实现 | **骨架** | P0 |
| `ThreadDispatcher` | `object/thread-dispatcher.hpp` | 线程内核对象 | `spawn/start/activate/trampoline` 已实现；`resume/suspend` 桩 | 基本实现 | P0 |
| `ChannelDispatcher` | `object/channel-dispatcher.hpp` | IPC 通道（对等） | `create()` 声明但**未实现** | **桩代码** | P1 |
| `JobDispatcher` | `object/job-dispatcher.hpp` | 作业对象（进程容器） | `root_job()` 声明但**未实现** | **桩代码** | P1 |
| `ResourceDispatcher` | `object/resource-dispatcher.hpp` | 硬件资源对象 | `create()` 验证参数但**未构造 Handle** | **部分** | P1 |
| `VmAreaDispatcher` | `object/vm-area-dispatcher.hpp` | VMAR 内核对象 | 完全**空壳** | **空壳** | P1 |
| `PagerDispatcher` | `object/pager-dispatcher.hpp` | 用户态分页器 | `operate_range()` 声明但**未实现** | **桩代码** | P2 |

**P0 问题详解**：

| 问题 | 后果 | 修复路径 |
|------|------|---------|
| `HandleTable` 无 add/get/remove | 进程无法管理句柄——无法实现 `sys_close` | 添加 `add_handle(KernelHandle)` / `remove_handle(u32)` / `get_handle(u32)` |
| `ProcessDispatcher::create()` 未实现 | 无法创建进程 | 实现：创建 VmAspace → 创建 HandleTable → 分配 KoId |
| `ProcessDispatcher::detach_thread()` 未实现 | 线程退出时进程引用残留 | 实现：从 m_thread_list 移除 → 清理 |
| `ThreadDispatcher::resume/suspend` 为空 | 无法挂起/恢复线程 | 实现：调用 `task::Thread::suspend()/resume()` |

**新增建议类**：

| 类 | 用途 | 设计原因 | 优先级 |
|---|------|---------|------|
| `PortDispatcher` | 多对象等待端口 | Zircon 的 `zx_port_wait` 等效；epoll 风格的事件聚合 | P1 |
| `EventDispatcher` | 事件内核对象 (用户态可信号化) | 区别于内核内部 `sync::Event`——用户态可 wait/signal 的可等待对象 | P1 |
| `FutexDispatcher` | Futex 内核对象 | 用户态互斥的高效实现 | P2 |
| `StreamDispatcher` | 数据流对象 | 单向字节流，用于 socket 等效物 | P2 |

---

## Layer 5: 系统调用接口 — `kernel/syscall/` (新目录)

该层当前**不存在**——`ThreadDispatcher::trampoline()` 直接硬编码了用户态入口。需要设计正式的系统调用分发层。

### 5.1 新增类设计

| 类 | 用途 | 设计原因 | 优先级 |
|---|------|---------|------|
| `SyscallDispatcher` | 系统调用表 + 分发 | 将 syscall number → handler 映射；参数从 `arch::IrqFrame` 提取；返回值写入 `rax` | P0 |
| `SyscallContext` | 单次系统调用上下文 | 封装参数解码/验证/返回；权限检查 | P1 |
| `SyscallTable` | 系统调用注册表 | 编译期自动生成 (类似 Linux `SYSCALL_DEFINEn` 宏) | P1 |
| `UserPointer<T>` | 用户态指针包装 | 自动 `copy_from_user`/`copy_to_user` + SMAP 切换 | P1 |

---

## 实施路线图

### Phase 0: 恢复到可编译 (P0 — 阻塞性)

| 顺序 | 任务 | 涉及模块 |
|------|------|---------|
| 0.1 | 创建缺失的 IRQ 头文件 | irq |
| 0.2 | 修复 TimerQueue 字段命名不匹配 | time |
| 0.3 | 修复 `calculate_mult_shift()` 返回类型 | time |
| 0.4 | 创建 SyscallDispatcher 基础框架 | syscall (新) |

### Phase 1: 补全核心功能 (P0)

| 顺序 | 任务 | 涉及模块 |
|------|------|---------|
| 1.1 | 实现 `PmNode::direct_reclaim` + `direct_compact` | mem |
| 1.2 | 实现 `Slab::get_object` / `put_object` | mem |
| 1.3 | 实现 `VmMapping::fault()` + `VmArea::unmap/protect` | mem |
| 1.4 | 补全 `OwnedWaitQueue` PI 链和阻塞路径 | sync |
| 1.5 | 补全 `Mutex::acquire_slowpath` OWQ 阻塞 | sync |
| 1.6 | 实现 `WaitQueue::lock_for_notify_one/all` | sync |
| 1.7 | 实现 `TimerScheduler::activate_timer` + `TimerQueue::remove_timer` | time |
| 1.8 | 实现 `HandleTable` CRUD 方法 | object |
| 1.9 | 实现 `ProcessDispatcher::create()` | object |
| 1.10 | 实现负载均衡 `steal_work_from()` | task |

### Phase 2: 功能完善 (P1)

| 顺序 | 任务 | 涉及模块 |
|------|------|---------|
| 2.1 | 实现 `VmAspace::clone()` + COW 克隆 | mem |
| 2.2 | 实现 PageQueues LRU/MRU runner 线程 | mem |
| 2.3 | 实现 `RwLock` | sync |
| 2.4 | 实现 `Semaphore` | sync |
| 2.5 | 实现 `LoopDetector` (lockdep 核心) | lockdep |
| 2.6 | 实现 `ChannelDispatcher::create()` | object |
| 2.7 | 实现 `JobDispatcher::root_job()` | object |
| 2.8 | 实现 `ResourceDispatcher::create()` 完整逻辑 | object |
| 2.9 | 填充 `ArchHandOff` 交接数据 | arch |
| 2.10 | 实现 `ArchIdleThread::idle()` (HLT) | arch |

### Phase 3: 高级特性 (P2)

| 顺序 | 任务 | 涉及模块 |
|------|------|---------|
| 3.1 | 实现 Deadline 调度类 (SCHED_DEADLINE) | task |
| 3.2 | 实现 `PagerDispatcher` + 异步页入 | object + mem |
| 3.3 | 实现 `PortDispatcher` + 多对象等待 | object |
| 3.4 | 实现 MSI/MSI-X 中断分配 | irq + arch |
| 3.5 | EPT/VMX 支持 (嵌套虚拟化) | arch |
| 3.6 | RISC-V 架构移植 | arch/riscv/ |
| 3.7 | 实现 Per-CPU frame cache (PCP) | mem |
| 3.8 | `ScopedGuard` 多锁有序获取 | lockdep |

---

## 设计原则总结

1. **编译时协议优先**：C++20 Concept + `static_assert` + `#include` guard 三层静态验证，确保架构移植时不遗漏接口
2. **零开销抽象**：Arch 类型直接嵌入通用类（组合），无虚函数调用，无运行时多态
3. **侵入式容器**：所有内核数据结构使用侵入式链表/set (ustl)，避免独立的内存分配
4. **双层引用模型**：Handle Count (用户态) + RefCounter (内核态) 分离，允许内核在用户关闭所有句柄后保持内部引用
5. **三层 Profile**：base (用户指定) / effective (实际) / inherited (PI) 分离，PI 传播时只需修改 inherited 部分
6. **惰性 + 批量化**：Timer 惰性取消，MappingCoalescer 批量映射，FramePcpuCache 批量分配
