# kernel/phys/lib/bootmem 改动记录（2026-09-26）

## 保留区带类型，并从 physboot 传到内核（2026-09-26）

**文件**
- `kernel/phys/lib/bootmem/include/bootmem/types.hpp` — 新增 `MemoryType` 和 `to_string()`；新增 `Reservation`；`MemRange` 的 `bool used` 换成 `MemoryType type` 加 `used()`；`AllocationRequest` 加 `type`；`PhysicalMemoryStorage::reservations` 变成 `Span<Reservation>`
- `kernel/phys/lib/bootmem/include/bootmem/physical_memory.hpp` — `reserve()` 收类型、只合并同类型、按 base 有序插入；`allocate()` 把 `request.type` 传下去；`release()` 保留切剩部分的类型；`snapshot()` 输出类型；删掉不再有调用者的 `replace_reservation()`
- `kernel/phys/memory.cpp`、`kernel/arch/x86/phys/memory.cpp`、`kernel/phys/include/ours/phys/boot_package_input.hpp`、`kernel/phys/lib/multiboot/include/multiboot/memory_map.hpp`、`kernel/phys/aspace.cpp`、`kernel/phys/detail/boot_alloc.hpp`、`kernel/phys/module_loader.cpp`、`kernel/phys/physboot.cpp`、`kernel/arch/x86/phys/handoff.cpp` — phys 侧每个保留点报出自己的类型
- `kernel/mm/include/ours/mm/early_mem.hpp` — `reserve()`/`allocate()` 收类型，且**不给默认值**
- `kernel/mm/pm_node.cpp`、`kernel/mm/sparse_memmap.cpp`、`kernel/mm/cpu_local.cpp`、`kernel/arch/x86/kernel/bootstrap.cpp`、`kernel/mm/pmm.cpp` — 内核侧每个早期分配报出自己的类型
- `kernel/mm/init_pmm.cpp` — `handoff_early_pmm()` 按 `entry.type` 重建保留区
- `kernel/mm/early_mem.cpp` — `retire_bootmem()` 改成逐段保留为 `PageAllocator`
- 两个 smoke test 跟着改

**症状**
潜伏，而且是一整类问题的根，不是单个 bug。内核拿到手的内存图里，一段保留内存长什么样都一样：
physload 的镜像、一张内核正要走的页表、固件声明的空洞、一个没人再用的临时缓冲——四者是同一个事实。
`bootmem::Reservation` 原来就是个裸 `Range`，`MemRange` 只有一个 `bool used`。

直接后果是 `mm::init_memmap_reserved()` 只能把这些帧一律打成 reserved，谁也说不出它们装着什么。
本轮之前修掉的 NUMA 启动缺页（`docs/changes/260925/mm.md`）就是这个缺失的一个表现：
一页还在被硬件走的页表和一段空闲内存在 bootmem 眼里没有区别，于是被发了第二次。

**原因**
交接是单向的、一次性的：physboot 做完 `snapshot()` 就没了，内核之后再想问「这段保留内存是干什么的」
没有对象可问。**信息必须跟着范围一起过河**，否则就永久丢失。原来过河的只有「被占了」这一个比特。

Zircon 的 `memalloc::Type`（`kernel/phys/lib/memalloc/include/lib/memalloc/range.h`）就是干这件事的，
三十多个取值：`kKernelPageTables`、`kDataZbi`、`kVdso`、`kUserboot`、`kBootMachineStack`、
`kPhysScratch`…… 内核侧 `PmmNode::InitReservedRange()` 按类型分流：

```cpp
if (range.type == memalloc::Type::kKernelPageTables) {
    ArchVmAspace::HandoffPageTablesFromPhysboot(&reserved);
    return;
}
for (auto& p : reserved) {
    p.set_state(vm_page_state::WIRED);
}
```

ours 之前没有这一层，所以连「把这批帧标成 MMU」都无从下手——不知道哪些是。

**改法**
一个枚举而不是「一个 bool 加一个用途」：两个字段会互相矛盾，而 `Free` 作为枚举的一个取值之后，
一段内存不可能既是空闲的又是页表。零值故意是 `Unknown` 而不是 `Free`——没被写过的字节读出来是
「保留、用途未记录」，这是错也要错在安全那一侧。

改动前：

```cpp
struct MemRange final {
    Range range{};
    bool used{};
};

struct PhysicalMemoryStorage final {
    ustl::views::Span<MemoryRegion> usable{};
    ustl::views::Span<Range> reservations{};
};
```

改动后：

```cpp
enum class MemoryType : u8 {
    Unknown, Free, FirmwareReserved, Peripheral, PhysImage, BootPackage,
    KernelImage, PageTable, Handoff, KernelData, Transient, PageAllocator,
};

struct Reservation final {
    Range range{};
    MemoryType type{MemoryType::Unknown};
};

struct MemRange final {
    Range range{};
    MemoryType type{MemoryType::Unknown};

    [[nodiscard]]
    constexpr auto used() const -> bool {
        return type != MemoryType::Free;
    }
};
```

每个取值都有真实的保留点，没有预留的空项：`PhysImage`（physload 自己和它加载的模块）、
`BootPackage`（OBI 容器）、`KernelImage`（physboot 放下的内核镜像）、`PageTable`
（phys 建的、内核继续走的那些表）、`Handoff`（交接结构和跳转蹦床）、
`FirmwareReserved` / `Peripheral`（固件说的非 RAM 和 MMIO）、`KernelData`
（节点/zone 描述符、帧 memmap，终生保留）、`Transient`（早期占用、之后还给 buddy：
boot CPU 的临时 per-CPU unit、AP 蹦床缓冲）、`PageAllocator`（交接时给了 buddy 的）。

**`reserve()` 只合并同类型**，因为合并是在声称「这两段可以互换」，而不同用途的两段不可互换——
把一张页表和紧邻的 phys 镜像并成一条，正好把这次加类型要留住的东西抹掉。代价是保留表条目变多，
上界从「一条一段连续保留」变成「一条一段同类型连续保留」。

**不同类型的重叠直接回 `AlreadyExists`**，不静默改写：那意味着两个子系统都认为自己拥有这段，
是调用者的 bug；悄悄挑一个赢家正是信息丢失的方式。

**踩到的坑（重要）**：第一版 `reserve()` 沿用了原来「先定位插入点，再向后吞并」的写法，
遇到不同类型就 `break`。这会破坏保留表的**有序性**：已有 `[0x1000,0x2000)` Peripheral，
再 `reserve([0x2000,0x3000), PageTable)`，定位到下标 0（它的 `end()` 不小于新 base），
吞并循环第一眼就是不同类型且不重叠 → break → `erase_count == 0` → 把新条目插在下标 0，
于是表变成 `[0x2000..) , [0x1000..)`。而 `find_free()`、`snapshot()`、`visit()` 全都假定升序并在
「`reserved.base >= end`」处 `break`——顺序错了不是变慢，是给出错误答案。实测表现是内核在
`init_platform_early()` 里静默挂死，连 panic 都没打出来。

所以改成三步：先把同类型的邻居吞并成 `merged`（循环到不再增长，和 `add_usable()` 同一个形状，
因为吞掉一个可能让下一个进入射程）；再检查有没有不同类型与 `merged` 重叠；最后按 `merged.base`
插到保持有序的位置。插入点必须由 `merged` 算，不能由入参 `range` 算——吞并可能把 base 往下移了。
`replace_reservation()` 没有调用者了，删掉。

**`retire_bootmem()` 跟着改**。它上一条改动里是「一条保留盖住 `[min_address, max_address)`」，
在有类型之后这条会与 phys 镜像、页表、固件空洞全部重叠，被新的 `reserve()` 正确地拒绝。
改成逐段：反复取 free 视图的第一段并把它保留成 `PageAllocator`，直到没有。不能边走边保留——
保留会改变遍历器还能看到什么。类型选 `PageAllocator` 而不是 `KernelData`，是因为这批帧的事实就是
「buddy 拥有了」，而这正是内核在河对岸要读的东西。

**`EarlyMem::allocate()` 的类型参数不给默认值**，四个重载都要显式传。默认值会变成「没人选过的那个值」，
而加类型的全部意义就是河对岸问不到——一个不说自己是什么的分配点，就是一段永远无法再被描述的内存。

**参照**
`memalloc::Type` 与 `PmmNode::InitReservedRange()`，见上。一处刻意的不同：Zircon 的
`InitReservedRange()` 是**让 PMM 先拥有全部 RAM，再把保留区从 PMM 里分配出来**
（`pmm_alloc_range()`），所以它根本没有「交接」这一步，也就没有两个主人的可能。ours 目前还是
bootmem 先持有、交接给 buddy 的形状，`PageAllocator` 这个类型是在现有形状里把交接说清楚；
要不要改成 Zircon 那个顺序是后面的事，本条没有动它。

**验证**
- `out/bringup.x86`、`out/numa.x86` 两个配置全量构建通过。
- `out/numa.x86`（`NUMA=1`，8 CPU，四节点）：7 个 AP 全部 online，shell 六组检查全过
  （channel / handle transfer / handle quota / bad-pointer / vmo_read / libc），进程正常销毁，
  无 fault / panic / 断言失败。`Released 16 regions, 255666 frames (998 MiB)`。
- `out/bringup.x86`：六组检查全过，正常销毁，无回归。
- 行为应当完全不变（类型只是被记录和搬运，还没有人按它分支），两个镜像的日志与上一条一致。
- `kernel/phys/lib/bootmem/test/physical_memory_smoke.cpp` 和
  `kernel/phys/lib/multiboot/test/memory_map_smoke.cpp` 跟着改了。前者在本 checkout 里
  **单独编译不过**（`error: "No thread API"`、`ours/types.hpp` 里一片 unknown type），
  是它自己的编译参数问题，与本改动无关，而且它不在任何配置的默认目标里；后者在默认构建里，通过。

**故意没做**
- **没有让任何人按类型分支**。内核这一步只是把类型收下并存进自己的 bootmem，
  `init_memmap` 仍然一律 `mark_reserved()`。按类型给帧打 `Role`（`PageTable` → MMU，
  `Transient` → 可以还给 buddy，`KernelData` → 永久占用）是下一条，单独验证。
- 没有把 `Unknown` 做成编译错误。现在已经没有产生 `Unknown` 的调用点，但它得留着，
  因为零初始化的字节必须读出「保留」而不是「空闲」。
- 没有给 `PhysicalMemory` 加按类型查询的接口。等第一个真正要按类型走的消费者出现再加，
  否则形状是猜的。
- 没有动 bootmem 先持有、再交给 buddy 的整体顺序（见「参照」）。

## `MemoryType::PageAllocator` 改名 `Mmu`（2026-09-26）

**文件**
- `kernel/phys/lib/bootmem/include/bootmem/types.hpp` — 枚举项和 `to_string()`
- `kernel/mm/early_mem.cpp` — `retire_bootmem()` 的调用点和注释

**症状**
无，纯改名。

**原因**
这个取值说的是「交接之后这段内存归内存管理器了」。原来叫 `PageAllocator`，是从
「谁会把它发出去」的角度命名的——buddy。改叫 `Mmu`，是从「归谁」的角度命名的——mm 子系统。
用户选了后者。

拼写用 `Mmu` 而不是 `MMU`，跟这个仓库里缩写的既有写法一致：`PageFrame::Role` 是
`None, Io, Lru, Vmm, Pmm, Slab, Heap, Folio`，缩写一律首字母大写其余小写。

**改法**
一处枚举项、一处 `to_string()`、一处调用点、两处注释。`MemoryType::PageTable`
没有动——那是 phys 建的、内核继续走的页表，和这一项不是一回事。

**参照**
Zircon 的 `vm_page_state::MMU` 专指页表页，语义上对应的是这里的 `PageTable` 而不是 `Mmu`。
本项在 Zircon 里没有对应物，因为它的 PMM 一开始就拥有全部 RAM、保留区是从 PMM 里分配出来的，
没有「交接」这个状态需要命名。

**验证**
- 两个配置构建通过。
- `out/numa.x86`：7 个 AP 全部 online，shell 六组检查全过，无 fault / panic / 断言失败。
- `out/bringup.x86`：构建通过，无回归。

**故意没做**
- 没有改 `PageTable`。下一条要给帧打的 `Role` 也不会因为这次改名而改叫别的。

## 按保留类型给页帧打 `Role` 和 `Reserved`，并让 `reserved_frames` 第一次有人写（2026-09-26）

**文件**
- `kernel/phys/lib/bootmem/include/bootmem/physical_memory.hpp` — 新增 `visit_reservations()`
- `kernel/phys/lib/bootmem/include/bootmem/types.hpp` — `MemoryType` 加 `Count`
- `kernel/mm/include/ours/mm/early_mem.hpp` — `visit_reservations()` 门面
- `kernel/mm/include/ours/mm/pm_frame.hpp` — `page::Role` 新增 `Mmu`
- `kernel/mm/include/ours/mm/pm_zone.hpp`、`pm_node.hpp` — 新增 `add_reserved_frames()`
- `kernel/mm/memmap_init.cpp` — 新增 `stamp_reserved_frames()`，`init_memmap()` 末尾调用；按类型汇总并打印

**症状**
潜伏。帧描述符说不出自己装着什么：`init_memmap_range()` 给每个可用帧
`Role::None`，`init_memmap_reserved()` 只给**空洞**打 `Flags::Reserved`——落在
bootmem 保留区里的帧两样都没有。一页正被硬件走的页表和一页空闲内存，在描述符层面完全一样，
唯一的区别是 buddy 没收到它。

另一处：`PmZone::m_reserved_frames` 从来没有写者。头文件里写着
`managed_frames_ = present_frames_ - reserved_frames_`，`PmNode::dump()` 也在打印它，
但每个 zone 不论丢了多少早期内存都报 0。

**原因**
上一条给保留区加了类型，但内核只是把类型收下存进自己的 bootmem，没有人读。这一条是读的那一半。

读不到的原因不只是没写代码：`PhysicalMemory` 原来根本没有能读出类型的出口。`visit()` 的两个视图
（`Usable`/`Free`）回答的是「哪些地址」，访问器看到的 `MemoryRegion` 有 node 没有 type。
想要类型的调用者要的是保留区本身，所以新增 `visit_reservations()` 直接交出 `Reservation`，
而不是再加一个 `MemoryView`。

`Role` 新增 `Mmu` 而不是复用现有项：`Vmm` 是虚存管理器持有的页，`Pmm` 在 buddy 的语义里表示
「正挂在空闲链上」（`FrameBuddy::is_mergeable()` 第一条就是
`buddy->role() != Role::Pmm → false`），都不是页表。Zircon 同样给页表页一个专属状态
`vm_page_state::MMU`。`Role` 的位宽是 `bit_width(Role::Count)`，从 8 变 9 仍是 4 位，不溢出。

**改法**
`init_memmap()` 末尾加一遍 `stamp_reserved_frames()`：遍历保留区，按类型给帧打标。

```cpp
[[nodiscard]]
auto role_of(bootmem::MemoryType const type) -> page::Role {
    return type == bootmem::MemoryType::PageTable ? page::Role::Mmu : page::Role::None;
}
```

只有 `PageTable` 在 mm 的词汇里有主人——建它的那个 aspace 在走它；其余
（镜像、blob、描述符数组）是内核或启动链单纯持有的内存，mm 侧没有可命名的所有者，
所以保持 `Role::None`，只用 `Reserved` 标志描述。页表额外 `mark_pinned()`：
只要还有根指向它就不能移动、不能回收，这正是 Pinned 的意思。

**跑在这个位置，是因为只有这个窗口两个条件同时成立**：帧描述符已经建好可以打标，
而且所有以后会有意义的保留区都已经在账上了——phys 链那边的（`handoff_early_pmm()` 原样重放）
加上内核自己的（node/zone 描述符、memmap 本身、boot CPU 的临时 per-CPU unit、AP 蹦床）。
唯一更晚出现的类型是 `Mmu`，那是 `do_handoff()` 交给 buddy 的那批，它们的 role 由 buddy 给。

**`Transient` 故意跳过**：那些帧用完要还给 buddy（`CpuLocal::init()` 释放临时 unit，
`free_bootstrap16_buffer()` 释放蹦床），算成保留会在它们回来的那一刻让账目变错——
而且还回去的路上没有人清 `Flags::Reserved`。

顺带给 `PmZone`/`PmNode` 的 `m_reserved_frames` 加上第一个写者，并在最后按类型打一行总数。
这一行是让整件事**可验证**的东西，和 `free_unused_memory()` 的「Released N regions」并列、
出于同一个理由：没人能读的总数就是没人能发现它错了的总数。

**验证**
- 两个配置构建通过。
- `out/numa.x86`（`NUMA=1`，8 CPU，四节点）：7 个 AP 全 online，shell 六组检查全过，
  无 fault / panic / 断言失败。
- `out/bringup.x86`：六组检查全过，无回归。新增的启动输出：

```
[info] Reserved 246 frames (984 KiB) of phys image
[info] Reserved 191 frames (764 KiB) of boot package
[info] Reserved 229 frames (916 KiB) of kernel image
[info] Reserved 12 frames (48 KiB) of page table
[info] Reserved 2052 frames (8208 KiB) of kernel data
```

两处能和 phys 侧的日志对上，是这次最有力的交叉验证：

- **kernel image 229 帧** 对上 physboot 打的 `Kernel image at 0x..., 229 pages`
  （numa 那次是 234 对 234）。
- **page table 12 帧** 对上 phys 日志里 12 行 `Allocate page table`
  （`0x3FEBB000`、`3FEBA`、`3FEB9`、`3FEB8`、`3FEB7`、`3FEB6`、`3FEB5`、`3FEB4`、`3FEB3`、
  `3FEA0`、`3FE9F`、`3FE9E`）。一个不多一个不少。

**故意没做 / 顺带发现**
- `handoff` 一帧都没打上标，查了一遍：交接快照缓冲区是按 `alignof(bootmem::MemRange)`（8 字节）
  分配的，所以它的保留区是**跨页的一小段尾巴**
  （`[0x3FE9DBE0, 0x3FE9E000) handoff`），而它落在的那一页没有被 `init_memmap()` 初始化过
  （`pfn_valid()` 为真但没有 `Flags::Initialized`），于是被新加的守卫正确跳过。
  帧是下游一切东西的单位，所以一段要活到内核里的保留区应该是**整页粒度**的。
  这是上游的问题，单独一条改。
- 没有让任何人**读** `Role::Mmu`。它现在是描述，不是判据。第一个判据大概是页表回收或者
  `free_page_table()`，等那件事真要做的时候再接。
- 没有动 `m_managed_frames`。头文件说它应该等于 `present - reserved`，现在
  `reserved` 终于有值了，这个恒等式可以开始校验——但 `managed` 目前是在
  `PmZone::free_frame()`/`alloc_frame()` 里增减的运行期计数，把两者对齐是另一件事。
