# kernel/lib/acpi 代码审核

日期：2026-08-18。范围：`kernel/lib/acpi/**` 及其全部调用方。
基线：`out/bringup.x86`，`kernel_x86.debug_level_0/obj/kernel/lib/acpi/acpi.stamp` 可编译通过。

---

## 0. 现状速览

```
include/acpi/
  types.hpp                 30 行   整数别名（与 ours/prelude.hpp 重复）
  parser.hpp                85 行   IPhysToVirt / IAcpiParser / AcpiParser + 校验和 + 按签名查表
  apic.hpp / apic.cpp       24/55   3 个 enumerate_* 包装同一个模板循环
  numa.hpp / numa.cpp       30/124  4 个函数，各自重复"查表 + BinaryReader 循环"
  debug.hpp / debug.cpp     29/0    **debug.cpp 是空文件**，两个函数只有声明
  structures/
    header.hpp   91   Signature / EntryHeader / TableHeader / downcast / GenericAddress
    rdst.hpp     70   Rsdp / RsdpV2 / Rsdt / Xsdt
    madt.hpp    233   MADT + 16 个 entry
    srat.hpp    100   SRAT + 3 个 entry
    slit.hpp     22   SLIT
    dbg2t.hpp    59   DBG2
    hpet.hpp    220   HPET 表 **+ HPET MMIO 寄存器块（不属于 ACPI）**
```

调用方：`kernel/phys/acpi.cpp`、`kernel/platform/pc/acpi.cpp`、`kernel/arch/x86/mm/srat.cpp`、
`kernel/platform/pc/{hpet,interrupt}.cpp`、`kernel/arch/x86/kernel/time/hpet.cpp`。

---

## 1. 正确性缺陷（按严重程度排序）

### C1. `Rsdp::validate_signature()` 用了 `||`，签名校验形同虚设
`include/acpi/structures/rdst.hpp:12`
```cpp
return sig1 == kSignature1 || sig2 == kSignature2;   // 应该是 &&
```
RSDP 签名是 8 字节 `"RSD PTR "`，被拆成两个 4 字节半段。用 `||` 意味着只要前半段
（或后半段）对上就通过。任意一段内存只要恰好含 `"RSD "` 就会被当成合法 RSDP，
后续按 RSDP 布局解引用 `rsdt_addr` / `xsdt_addr`，直接得到野物理地址。

### C2. `count_numa_domains()` 越界读
`numa.cpp:90-98`
```cpp
auto slit = reinterpret_cast<slit::Table const *>(header);
return slit->locality_count;      // 没有任何长度检查
```
`get_table_by_signature()` 只保证 `length >= sizeof(TableHeader)`（36 字节），
而 `slit::Table` 是 44 字节。固件给出一个 length 在 36..43 的 SLIT，就会读到
映射区之外的 8 字节。

### C3. `enumerate_numa_domain_distance()` 先解引用后校验
`numa.cpp:107-113`
```cpp
auto const slit = reinterpret_cast<slit::Table const *>(header);
auto const num_domain = slit->locality_count;               // 先读
auto const table_size = static_cast<u64>(slit->header.size());
if (table_size < sizeof(slit::Table) || ...) { ... }        // 后查
```
和 C2 同源：`table_size < sizeof(slit::Table)` 这个检查写对了，但放在读之后。
（顺带一提，后面那个 `num_domain > (table_size - sizeof(Table)) / num_domain`
溢出检查本身是正确的，这点没问题。）

### C4. `enumerate_cpu_numa_pairs()` 把"CPU 未启用"当成致命错误
`numa.cpp:71-73, 80-82`
```cpp
if (!(processor->flags & Flag::Enabled)) {
    return Status::InternalError;      // 应该是 continue
}
```
同一文件的 `enumerate_numa_region()`（`numa.cpp:33`）对相同语义用的是 `continue`。
SRAT 里存在任意一个 disabled 处理器条目（热插拔槽位、超配的 vCPU 槽位，
QEMU `-smp maxcpus=` 就会产生），整个 CPU↔NUMA 绑定就直接失败返回。

### C5. `AcpiParser` 的公开默认构造函数留下未初始化成员
`parser.hpp:44, 52-55`
```cpp
AcpiParser() = default;
...
IPhysToVirt *m_p2v;          // 无 NSDMI
sdt::Rsdt const *m_rsdt;
sdt::Xsdt const *m_xsdt;
usize m_num_tables;
```
`AcpiParser p;` 是合法代码，`p.num_tables()` 返回垃圾值，
`get_table_at_index()` 随即解引用野指针。这个默认构造函数没有任何调用方需要，
纯粹是负担。

### C6. MADT 类型枚举从 type 5 起整体错位一位
`include/acpi/structures/madt.hpp:10-28`

ACPI 6.5 表 5.20 的实际编号 vs 当前枚举：

| 实际 | ACPI 规范 | 当前枚举 |
|---|---|---|
| 5 | Local APIC Address Override | `IoSapic` ❌ |
| 6 | I/O SAPIC | `LocalSapic` ❌ |
| 7 | Local SAPIC | `PlatformInterruptSource` ❌ |
| 8 | Platform Interrupt Sources | `LocalX2Apic` ❌ |
| 9 | Processor Local x2APIC | `IoX2Apic` ❌（规范里没有这个类型） |
| 10 | Local x2APIC NMI | `LocalX2ApicNmi` ✅（歪打正着） |
| 15 | GIC ITS | `GicExtendedRegister` ❌ |

根因是漏掉了 type 5 `Local APIC Address Override`，又凭空插入了一个
`IoX2Apic`，两个错误在 type 10 处相互抵消。目前只用到 0/1/2，所以还没爆；
一旦开始做 x2APIC 枚举就会静默取到错误类型的条目。

### C7. 多个 MADT entry 的结构体布局与规范不符
| 结构体 | 规范长度 | 当前 `sizeof` | 问题 |
|---|---|---|---|
| `InterruptOverrideEntry` | 10 | 9 | `flags` 应为 `u16`，写成了 `u8` |
| `NmiEntry` (type 3, NMI Source) | 8 | 6 | 布局完全不对：规范是 `flags(u16) + gsi(u32)`，当前写成了 Local APIC NMI 的样子 |
| `LocalX2ApicEntry` | 16 | 14 | `reserved` 应为 2 字节，`x2apic_id` 应为 `u32`（当前是 `u8`） |
| `LocalX2ApicNmiEntry` | 12 | 9 | 布局不对 |
| `GicCpuInterfaceEntry` | 80 | 10 | 整个 GIC 系列 (11–15) 都是凭空捏造的 |

`InterruptOverrideEntry` 目前"能用"纯属小端序的运气——极性/触发方式恰好在低字节。
所有这些结构体都**没有 `static_assert(sizeof(...) == N)`**，而 `srat.hpp` 里是有的。
这正是这类 bug 能活下来的原因。

### C8. `debug.cpp` 是空文件
`acpi/debug.hpp` 声明了 `get_debug_port()` 和 `parse_dbg2t()`，`BUILD.gn` 把
`debug.cpp` 列进了 `sources`，但文件 0 字节。任何调用方都会得到链接错误。
`DebugConfig::Type` 的枚举项叫 `PortIo`/`Mmio`，注释里却写的是 `Type::kMmio`/`Type::kPio`。

### C9. `kernel/platform/pc` 里三个文件引用的是已经不存在的 API
```
platform/pc/hpet.cpp:10       acpi::AcpiHpet                     → 现名 acpi::Hpet
platform/pc/interrupt.cpp:16  acpi::AcpiMadtInterruptOverrideEntry → 现名 madt::InterruptOverrideEntry
platform/pc/interrupt.cpp:65  acpi::enumerate_io_apic_isa_overrides → 现名 enumerate_interrupt_overrides
```
而且 `kernel/platform/pc/BUILD.gn` 根本没有依赖 `//kernel/lib/acpi:header`，
实际编译时先死在 `fatal error: 'acpi/apic.hpp' file not found`。
（注：整个 kernel proper 目前因为 `ustl/rc.hpp` 的问题也编不过，这是既有状态。）

### C10. `Signature` 通过 union 做类型双关
`header.hpp:11-35`：写 `char name[4]`、读 `u32 value`。GCC/Clang 作为扩展支持，
但在**常量表达式**里读非活跃成员是 ill-formed —— 所以那几个
`constexpr friend bool operator==` 实际上没法在 constexpr 上下文里用。
另外 `constexpr explicit Signature(char const name[4])` 收的是**指针**，
`Signature("XY")` 会越界读且不报错。

---

## 2. 设计问题

### D1. 每次查表都重新遍历 + 重新映射（README 自己也记了这条）
`get_table_by_signature()` 是 O(N) 全表扫描，每次迭代都调一次
`IPhysToVirt::phys_to_virt()`（而且 `map_structure` 内部要调两次）。
`numa.cpp` 里四个函数各自独立调用它 —— 光是 NUMA 初始化就要把整张表目录
扫 4 遍。加上 APIC 的 3 次，启动路径上一共 7 次全扫描。

### D2. 六份几乎一模一样的"查表 + BinaryReader 循环"
`apic.cpp` 的 `enumerate_entries<Entry>` 模板已经把 MADT 那三个收敛了，
但 `numa.cpp` 里三个 SRAT/SLIT 循环又把同样的骨架手抄了三遍——
连 `if (!reader.valid()) return Status::InternalError;` 都是逐字复制的。
这是典型的**浅模块**：接口（7 个自由函数 + 6 个 `ustl::function::Fn` typedef）
几乎和实现一样复杂。

### D3. 回调式枚举接口的三个硬伤
```cpp
using IoApicCommitFn = ustl::function::Fn<auto(madt::IoApicEntry const &) -> Status>;
auto enumerate_io_apics(IAcpiParser const &, IoApicCommitFn const &) -> Status;
```
1. **回调的返回值被丢弃**——`apic.cpp:36` 只写了 `visitor(*entry);`。
   声明说返回 `Status`，实现从不看，调用方无法中止枚举。
2. **类型擦除是白付的代价**——`ustl::function::Fn` 在这里没有任何多态需求，
   调用点全是就地 lambda。
3. **每种 entry 类型要新增一个函数 + 一个 typedef**，接口随类型数线性膨胀。

一个 `for (auto const &e : madt.entries<madt::IoApicEntry>())` 能同时解决这三条。

### D4. `IAcpiParser` 是个只有一个实现的假接缝
按 `codebase-design` 的说法：**一个 adapter 只是假想接缝，两个才是真接缝。**
- `IPhysToVirt`：两个真实 adapter（`kernel/phys` 恒等映射、kernel proper 的
  `PhysMap`）→ **真接缝，保留**。
- `IAcpiParser`：只有 `AcpiParser` 一个实现，代价是全局对象里多一张 vtable，
  换来的"可测试性"从来没被兑现过（这个库一个测试都没有）→ **假接缝**。

而且项目风格明确要求"审慎对待虚函数"（`modern-cpp-development` 第 2 条）。

### D5. `IPhysToVirt` 的契约没写清楚
```cpp
virtual auto phys_to_virt(PhysAddr addr, usize size) -> ktl::Result<void const *> = 0;
```
- 返回的指针活多久？谁负责 unmap？——没说。
- `size` 是"请保证这个范围连续可读"的请求，还是纯提示？
  `kernel/phys` 的实现**完全忽略** `size`（直接返回 `addr`），
  `platform/pc` 的实现也**完全忽略** `size`。
  于是 `map_structure` 里那两次调用带的长度检查在真实内核里是装饰品。

按 `codebase-design` 的定义，接口 = 调用方为了正确使用所必须知道的一切，
包括不变式、生命周期和错误模式。这里三样都缺。

### D6. `hpet.hpp` 把两个不相干的东西塞进一个头文件
前 20 行是 ACPI HPET 表（属于本库），后 200 行是 HPET 的 MMIO 寄存器块
（`HpetRegs` / `HpetTimerRegs`，属于 x86 定时器驱动）。后者还把
`<ours/assert.hpp>` 和 `<ustl/bitfields.hpp>` 拖进了本该纯数据的结构体头文件。
唯一的消费者是 `kernel/arch/x86/kernel/time/hpet.cpp`。

顺带一个 bug：`set_mode_oneshot()` 里写了 `DEBUG_ASSERT(support_periodic())`
（`hpet.hpp:78`）——单次模式不需要周期能力，复制粘贴漏改。

### D7. 零测试，而这个库是最适合做主机测试的那一类
纯数据解析、无硬件依赖、输入就是一段字节。仓库里已经有现成的模式：
`//system/lib/ustl:binary-reader-test` 之类的 `executable(...) { testonly = true }`
跑在 host toolchain 上（`BUILDCONFIG.gn:211` 会给 host 强制包含 prelude，
所以 `ours/status.hpp` 这些能直接用），并挂在 `//:default` 上。
用一个恒等 `PhysMapper` 在内存里合成一张 RSDP/XSDT/MADT/SRAT/SLIT，
上面 C1–C7 每一条都能被一个断言钉死。

### D8. 一些较小的问题
- `types.hpp` 重新定义了 `i8..usize`，而 prelude 已经在全局提供了；`VirtAddr`/`PhysAddr`
  也和 `ours/types.hpp` 重复。直接 `using ours::PhysAddr;` 即可。
- `parser.cpp` 里 `ustl::ok` / `ktl::err` / `ustl::err` 混用（同一个类型的两个拼法）。
- `get_table_by_signature()` 返回 `0` 而非 `nullptr`（`parser.cpp:172`）。
- 头文件保护宏还叫 `ACPI_DETAILS_HEADER_HPP` / `ACPI_DETAILS_SLIT_HPP`，
  是从旧的 `details/` 目录改名后残留的。
- `srat::Type` 里混进了一个 `AcpiSratFlagEnabled = 1`，和 `MemoryAffinity` 同值。
- `madt::Flag` 定义了却没人用，也没开 `USTL_ENABLE_ENUM_BITMASK`。
- `Rsdt`/`Xsdt` 用 `u32 addr32[0]`（GNU 零长数组扩展），`slit::Table` 用
  `u8 entry[]`（C99 柔性数组），同一个库里两种写法。
- `BUILD.gn`：`visibility = [ "*" ]` 是空操作；`source_set` 里的
  `include_dirs = [ "include" ]` 和 `cpp_interface` 导出的 public_config 重复；
  `hpet.hpp` 需要 `ours/assert.hpp`、`ustl/bitfields.hpp`，但 `header` 目标
  只声明了 `//kernel/lib/ktl:headers`。
- `from_rsdp()` 在 XSDT **映射失败**时直接返回错误，不回退 RSDT；
  但在 XSDT **校验失败**时会回退。两条路径应该一致。
- 命名空间缩进在文件之间不一致（`.clang-format` 是 `NamespaceIndentation: Inner`）。
