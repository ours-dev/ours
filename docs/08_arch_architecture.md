# 08 — 架构抽象层 (Architecture Abstraction Layer)

## 1. 概述

架构抽象层 (arch layer) 通过编译时类型协议 + C++20 Concept 验证的模式，将架构相关代码与通用内核代码分离。当前仅有 x86_64 实现，RISC-V 占位符为空。

核心分离原则：

```
kernel/main/  (通用内核代码)
    │
    │  #include <ours/arch/xxx.hpp>      ← 编译时协议
    │  static_assert(concepts::ArchXxx<T>)  ← Concept 验证
    │  T m_arch;                          ← 直接嵌入 (组合而非继承)
    │
    ▼
kernel/arch/x86/main/  (x86_64 实现)
    │
    │  定义 ArchVmAspace, ArchThread, ArchCpuLocal 等
    │  提供 arch_ 前缀自由函数
    │  实现 INIT_CODE 引导函数
    │
    ▼
kernel/arch/riscv/  (未来 RISC-V 实现)
```

## 2. 编译时协议机制

### 2.1 Include Guard 强制

通用内核代码使用 `#include` 协议确保架构头文件存在：

```cpp
// kernel/main/include/ours/mem/vm_aspace.hpp
#if !__has_include(<ours/arch/vm_aspace.hpp>)
#   error "The header <ours/arch/vm_aspace.hpp> is required by [protocol.module.mem]"
#endif
#include <ours/arch/vm_aspace.hpp>
```

这要求每个架构移植必须在 `<ours/arch/>` 路径下提供特定头文件。

### 2.2 C++20 Concept 验证

```cpp
// kernel/main/include/ours/mem/concepts/arch-vm-aspace.hpp
template<typename T>
concept ArchVmAspace = requires(T t, VirtAddr va, PhysAddr pa, usize n, MmuFlags f) {
    { t.init() }              -> SameAs<Status>;
    { t.map(va, n, pa, f) }  -> SameAs<Status>;
    { t.map_bulk(...) }      -> SameAs<Status>;
    { t.unmap(va, n, ...) }  -> SameAs<Status>;
    { t.protect(va, n, f) }  -> SameAs<Status>;
    { t.query(va, ...) }     -> SameAs<Status>;
    { t.mark_accessed(va, n)} -> SameAs<Status>;
    { t.harvest_accessed() } -> SameAs<Status>;
};

// 使用处
static_assert(concepts::ArchVmAspace<ArchVmAspace>);
```

### 2.3 组合而非继承

通用内核类直接嵌入架构类型作为成员：

```cpp
class VmAspace {
    ArchVmAspace m_arch;   // x86: 包含 PageTable 实例
};

class Thread {
    ArchThread m_arch_thread;   // x86: X86Thread, 包含 SP/FS/GS base
};

class IdleThread {
    ArchIdleThread m_arch;      // x86: HLT/MWAIT 指令
};
```

这与 Linux 的 `arch/` 目录下各种 `asm/` 头文件模式类似，但使用 C++ Concept 提供编译期接口校验。

## 3. 架构提供的类型

| 通用内核类型 | 架构类型 (x86) | 职责 |
|-------------|---------------|------|
| `ArchVmAspace` | class (包含 `PageTable`) | 页表操作 (map/unmap/protect) |
| `ArchThread` | `= X86Thread` | 寄存器上下文 + 线程切换 |
| `ArchCpuLocal` | struct (GS 段操作) | per-CPU 存储基础 |
| `ArchIdleThread` | class | CPU 空闲指令 (HLT/MWAIT) |
| `ArchPmmZoneConfig` | TypeList | Zone 的物理地址范围 |

## 4. 自由函数接口 (arch_ 前缀)

通用内核调用以 `arch_` 为前缀的自由函数，由架构实现：

| 自由函数 | x86 实现位置 | 用途 |
|---------|------------|------|
| `init_arch_early()` | `init.cpp` | 早期初始化 (IDT, percpu, MMU) |
| `init_arch()` | `init.cpp` | 完整初始化 (GDT, 全部 IDT, MMU setup) |
| `arch_mp_send_ipi()` | `mp.cpp` | 发送 IPI 到目标 CPU |
| `arch_mp_reschedule()` | `mp.cpp` | 发送重调度 IPI |
| `arch_mp_suspend()` | `mp.cpp` | 挂起 CPU (桩代码) |
| `arch_handle_exception()` | `faults.cpp` | 异常分发 (从汇编调用) |
| `arch_time_init()` | `time/time.cpp` | 启动 PIT 作为初始时钟 |
| `arch_init_irq()` | `irq/init.cpp` | 初始化 IRQ (桩代码) |
| `arch_activate_cpu()` | `mp.cpp` | CPU 上线 (桩代码) |
| `arch_deactivate_cpu()` | `mp.cpp` | CPU 下线 (桩代码) |
| `arch_build_uspace_irqframe()` | `entry.hpp` (内联) | 构造用户态中断帧 |
| `arch_enter_uspace()` | `entry.hpp` → `x86_enter_uspace()` | 进入用户态 |

## 5. x86 关键实现细节

### 5.1 Per-CPU 存储 — GS 段

```cpp
struct ArchCpuLocal {
    static void install(void* base);  // WRMSR(IA32_GS_BASE, base)
    template<typename T>
    static T read(usize offset);      // GS-relative load
};
```

通用 `CpuLocal` 类在此基础上添加 CPU 偏移计算。

### 5.2 Per-CPU 数据结构

```cpp
struct X86PerCpu {  // GS 段指向的 per-CPU 数据
    X86Tss tss;                    // 任务状态段 (含 IO 位图)
    u32 apic_id;
    CpuNum cpunum;
    InterruptStack intr_stack[kMaxNumStacks];  // 5 个 IST 栈
};

// 在链接器中每 CPU 复制
CPU_LOCAL inline X86PerCpu g_x86_pcpu;
```

### 5.3 线程上下文切换 (汇编)

```
x86_switch_context(prev_sp_ptr, next_sp):
    push rbx, rbp, r12-r15      // 保存被调用者保存的寄存器
    mov [rdi], rsp              // *prev = RSP
    mov rsp, rsi                // RSP = next
    pop r15, r14, r13, r12, rbp, rbx  // 恢复
    ret                         // 跳转到保存的 RIP
```

**完整切换流程**：
```
Thread::switch_thread(curr, next)
  → ArchThread::switch_thread()
      ├── 更新 TSS.sp0 = next.kernel_stack_top  (ring0 栈切换)
      ├── swap_gsbase_and_fsbase()               (FSGSBASE 优化或 MSR)
      └── x86_switch_context(&curr.sp, next.sp)
```

### 5.4 中断/异常入口

汇编 `interrupt_common` (在 `exception.S` 中)：
- 保存全部寄存器 (push regs + push vector)
- 寄存器清零 (Spectre/Meltdown 缓解)
- CPL != 0 检测 (来自用户态则 swapgs)
- 调用 `arch_handle_exception()`
- 恢复寄存器
- `iretq` 返回

### 5.5 AP 引导 (Bootstrap)

```
主 CPU (BSP):
  make_bootstrap_area()
    → 复制 startup trampoline 到 <64KB 低内存
    → 设置 GDT, PGD, SP, 入口点
  x86_bring_aps()
    → INIT IPI → STARTUP IPI (trampoline vector)

AP:
  trampoline (16-bit → 32-bit → 64-bit)
    → 加载 bootstrap PGD
    → 长模式 → 64-bit
    → x86_start_nonboot_cpu()
        → init CPU-local
        → init local APIC
        → 加载 GDT/IDT/TSS
        → enable syscall
        → start_nonboot_cpu() → become_idle()
```

### 5.6 地址空间布局

```
KERNEL_ASPACE_BASE     = 0xffffff8000000000       (512 GB)
PHYSMAP_VIRT_BASE      = 0xffffff8000000000       (物理映射窗口)
PHYSMAP_SIZE           = 32 GB
KERNEL_LOAD_BASE       = 0xffffff8800000000       (+32GB+1GB)
USER_ASPACE_BASE       = 0xffffff8000000000       (与内核基址相同 — 双用途)
USER_ASPACE_SIZE       = 0x0000008000000000       (512 GB)
KERNEL_LOAD_ADDR       = 0x100000                 (物理加载地址)

PML4 布局:
  [0]     = physmap (0..32GB)
  [1..510] = NULL
  [511]   = 内核页目录
```

### 5.7 MMU 特性

```cpp
// x86_init_mmu_percpu() 设置:
CR0:  WP=1 (写保护), NW=0, CD=0 (缓存启用)
CR4:  SMEP=1, SMAP=1, PCID=1, PGE=1, LA57=* (条件)
EFER: NXE=1 (不可执行页)

// x86_init_mmu_early():
从 CR3 保存内核 PGD
清除 PGD[0] (拆除恒等映射，保留 PGD[511])
通过 CPUID 获取物理/虚拟地址位数
```

**全局 TLB 刷新**：优先使用 `INVPCID` 指令，回退到写 CR3。

## 8. Init Hook 注册顺序

几个关键的架构层 init hooks：

| Hook | 级别 | 函数 |
|------|------|------|
| Bootstrap16 Buffer | `PlatformInitLevel::PrePmm` | `make_bootstrap_area` |
| NumaInit | `PlatformInitLevel::PrePmm + 1` | `x86_init_numa` |
| FreeBootstrap16 | `InitLevel::Platform` | `free_bootstrap16_buffer` |
| InitBspApicTimer | `InitLevel::Irq + 4` | 校准 APIC 定时器 |
| InitTimerDevice | `InitLevel::Irq + 1` | PIT/HPET 初始化 |

## 9. 实现状态一览

| 组件 | 状态 | 备注 |
|------|------|------|
| IDT 初始化 (256 门) | **完整实现** | IST 绑定, RO 重映射 |
| 异常分发 (全部向量) | **完整实现** | #PF 处理完整 |
| Local APIC (init, EOI, ID, timer) | **完整实现** | xAPIC + x2APIC |
| IO-APIC (init, IRQ 路由, ISA override) | **完整实现** | |
| IPI (self, uni, multi, broadcast) | **完整实现** | |
| GDT + TSS (per-CPU) | **完整实现** | |
| FS/GS context switch (FSGSBASE opt) | **完整实现** | |
| Syscall enable (MSR) + handler | **完整实现** | |
| Context switch 汇编 | **完整实现** | |
| User space 入口 (iretq) | **完整实现** | |
| MMU init (CR0/4/EFER, PCID, SMEP/SMAP) | **完整实现** | |
| Physmap 窗口 | **完整实现** | 32GB |
| 页表操作 (map, unmap, protect, query) | **完整实现** | |
| HPET 初始化 | **完整实现** | 时钟源 + 定时器通道 |
| PIT 初始化 | **完整实现** | 回退时钟 |
| Bootstrap trampoline | **完整实现** | AP 引导 |
| AP bring-up (INIT+SIPI) | **完整实现** | |
| AP boot 入口 | **完整实现** | |
| 5-level paging (LA57) | **条件支持** | 取决于 CPUID |
| PCID 分配 | **完整实现** | 每 aspace 分配 PCID |
| arch_mp_suspend | **桩代码** | 空函数体 |
| arch_activate/deactivate_cpu | **桩代码** | Unimplemented |
| arch_init_irq | **桩代码** | 空函数体 |
| x86_handle_ipi_generic/interrupt | **桩代码** | 空函数体 |
| ArchVmAspace::harvest/mark_accessed | **桩代码** | Unsupported |
| EPT/VMX 页表同步 | **桩代码** | 无实现 |
| IO-APIC 亲和性 | **桩代码** | 返回 Ok, 无实际操作 |
| topology.cpp (phys) | **桩代码** | 空文件 |

## 10. 设计要点与改进建议

### 10.1 当前设计优点
1. **编译时类型安全**：C++20 Concept 在编译期验证架构接口，避免运行时意外
2. **零开销抽象**：架构类型直接嵌入，无虚函数调用或间接层
3. **Include Guard 强制**：新增架构移植时必须提供所有必需头文件
4. **INIT_CODE 回收**：引导代码标记 `INIT_CODE`，启动后可释放内存

### 10.2 可改进之处
1. **RISC-V 空壳**：仅有 `BUILD.bazel` 占位符
2. **ArchHandOff 为空**：x86 特定交接数据未被填充（如 trampoline PGD、GDT 地址）
3. **CpuTopologyNode 为空**：CPU 拓扑信息未传递
4. **EPT 未实现**：虚拟机支持的基础缺失
5. **IO-APIC 亲和性**：无法设置中断亲和性
6. **harvest_accessed**：无法高效实现 accessed bit 回收

### 10.3 从 Linux/Zircon 可借鉴的设计
- **Linux asm-generic**：提供默认实现，架构只需覆盖差异部分
- **Linux cpu_ops**：标准化的 CPU 操作接口 (bringup, takedown, idle)
- **Zircon arch_ops**：架构操作结构体，支持运行时选择（比编译时 Concept 更灵活）
- **Zircon gic/virt**：已有 ARM GIC 和虚拟化中断控制器抽象
