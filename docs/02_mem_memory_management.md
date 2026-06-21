# 02 — 内存管理子系统 (Memory Management)

## 1. 概述

内存管理子系统是内核最核心的部分，分为三大层次：

| 层次 | 职责 | 核心类 |
|------|------|--------|
| **物理内存模型** | PFN ↔ struct page 转换，稀疏内存支持 | `MemoryModel`, `MemSection` |
| **PMM (物理内存管理)** | 伙伴分配器，NUMA感知，Frame分配/回收 | `PmNode`, `PmZone`, `FrameSet` |
| **VMM (虚拟内存管理)** | 地址空间，VMO层次，页表映射，缺页处理 | `VmAspace`, `VmArea`, `VmMapping`, `VmObject` |

此设计与 Zircon 内核的内存管理几乎一一对应，同时引入了 Linux 的 NUMA 感知分配和伙伴系统设计。

## 2. 物理内存模型 (Memory Model)

### 2.1 稀疏内存模型 (Section-based Sparse Memory)

项目实现了类似 Linux SPARSEMEM 的 Section 基内存模型：

```
Physical Address Space
│
├── Root Section (Level 2)
│   ├── Section[0] (Interminal → Level 1)
│   │   ├── Section[0] (Interminal → Level 0)
│   │   │   ├── Section[0] (Terminal → PmFrame[])
│   │   │   │   ├── PmFrame[0]  ← pfn_to_frame(pfn)
│   │   │   │   ├── PmFrame[1]
│   │   │   │   └── ...
│   │   │   └── Section[1] (Terminal)
│   │   └── Section[1] (Interminal → Level 0)
│   └── Section[1] (None — no memory here)
```

核心类 `MemSection` 是一个紧凑的节点：
- 2 bits 状态 (`None`, `Present`, `Online`)
- 2 bits 类型 (`Unknown`, `Terminal`, `Interminal`)
- 2 bits 级别 (0-2)
- 58-bit 指针 (指向子 Section 或 PmFrame 数组)

最多支持 3 级 Section，覆盖完整的 64 位物理地址空间。

### 2.2 与 Linux SPARSEMEM 的对比

| 概念 | Linux | 本项目 |
|------|-------|--------|
| Section 大小 | `SECTION_SIZE_BITS` (默认 27, 128MB) | `SECTION_SIZE_BITS` (默认 27) |
| 最大 Section 级数 | 4 (MAX_PHYSMEM_BITS) | 3 (kMaxLevel) |
| PFN→Section 转换 | `pfn_to_section_nr` + `__nr_to_section` | `pfn_to_frame()` → 逐级下降 |
| 每 Section 帧数 | `PAGES_PER_SECTION` | `kFramesPerSection` |
| MemMap 初始化 | `sparse_init` + `sparse_buffer_init` | `MemoryModel::init_memmap()` |

### 2.3 PhysMap (直接映射窗口)

内核将所有物理内存通过 `KERNEL_ASPACE_BASE` 起始的恒等映射窗口访问：

```cpp
struct PhysMap {
    PhysAddr kPhysBase;   // 物理地址基址
    VirtAddr kVirtBase;   // 对应的虚拟地址基址
    usize    kSize;        // 映射窗口大小

    static PhysAddr phys_to_virt(PhysAddr pa);  // pa → va (直接偏移)
    static PhysAddr virt_to_phys(VirtAddr va);  // va → pa (直接偏移)
};
```

这等同于 Linux 的 `PAGE_OFFSET` / `phys_to_virt` 和 Zircon 的 `PHYS_MAP`。

## 3. 物理内存管理 (PMM)

### 3.1 整体架构

PMM 是从 Zircon `PmmNode` 设计中演变而来的三层结构：

```
alloc_frames(gaf, order)                    ← 顶层 API (pmm.hpp)
    │
    ▼
PmNode::Current::alloc_frames()             ← NUMA 节点选择 + 策略
    │
    ▼
PmNode::get_frame_from_zone_queues()        ← 遍历 Zone 队列
    │
    ▼
PmZone::alloc_frame()                       ← 单 Zone 内分配
    │
    ▼
FrameSet (Buddy Allocator)                  ← 伙伴系统核心
```

### 3.2 伙伴分配器 (FrameSet / Buddy System)

`FrameSet` 是一个标准的二进制伙伴分配器，支持 order 0 到 `MAX_FRAME_ORDER` (默认 10，即 4MB 最大连续分配)：

```
order 3:  [========================================]  (32 页)
order 2:  [==================] [==================]
order 1:  [========] [========] [========] [========]
order 0:  [====][====][====][====][====][====][====][====]
```

**分配算法** (`acquire_frame_locked`)：
1. 从目标 order 向上下搜索第一个空闲块
2. 向下分裂：递归地将多余部分插入低 order 空闲链表
3. 标记页面角色为 `Pmm`，清零（可选）

**释放算法** (`release_frame_locked`)：
1. 检查 buddy 是否空闲且同 zone/node/order
2. 如果 buddy 空闲，从空闲链表中移除，合并升级到更高 order
3. 递归合并直到 buddy 不空闲

### 3.3 NUMA 感知分配

项目的 NUMA 设计同时吸收了 Linux 和 Zircon 的理念：

**PmNode (NUMA 节点)**：
```
PmNode[0]               PmNode[1]
├── PmZone(Dma)         ├── PmZone(Dma)
├── PmZone(Dma32)       ├── PmZone(Dma32)
├── PmZone(Normal)      ├── PmZone(Normal)
│   └── FrameSet        │   └── FrameSet
└── ZoneQueues          └── ZoneQueues
    ├── LocalSequential      ├── LocalSequential
    ├── LocalContiguous      ├── LocalContiguous
    ├── SharedAffinity       ├── SharedAffinity (包含 Node0 的 Zone)
    └── SharedSequential     └── SharedSequential
```

**Node 间距离矩阵**：`PmNode::build_optimal_node_route()` 使用 Dijkstra 最短路径算法计算 NUMA 节点间最优分配路由。

**Zone 类型层次**（借鉴 Linux）：
```
ZoneType::Dma        ← ISA DMA 限制 (< 16MB)
ZoneType::Dma32      ← 32-bit DMA 限制 (< 4GB)
ZoneType::Normal     ← 常规内存
ZoneType::Movable    ← 可移动页面
```

**分配策略** (`alloc_frames`)：
1. 快路径：从本地 Node 的 Zone 队列查找满足 watermark 的 Zone
2. 慢路径（桩代码）：`direct_reclaim()` + `direct_compact()`
3. `NoFail` 标志：自旋等待直到分配成功

### 3.4 GAF 分配标志

```cpp
enum Gaf {
    Dma, Dma32, Movable,      // Zone 修饰符
    NoFail, NoRetry, Write,   // 策略标志
    Compound, ThisNode, Zero,  // 其他
};
```

预定义组合：`kGafBoot`（启动阶段）, `kGafKernel`（内核分配），`kGafUser`（用户空间分配）。

### 3.5 页面框架 (PageFrame 层次)

```
PageFrame (基类，BitFields 压缩存储)
├── PmFrame (Role::Pmm — 伙伴分配器管理)
├── VmPage  (Role::Vmm — VMO 逆向映射)
├── PmFolio (Role::Folio — 复合页)
└── Slab    (Role::Slab — Slab 分配器)
```

`PageFrame` 使用 `BitFields<>` 将角色、标志、order、zone、section、node 压缩到单个字中，实现了紧凑的 page 结构体（类似 Linux 的 `struct page` 使用 union 压缩）。

### 3.6 页面队列 (PageQueues)

与 Zircon 的 `PageQueues` 和 Linux 的 LRU 链表类似：

```
Pinned Queue       → 固定页面 (不可换出)
Anonymous Queue    → 匿名页面 (可换出)
MRU Generation 0   → 可回收页面，最近使用
MRU Generation N   → 可回收页面，最久未使用
```

- `rotate_reclaimable_queues()` 推进 MRU 代际
- LRU/MRU Runner 线程用于异步页面回收（线程定义存在但 `run()` 为空）

## 4. 虚拟内存管理 (VMM)

### 4.1 地址空间层次

VMM 的地址空间模型直接来自 Zircon：

```
VmAspace (地址空间)
└── VmArea (root)              ← 根区域，覆盖整个用户地址空间
    ├── VmArea (sub-vma)       ← 子区域 (VMAR)
    │   ├── VmMapping          ← 映射 (关联 VMO)
    │   └── VmArea (sub-vma)   ← 嵌套子区域
    └── VmMapping              ← 映射
```

**关键设计点**：
- `VmArea` 和 `VmMapping` 共享基类 `VmAreaOrMapping`
- 地址空间内所有区域通过 `VmaSet`（基于虚拟地址排序的侵入式 multiset）维护
- `find_spot()` 在两个现有区域之间搜索空闲地址区间（实现 ASLR 的基础）

### 4.2 VMO (VmObject) 层次

```
VmObject (抽象基类，引用计数)
├── VmObjectPaged (可分页 VMO)
│   └── 内部使用 VmCowPages (写时复制页面管理)
└── VmObjectPhysical (物理内存 VMO)
```

**VmObjectPaged** — 最核心的 VMO 类型，支持：
- `commit_range()` — 预提交页面（带 Pin/Write 选项）
- `create_cursor()` — 创建页面遍历游标，供 `VmMapping::map()` 使用

**VmObjectPhysical** — 直接映射物理内存，用于 MMIO 和设备内存。

**VmCowPages** — 管理 VMO 内部的 COW 页面：
- `Cursor` 模式迭代页面范围
- 按需分配 `VmPage`
- 页面请求创建 (async page-in) — 桩代码

### 4.3 映射 (VmMapping)

`VmMapping` 将 VMO 的一段区间映射到地址空间的一段虚拟地址：

```
VmMapping::map()
    ├── map_paged()     ← 通过 VmCowPages::Cursor 获取页面
    │   └── MappingCoalescer  ← 批量映射优化 (最多 N 页)
    └── map_physical()  ← 直接映射物理页面
        └── MappingCoalescer
```

**MappingProtectionRanges** — 一个精妙的数据结构，用于跟踪同一映射内不同子区间的 MMU 保护位（例如：映射的前半部分可写，后半部分只读）。使用侵入式 interval set 实现，支持：
- `update()` — 修改某个子区间的权限，自动分裂/合并相邻区域
- `make_enumerator()` — 遍历所有子区域，供 `protect()` / `unmap()` 使用

### 4.4 缺页处理 (Page Fault)

```
arch 捕获缺页异常
  → handle_vm_fault(va, cause)
    → VmAspace::page_fault(va, cause)
      → page_fault_internal()
        → 在 VMA 树中查找包含该地址的 VmMapping
        → VmMapping::fault(vmf)              ← 桩代码（返回空）
          → VmCowPages::Cursor 获取页面
          → arch_aspace().map() 建立页表映射
```

**VmFault 结构**：
```cpp
struct VmFault {
    VirtAddr va;
    usize    num_pages;
    VmfCause cause;           // Write, Absent, User, Software
    PageRequest page_request;  // 异步 I/O 请求（桩代码）
};
```

### 4.5 地址空间布局

```
Kernel Address Space:
├── KERNEL_LOAD_BASE + kaslr_offset    ← 内核代码/数据
├── KERNEL_ASPACE_BASE                 ← physmap 窗口 (全部 RAM 恒等映射)
└── ...                                ← 内核栈、Slab、VMA 元数据等

User Address Space:
├── USER_ASPACE_BASE                         ← 用户空间起始
├── USER_ASPACE_BASE + RESTRICTED_SIZE       ← 私有地址空间边界
├── [共享地址空间区域]
└── USER_ASPACE_BASE + USER_ASPACE_SIZE      ← 用户空间结束
```

## 5. CPU-local 存储

`CpuLocal` 提供了一个类似 Linux `percpu` 和 Zircon `PerCpu` 的 per-CPU 变量系统：

```cpp
CpuLocal::Current<Thread, Options...>     // 获取当前 CPU 的 Thread 单例
CpuLocal::PerCpu<T>                       // per-CPU 数组 RAII 包装
CpuLocal::for_each<T>([](T& val) { ... })  // 遍历所有在线 CPU
```

底层 `Mapper` 负责：
- 为 per-CPU 变量分配物理页面（每个 CPU 组一份）
- 通过 `g_cpu_offsets[]` 数组索引每个 CPU 的偏移
- `compactify_sparse_topology_ids()` 压缩稀疏的 NUMA/CPU ID 空间

## 6. Slab 分配器 (Object Cache)

`MemCache` 是内核对象缓存，借鉴 Zircon `ObjectCache` 和 Linux SLUB：

```
MemCache
├── CpuCache (per-CPU)         ← 快速路径：活跃 slab + partial 链表
├── NodeCache (per-NUMA)       ← 慢速路径：per-node partial slab 链表
└── g_global_caches            ← 全局缓存注册表
```

## 7. 实现状态一览

| 组件 | 状态 | 备注 |
|------|------|------|
| MemoryModel (sparse section tree) | **完整实现** | init, add_range, pfn转换 |
| PhysMap | **完整实现** | 直接映射窗口 |
| FrameSet (buddy allocator) | **完整实现** | 分配/释放/合并/分裂 |
| PmNode 初始化和引导 | **完整实现** | Zone 创建, 跨节点连接 |
| PmNode::alloc_frames 快路径 | **完整实现** | fallback 链 |
| PmNode::direct_reclaim/compact | **桩代码** | 返回0 |
| Per-CPU frame cache | **桩代码** | kMaxPcpuCacheOrder = 0 |
| PageFrame 层次 | **完整实现** | BitFields 压缩 |
| PageQueues 入队/旋转 | **完整实现** | |
| PageQueues Runner 线程 | **桩代码** | run() 为空 |
| VmAspace 创建/缺页处理 | **完整实现** | |
| VmAspace::clone | **桩代码** | Unimplemented |
| VmArea 树管理 | **完整实现** | create, destroy, find_spot |
| VmArea::unmap/protect | **桩代码** | Unimplemented |
| VmMapping 创建/映射/取消映射 | **完整实现** | 含 protect |
| VmMapping::fault | **桩代码** | 空函数体 |
| VmObjectPaged::commit_range | **完整实现** | |
| VmObjectPaged::decommit/read/write | **桩代码** | |
| VmObjectPhysical | **完整实现** | |
| VmCowPages Cursor 页面分配 | **完整实现** | |
| VmCowPages 异步页面请求 | **桩代码** | |
| CpuLocal Mapper 基础设施 | **完整实现** | |
| CpuLocal 动态分配/bootstraap | **桩代码** | |
| MemCache 布局计算/自举 | **完整实现** | |
| Slab::get_object/put_object | **桩代码** | 空函数体 |
| MemCache::create_boot | **桩代码** | |
| Stack 分配 | **完整实现** | 16KB 默认 |
| 内核符号边界 (BSS/Data/Code) | **完整实现** | linker script 提供 |

## 8. 设计要点与改进建议

### 8.1 当前设计优点
1. **三层次清晰分离**：PMM / VMM / VMO 职责明确，互不耦合
2. **Zircon 兼容的接口**：VmObject, VmAspace, VmMapping 的 API 与 Zircon 高度一致
3. **NUMA 感知**：已实现节点距离矩阵、最优路由、交叉节点连接
4. **紧凑的 PageFrame**：使用 BitFields 压缩，内存效率极高
5. **空闲地址搜索**：`find_spot()` 支持 ASLR 的随机化地址分配

### 8.2 可改进之处
1. **缺页处理不完整**：`VmMapping::fault()` 为空，当前无法处理真正的按需分页
2. **页面回收未实现**：LRU/MRU runner 线程体为空，无法回收内存
3. **direct_reclaim / direct_compact**：慢速分配路径缺失，NoFail 分配可能无限自旋
4. **Slab 分配器未完成**：get_object/put_object 为空，kernel malloc 实际不可用
5. **VMO 操作缺失**：decommit, read, write, supply_pages, take_pages 未实现
6. **clone 未实现**：无法实现 fork() 风格的地址空间复制

### 8.3 从 Linux/Zircon 可借鉴的设计
- **Linux reverse mapping (rmap)**：`VmPage` 已有 `m_vmo` 逆向指针，但缺少完整的 rmap 链（一个物理页可能被多个 VMO 共享），这在 swap 时不可或缺
- **Linux KSM (Kernel Same-page Merging)**：当前 COW 页面管理没有合并相同页的能力
- **Zircon VmPageList**：使用紧凑的 page list 结构（类似 XArray），当前 `VmPageMap` 仅是最小骨架
- **Zircon page_source**：VMO 的异步页面源（user pager, discardable VMO），当前仅有 `PageSource` 空壳声明
- **Linux watermark-based reclaim**：当前 `PmZone` 有 watermark 定义但慢路径无实现
- **Linux compaction**：内存碎片整理（compaction）需要实现 movable page 迁移机制
