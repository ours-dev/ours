# 09 — ACPI 解析库与系统库

## 1. 概述

`kernel/lib/acpi/` 是一个自包含的 ACPI 解析库，用于在启动早期解析 ACPI 表，获取 APIC、NUMA、HPET 等信息。辅以三个系统库：`bits`（位操作）、`concurrent`（并发原语—空）、`lockdep`（锁依赖追踪）。

## 2. ACPI 解析库

### 2.1 设计架构

```
IPhysToVirt                    ← 平台提供的物理→虚拟映射接口
    │
    ▼
AcpiParser                     ← 核心解析器
    │
    ├── from_rsdp(PhysAddr)    ← 从 RSDP 创建，解析 RSDT/XSDT
    ├── num_tables()
    ├── get_table_at_index()   ← 获取第 N 个表的 TableHeader*
    │
    ▼
辅助函数:
├── get_table_by_signature()   ← 按签名查找表
├── get_table_by_type<T>()     ← 模板包装
├── enumerate_io_apics()       ← MADT → IO APIC
├── enumerate_local_apics()    ← MADT → Local APIC
├── enumerate_interrupt_overrides()  ← MADT → IRQ 重定向
├── enumerate_numa_region()    ← SRAT → NUMA 内存区域
├── enumerate_cpu_numa_pairs() ← SRAT → CPU NUMA 对
├── count_numa_domains()       ← SLIT → 域数量
└── enumerate_numa_domain_distance() ← SLIT → 距离矩阵
```

### 2.2 核心接口

```cpp
// 平台必须实现此接口以映射 ACPI 表到虚拟地址
class IPhysToVirt {
    virtual Result<const void*> phys_to_virt(PhysAddr addr, usize size) = 0;
};

// 表遍历接口
class IAcpiParser {
    virtual usize num_tables() = 0;
    virtual Result<const TableHeader*> get_table_at_index(usize) = 0;
};
```

### 2.3 RSDP 解析

```
from_rsdp(mapper, rsdp_phys_addr):
  1. map RSDP → 验证签名 ("RSD PTR ") + 校验和
  2. 如果 revision >= 2 → map RsdpV2 → 获取 xsdt_addr (64-bit)
  3. 否则回退 rsdt_addr (32-bit)
  4. 优先 XSDT: map + validate → num_tables = length / 8
  5. 回退 RSDT: map + validate → num_tables = length / 4
  6. 返回 AcpiParser 实例
```

### 2.4 标准 ACPI 结构

所有 ACPI 表结构使用紧凑打包 (packed struct)，签名标识：

```cpp
struct TableHeader {     // 36 字节
    Signature sign;      // "APIC", "SRAT", "HPET", "SLIT", "DBG2" ...
    u32 length;
    u8  revision;
    u8  checksum;
    char oem_id[6];
    char oem_tableid[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
};

struct EntryHeader {     // 子表条目头
    u8 type;
    u8 length;
};
```

**类型安全下转**：
```cpp
template<typename Dest, typename Src>
auto downcast(Src* src) -> Dest* {
    static_assert(&Dest::header == static_cast<EntryHeader*>(static_cast<Dest*>(nullptr)));
    if (src->size() < sizeof(Dest)) return nullptr;
    return static_cast<Dest*>(src);
}
```

### 2.5 MADT — 多 APIC 描述表

完整定义了 ACPI 6.x 规范的 15 种子表条目类型：

| Type | 结构名称 | 用途 |
|------|---------|------|
| 0 | LocalApicEntry | 本地 APIC (ACPI ID → APIC ID) |
| 1 | IoApicEntry | I/O APIC (地址, 全局 IRQ 基址) |
| 2 | InterruptOverrideEntry | ISA IRQ → 全局 IRQ 重定向 |
| 3 | NmiEntry | NMI 源配置 |
| 4 | LocalApicNmiEntry | 每 CPU 的 LINT 配置 |
| 5 | IoSapicEntry | I/O SAPIC |
| 6 | LocalSapicEntry | 本地 SAPIC |
| 7 | PlatformInterruptSourceEntry | 平台中断源 |
| 8 | LocalX2ApicEntry | x2APIC 本地条目 |
| 9 | IoX2ApicEntry | x2APIC I/O 条目 |
| 10 | LocalX2ApicNmiEntry | x2APIC NMI 配置 |
| 11-15 | GIC 各种条目 | ARM GIC 支持 |

MADT 枚举使用模板方法模式：

```cpp
template<typename Entry, typename Visitor>
Status enumerate_entries(IAcpiParser& parser, Visitor&& commit) {
    auto madt = get_table_by_signature(parser, "APIC");
    BinaryReader reader(madt->payload);
    while (!reader.eof()) {
        EntryHeader header; reader >> header;
        if (header.type == Entry::kType) {
            auto* entry = downcast<Entry>(&header);
            commit(*entry);            // 回调处理
        }
        reader.skip(header.length - sizeof(EntryHeader));
    }
}
```

### 2.6 SRAT — 系统资源亲和性表

三种条目类型：
- **ProcessorAffinityEntry** (Type 0): APIC ID → proximity domain
- **MemoryAffinityEntry** (Type 1): base/size → proximity domain (32-bit 高低拼接 64-bit)
- **ProcessorX2ApicAffinityEntry** (Type 2): x2APIC ID → proximity domain

### 2.7 SLIT — 系统局域信息表

提供 NUMA 节点间的距离矩阵 (locality_count × locality_count)。

### 2.8 HPET 表 + MMIO 寄存器

不仅定义了 ACPI HPET 表结构，还完整定义了 HPET MMIO 寄存器布局：

```cpp
struct HpetRegs {
    HpetGeneralCapAndIdRegs general_caps;     // 版本, 定时器数量, 周期
    HpetGeneralConfigRegs general_config;      // 启用/禁用
    u64 general_intr_status;                   // 中断状态
    u64 main_counter_value;                    // 主计数器
    HpetTimerRegs timers[];                    // 可变长度定时器数组
};

struct HpetTimerRegs::ConfigAndCapRegs {  // 64-bit 位域:
    Field<u32, 0, 1>   Mask;              // 中断屏蔽
    Field<u32, 1, 1>   TriggerMode;       // 触发模式 (边沿/电平)
    Field<u32, 2, 1>   IntrEnable;        // 中断启用
    Field<u32, 3, 1>   TimerType;         // 周期性/一次性
    Field<u32, 4, 1>   PeriodicCap;       // 周期能力
    Field<u32, 13, 1>  FsbIntrEn;         // FSB 中断
    Field<u32, 9, 5>   IrqRoute;          // IRQ 路由
    // ... 共 14 个命名位域
};
```

使用 `ustl::BitFields<>` 命名位域技术，远比 C 语言的 `#define` 面具安全。

### 2.9 DBG2 — 调试端口表

定义了表结构、设备类型枚举和 `DebugConfig` 结构体，但解析函数 `get_debug_port()` 未实现（`debug.cpp` 为空）。

## 3. Bits 库 (`system/lib/bits/`)

提供 C++ 枚举的位掩码操作支持：

```cpp
// 声明枚举支持位掩码操作
enum class MyFlags { A = 1, B = 2, C = 4 };
BITS_IMPL_BITMASK(MyFlags);  // SFINAE 探测注册

// 使用
MyFlags x = MyFlags::A | MyFlags::B;
x |= MyFlags::C;
bool has_a = !!(x & MyFlags::A);

// 用户自定义字面量
auto flag = 3_bit;  // = 1 << 3 = 8
```

基于 SFINAE 的检测：`bits::bitmask::ImplementedV<Enum>` 在编译期判断枚举是否支持位掩码。

## 4. Lockdep 库 (`system/lib/lockdep/`)

### 4.1 锁类系统

```cpp
class LockClass {
    Flags m_flags;           // IrqSafe, Nestable, Leaf, MultiAcquire 等
    ValidatorState m_validator_state;

    LockClassId id() const;                       // 返回 this 指针作为 ID
    bool is_leaf() const;
    bool is_nestable() const;
    static LockResult add_dependency(Id prev, Id next);  // 桩代码
};
```

### 4.2 锁上下文 (每线程)

```cpp
class LockContext {
    Slist<AcquiredLockEntry> m_acquired_locks;  // 当前持有的锁链表

    LockResult acquire(AcquiredLockEntry& entry) {
        // 遍历已持有的锁:
        //   - old is Leaf → AcquireAfterLeaf 违规
        //   - same class + same object → Reentrance
        //   - same class + out of order → InvalidNesting / AlreadyAcquired
        //   - different class → add_dependency(new, old)
        // 然后加入链表
    }
};
```

### 4.3 RAII Guard

```cpp
template<typename LockType, typename... Options>
class Guard {
    LockType& m_lock;
    Guard(LockType& lock) : m_lock(lock) { lock.acquire(); }
    ~Guard() { m_lock.release(); }
    void unlock();  // 提前释放
    void unguard(); // 禁止析构释放
};
```

使用 CTAD 推导，无需模板参数：`Guard guard{some_lock};`

### 4.4 锁特性声明

```cpp
LOCK_DEP_TRAITS(SpinLock, LockFlags::IrqSafe);
LOCK_DEP_TRAITS(Mutex, LockFlags::None);
LOCK_DEP_TRAITS(PageRequestMutex, LockFlags::Nestable | LockFlags::Leaf);
```

### 4.5 状态保存器

```cpp
LOCKDEP_STATE_SAVER(SpinLock, IrqSave, SpinLockIrqSave);
LOCKDEP_STATE_SAVER(SpinLock, PreemptDisable, SpinLockPreemptDisable);
```

## 5. Concurrent 库

`system/lib/concurrent/` 仅有 `.git/` 和 `LICENSE`，**完全为空**。计划中是并发原语库，但目前无任何实现。

## 6. 实现状态一览

### ACPI 库

| 组件 | 状态 |
|------|------|
| RSDP v1/v2 解析+校验 | **完整实现** |
| RSDT/XSDT 遍历 (偏好 XSDT) | **完整实现** |
| 按签名查找表 (checksum 验证) | **完整实现** |
| MADT: 15 种条目类型 + 3 种枚举器 | **完整实现** |
| SRAT: 3 种条目 + NUMA 区域/CPU 枚举 | **完整实现** |
| SLIT: 距离矩阵枚举 | **完整实现** |
| HPET: 表 + MMIO 寄存器 (位域) | **完整实现** |
| DBG2: 表结构 | **定义完整** |
| DBG2 解析 | **桩代码** (debug.cpp 为空) |
| Safe downcast | **完整实现** |

### Bits 库

| 组件 | 状态 |
|------|------|
| 枚举位掩码 | **完整实现** |

### Lockdep 库

| 组件 | 状态 |
|------|------|
| LockClass + 标志 | **完整实现** |
| LockContext acquire 验证 | **完整实现** |
| Guard\<T\> RAII | **完整实现** |
| LockTraits + LOCK_DEP_TRAITS 宏 | **完整实现** |
| StateSaver + LOCKDEP_STATE_SAVER | **完整实现** |
| LockContext release | **完整实现** |
| LockClass::add_dependency | **桩代码** (空) |
| LoopDetector | **桩代码** (未实现) |
| ScopedGuard (多锁) | **桩代码** (声明无实现) |
| LockDep 模板 (锁集成) | **空骨架** |

### Concurrent 库

| 组件 | 状态 |
|------|------|
| 全部 | **空** |

## 7. 设计要点与改进建议

### 7.1 ACPI 库
- **已知问题**：README 指出同一表被反复解析，应改为静态观察者模式缓存解析结果
- `AcpiParser` 同时持有 RSDT 和 XSDT 指针但只使用 XSDT，可清理
- DBG2 解析需实现以支持早期调试输出

### 7.2 Lockdep 库
- 循环检测算法（BFS/DFS）是 lockdep 最核心的功能，当前 LoopDetector 为空
- `LockDep` 集成模板为空骨架，锁类无法真正接入 lockdep
- `ScopedGuard` 多锁获取未实现

### 7.3 Bits 库
- 已完整实现，无需改进
