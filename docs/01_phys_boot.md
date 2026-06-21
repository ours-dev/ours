# 01 — 物理启动子系统 (Phys/Boot)

## 1. 概述

Phys 子系统负责内核最早期的启动流程：从 bootloader 接手控制权，初始化物理内存分配器（BootMem），建立临时页表，加载真正的内核镜像，最后通过 `Handoff` 结构体将控制权传递给内核主体。

在 Zircon 中，对应的是 `physboot` —— 一个独立的 ELF 程序，它不链接任何内核代码，自包含地完成上述所有工作。本项目也遵循这一设计。

## 2. 整体架构

```
Bootloader (Multiboot2 / Linux Boot Protocol)
  │
  ▼
phys_main()                          ← 架构相关入口 (x86: phys/boot-shim/shim.cpp)
  │
  ▼
obi_main(PhysAddr obi)              ← phys/phys-load.cpp
  │
  ├── Console::init_early()          ← 早串口初始化
  ├── Aspace::init()                 ← 分配页表根目录 (PGD)
  ├── init_memory(obi, &aspace)     ← 初始化 BootMem + 构建临时页表
  │     ├── obi_init_memory()       ← 从启动镜像解析内存布局 (TODO)
  │     ├── arch_detect_memory()    ← 架构相关的物理内存探测 (弱符号, x86 有实现)
  │     └── create_identity_map()   ← 为全部 RAM 创建恒等映射
  ├── KernelPackage::init(obi)      ← 在内核包中查找并解压内核镜像
  ├── KernelBoot::init(package)     ← 识别 ELF/Raw 镜像类型，生成 KASLR 偏移
  ├── KernelBoot::load()            ← 分配物理内存，加载镜像段
  ├── relocate_kernel()             ← 应用 ELF 重定位
  └── KernelBoot::boot()
        ├── build_kernel_aspace()   ← 建立最终内核页表 (KASLR + physmap)
        └── do_handoff()            ← 填充 Handoff 结构体，跳转到内核入口
```

### 与 Zircon 的对比

| 概念 | Zircon | 本项目 |
|------|--------|--------|
| 早内存分配器 | `BootAllocator` (region-based) | `bootmem::MemBlock` (region-based) |
| 临时页表 | `Aspace` (identity map + kernel map) | `Aspace` (同上) |
| 镜像加载 | `ZbiBoot` → `ZbiKernelImage` | `KernelBoot` → `ElfImage` / `RawImage` |
| 交接结构体 | `zbi_header_t` + boot items | `Handoff` 结构体 (≤一页) |
| 初始化顺序 | init_early_console → paging → memory → ... | 完全一致 |

### 与 Linux 的对比

| 概念 | Linux | 本项目 |
|------|-------|--------|
| 早内存分配器 | `memblock` (memblock_alloc) | `bootmem::MemBlock` |
| 临时页表 | `early_ioremap` + `__initial_page_table` | `phys::Aspace` |
| 镜像加载 | 自解压 + `extract_kernel` | `ElfImage::load_at()` |
| 交接结构体 | `boot_params` (struct setup_data) | `Handoff` |
| 内存探测 | E820 / UEFI Memory Map → memblock_add | `arch_detect_memory()` → MemBlock::add() |

Linux 的 memblock 和本项目的 BootMem 设计几乎一致：都是 region-based 的 early allocator，支持 top-down/bottom-up 分配策略。

## 3. 关键数据结构

### 3.1 Handoff

```cpp
struct Handoff {
    u32 magic;                          // 0xA1B2C3D4 用于自校验
    u32 nr_cpus;
    void* efi_system_table;
    PhysAddr acpi_rsdp;                 // ACPI RSDP 物理地址
    MemoryHandoff mem;                  // 内存管理交接
    Span<CpuTopologyNode> cpu_topology; // CPU 拓扑信息 (目前为空壳)
    ArchHandOff arch;                   // x86 架构特定数据 (目前为空)
};

struct MemoryHandoff {
    EarlyMem bootmem;                   // 封装了 BootMem 的函数对象
    PhysAddr kernel_load_addr;          // 内核加载的物理地址
};
```

设计约束：`sizeof(Handoff) <= PAGE_SIZE`，确保不会跨页。

### 3.2 EarlyMem (函数对象封装)

```cpp
struct EarlyMem {
    Fn<auto(PhysAddr, usize) -> void>           protect;     // 保留物理内存区域
    Fn<auto(PhysAddr, usize, NnId) -> void>      set_node;    // 设置 NUMA 节点
    Fn<auto(usize, usize, ...) -> PhysAddr>     allocate;    // 分配物理内存
    Fn<auto(PhysAddr, usize) -> void>           deallocate;  // 释放物理内存
    Fn<auto(IterationContext&) -> bool>         iterate;     // 迭代内存区域
};
```

这是一个精妙的设计：通过 `std::bind_front` 将全局 `BootMem` 实例的方法绑定为类型擦除的函数对象。内核主体通过这组回调可以继续使用同一个 BootMem，而不需要知道其具体类型（避免了对 bootmem 库的链接依赖）。

### 3.3 BootMem (bootmem::MemBlock)

```cpp
template <typename Collection = RegionVector<>>
class MemBlock {
    RegionList<Collection> memories_;     // 可用物理内存区域
    RegionList<Collection> reserved_;     // 已分配/保留的内存区域

    PhysAddr allocate(usize size, usize align);            // 分配
    PhysAddr allocate_bounded(size, align, lo, hi, nid);  // 受限分配
    void      deallocate(PhysAddr base, usize size);       // 释放
    void      protect(PhysAddr base, usize size);          // 标记为保留
    void      set_node(PhysAddr base, usize size, NnId);   // 设置 NUMA 节点
    void      trim(usize alignment);                        // 对齐整理
};
```

支持的操作：
- **Bottom-up / Top-down 分配策略**（由 `AllocationControl` 控制）
- **NUMA 感知**：每个 `Region` 携带 `NnId`
- **Bounded allocation**：在指定物理地址范围内分配
- **自举扩容**：`RegionVector` 满了后可以回调 `MemBlock` 获取更多内存存储 `Region` 元数据

这是对 Linux `memblock` 的忠实复刻，同时借鉴了 Zircon `BootAllocator` 的设计。

### 3.4 Aspace (临时地址空间)

```cpp
struct Aspace {
    LowerPaging lower;   // 下层页表 (x86: 4-level)
    UpperPaging upper;   // 上层页表 (仅当 dual-aspace 时存在)

    Status init();       // 分配 PGD
    Status map(VirtAddr va, usize n, PhysAddr pa, MmuFlags flags);
    void    install();   // 安装到当前 CPU (写 CR3)
};
```

`kDualAspace` 在 x86 4-level/5-level 混合场景下为 true，此时需要同时维护两套页表。

## 4. 引导器兼容性

`phys/boot-shim/shim.cpp` 将不同引导协议的入口点转换为统一的 `phys_main`:

| 引导协议 | 入口符号 | 转换逻辑 |
|---------|---------|---------|
| Multiboot2 | `_multiboot_start` | shim 提取 multiboot info → `phys_main(PhysAddr)` |
| Linux Boot Protocol | `_linux_start` | shim 设置 zero page → `phys_main(PhysAddr)` |

## 5. 镜像加载

### 5.1 IKernelImage 接口

```cpp
class IKernelImage {
    virtual Status init(Span<u8> file) = 0;
    virtual usize  size() const = 0;
    virtual PhysAddr load_at(PhysAddr dest) = 0;
    virtual Status fix_mapping(Aspace& aspace) = 0;
    virtual Status relocate(usize offset) = 0;
};
```

### 5.2 ELF 镜像

`ElfImage` 通过 `eftl::elf` 库解析 ELF 文件，遍历 PT_LOAD 段按 p_align 对齐复制到目标物理地址。支持 `R_X86_64_RELATIVE` 重定位以配合 KASLR。

### 5.3 Raw 镜像

`RawImage` 是一个简单的内存拷贝：直接将整个文件（减去 `ObiRawExeHeader`）拷贝到目标地址。

## 6. KASLR 支持

`KernelBoot::gen_load_bias()` 生成随机偏移：
1. 尝试 `arch::rand()`（硬件随机数生成器，如 RDRAND）
2. 回退到 TSC (Time Stamp Counter)
3. 取模限制在 1GB 范围内

当前 `obi_main` 中 `aslr_disabled = true`（调试默认），禁用 KASLR。

## 7. 实现状态一览

| 组件 | 状态 | 备注 |
|------|------|------|
| BootMem (MemBlock, RegionList, RegionVector) | **完整实现** | 含单元测试 |
| BootMem 算法 (lookup, for_each) | **完整实现** | |
| Console 框架 | **完整实现** | 驱动注册/激活/格式化输出 |
| Aspace::init, map, alloc_page_table | **完整实现** | |
| Aspace::unmap | **桩代码** | 返回 Unimplemented |
| RawImage 加载 | **完整实现** | |
| ElfImage 加载 + 重定位 | **完整实现** | |
| ElfImage::fix_mapping | **桩代码** | 需要实现 huge page split |
| KernelPackage 初始化 + 解压 | **完整实现** | copy_item 第三个参数待修复 (TODO) |
| KernelBoot 完整流程 | **完整实现** | |
| Handoff 填充 + 跳转 | **完整实现** | |
| obi_main 完整启动序列 | **完整实现** | |
| arch_detect_memory (x86) | *位于 arch 层* | E820/EFI 内存探测 |
| arch_init_memory (x86) | *位于 arch 层* | 架构页表构造 |
| Symbolize | **空壳** | 符号化功能未实现 |
| InitFs | **占位符** | 仅有 Span<u8> 别名 |

## 8. 设计要点与改进建议

### 8.1 当前设计的优点
1. **清晰的阶段分离**：phys 是一个独立程序，不链接内核，避免了依赖污染
2. **类型擦除的 Handoff**：`EarlyMem` 函数对象模式让内核主体无依赖地使用 bootmem
3. **一页约束**：`Handoff` 限制在 `PAGE_SIZE` 内，确保了交接的简洁性
4. **多启动协议支持**：通过 shim 层统一接口

### 8.2 可改进之处
1. **obi_init_memory 未完成**：从启动镜像解析内存布局的逻辑是 TODO，目前只走了 arch_detect_memory 回退路径
2. **InitFs 尚未实现**：完整的 init 文件系统解析（tar/cpio）缺失
3. **fix_mapping 未实现**：ELF 加载后无法更新页面权限（需要 huge page split 能力）
4. **CpuTopologyNode 为空壳**：CPU 拓扑信息未被填充，影响后续 NUMA/调度器初始化
5. **ArchHandOff 为空壳**：x86 特定的交接数据（如移植的 GDT/TSS 地址）未填充

### 8.3 从 Linux/Zircon 可借鉴的设计
- **Linux memblock**：支持 `MEMBLOCK_NOMAP` 标记（保留但不可直接访问的内存），当前 BootMem 缺少此语义
- **Zircon BootOptions**：从命令行/cmdline 解析启动参数，当前仅有空壳 `BootOptions`
- **Zircon ZBI**：Zircon 的启动镜像格式支持多 item（kernel、memconfig、cpu config、cmdline），本项目的 OBI 也类似但尚未完善
