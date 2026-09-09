
## 为 vmalloc 划出内核地址空间窗口（2026-09-08）

**文件**
- `kernel/arch/x86/kernel/include/ours/arch/aspace_layout.hpp` — 新增 `ARCH_VMALLOC_VIRT_BASE` / `ARCH_VMALLOC_SIZE`
- `kernel/mm/init_vmm.cpp` — `PHYS_VMOS[]` 增加 `k:vmalloc` 一条

**症状**
无，这是新增。`init_vmm.cpp:175` 那个空的 `init_heap()` 注释里写着它要装的是
"the VMM's own allocator: virtually contiguous, physically scattered"，本次先把它要用的
地址空间划出来。

**原因**
vmalloc 的返回地址必须落在一段**固定**范围里，这不是审美选择，是 `operator delete` 能
成立的前提：`ktl::kfree()` 靠 `virt_to_folio()` 经 physmap 回查所属 cache
（`ktl/kmalloc.cpp:80`），对窗口外的地址无意义；而 `KERNEL_DEBUG_LEVEL=0` 的镜像里
`PhysMap::virt_to_phys()` 对非 physmap 地址返回 0，`phys_to_frame(0)` 拿到 0 号帧，
`try_as<SlabBody>` 的断言又被编译掉——于是**在 release 镜像里把一个非 physmap 指针交给
kfree 是未定义行为，而不是一次断言**。所以 delete 必须先做范围判断，而范围判断要快到
可以放在每一次 delete 上，那就只能是一次减法，也就只能是固定窗口。Linux 的
`VMALLOC_START`/`VMALLOC_END` 与 `is_vmalloc_addr()` 是同一个道理。

**位置为什么在镜像之上**：这一条是约束而非偏好。cpulocal 窗口结束于
`KERNEL_ASPACE_BASE + 32 GiB + 64 MiB`，而镜像在 `KERNEL_LOAD_BASE`（base + 33 GiB）
再加最多 `GB(1)` 的 KASLR 偏移，中间的空隙不足 1 GiB。窗口若放在那里，要么小得没用，
要么盖住镜像。放 `base + GB(64)` 则与偏移上限无关，是个不依赖镜像实际落点的常数；
`KERNEL_ASPACE_SIZE = GB(512)`，到 base+68GB 远未触顶。

那段空隙保持原样也是有意的：`VmPlacement::anywhere()` 是**最低地址 first-fit**
（`vm_area_or_mapping.cpp:180`），local APIC、HPET、IDT、GDT 和每一个内核栈现在都落在
那里，不该被一个新窗口挤走。

**改法**

```c
#define ARCH_VMALLOC_VIRT_BASE (KERNEL_ASPACE_BASE + GB(64))
#define ARCH_VMALLOC_SIZE GB(4)
```

```cpp
{.name = "k:vmalloc",
 .base = VirtAddr(ARCH_VMALLOC_VIRT_BASE),
 .size = usize(ARCH_VMALLOC_SIZE),
 .vmaf = VmaFlags::Read | VmaFlags::Write,
 .backing = PhysVma::Backing::AddressSpace},
```

`Backing::AddressSpace` 而不是 `Mapped`：`entrol_preset_vma()` 对前者**只
`create_subvma()` 就返回，不调 `reserve()`**（`init_vmm.cpp:143`）。这是必须的——窗口
在任一时刻绝大部分都没有映射，`reserve()` 要去改写并不存在的页表项。和上面的
`k:cpulocal` 是同一个约定：VMM 只保证这段地址不会被发给别人，页表归子系统自己管。

一次性把 4 GiB 全部声明，而不是按需增长：这条 VMA 的作用就是拦住
`VmPlacement::anywhere()`。窗口里若混进了别人的映射，它会被
`is_vmalloc_addr()` 认成 vmalloc 的地址，然后在 `delete` 时被当作 vmalloc 分配释放掉。

**参照**
- `Linux VMALLOC_START` / `VMALLOC_END`（x86 在 `pgtable_64_types.h`）：同样是固定窗口，
  同样服务于 `is_vmalloc_addr()`。注意 Linux 的 `VMALLOC_END` 是**闭区间**上界而
  `is_vmalloc_addr` 用 `<` 比较；本树统一用半开区间 `[base, base + size)`。
- 本树 `k:cpulocal`（`init_vmm.cpp:122`）：同样的 `Backing::AddressSpace` 语义。

**验证**
`ninja` 通过；QEMU 实测 `kernel.dev.dbg_1.obi`，启动日志新增一行且范围与预期一致：

```
Reserve area k:cpulocal: [FFFFFF8800000000, FFFFFF8804000000)
Reserve area k:vmalloc:  [FFFFFF9000000000, FFFFFF9100000000)
```

与镜像（`FFFFFF8840000000` 起）不重叠。userboot 照常进入 ring 3 并打出
`hello from ring 3`，30 秒内 910 次用户态时钟中断，无回归。

**故意没做**
- **`init_heap()` 仍然是空的**。本次只划地址，分配器本身是下一步。
- **没有 guard page 机制**。整棵树都没有（`vm_area_or_mapping.hpp:284` 的相邻区间间隙为
  零），vmalloc 的 guard 由它自己在窗口内留页实现，不动 VMM。
- **riscv / arm 没有对应宏**。等那两个架构真的有启动路径时再加。

## vmalloc 的地址分配器（2026-09-08）

**文件**
- `kernel/mm/include/ours/mm/vmalloc_arena.hpp` / `vmalloc_arena.cpp` — 新增 `VmallocArena`
- `kernel/mm/tests/vmalloc_arena_smoke.cpp` — 新增宿主单测
- `kernel/mm/BUILD.gn`、`kernel/mm/tests/BUILD.gn`、`BUILD.gn` — 登记

**症状**
无，新增。上一条划出了 `k:vmalloc` 窗口，这一条管"窗口里哪些页还空着"。

**原因**
把地址簿记单独切出来，是为了**它能在宿主上测**。vmalloc 的另一半（分配物理页、装页表、
加锁）没有一样能在宿主跑，而地址簿记是整件事里唯一"错了不崩、只是慢慢漏"的部分——
合并少做一次不会立刻表现为故障，只会让窗口一点点碎掉，几百次分配之后才耗尽。这种缺陷
只有单测抓得住，所以它必须是一个不依赖内核任何全局状态的类。

**数据结构选型**。三个候选：

| 方案 | 元数据 | 否掉的理由 |
|---|---|---|
| 位图（窗口每页一位） | 4 GiB 窗口 = 128 KiB `.bss` | 内核现在整个 `k:bss` 才 168 KiB，翻倍换一个查询频率极低的结构不值 |
| 红黑树（Linux 的 `vmap_area` + 增强子树最大空隙） | 每个区间一个节点，要动态分配 | Linux 那棵树是为几十万个区间准备的；这里是个位数。而且节点要从 kmalloc 出，等于在分配器里嵌一次分配 |
| **有序空洞数组**（选中） | `256 × 16 B` = 一页 | — |

数组还有一个位图和红黑树都没有的好处：**分配器内部不分配**。没有嵌套分配，就没有
"vmalloc 调 kmalloc 调 alloc_frames 又回到 vmalloc"这类递归的可能，也没有"分配元数据
时 OOM"这条要回滚的失败路径。

**为什么记录的是空洞而不是已分配区间**：两者并不对称。一个空洞就是一段地址，除此之外
没有任何内容；而一个已分配区间还拥有背后的物理页，并且要能**从区间内部的指针**被找到。
后者是 `mm::vmalloc()` 的事，不是这一层的。

**`MAX_HOLES` 的界怎么来的**：空洞和已分配区间沿窗口交替排列——两个空洞之间必然隔着
至少一个已分配区间，否则它们相邻、就该被合并掉。所以恒有 `holes <= live + 1`。于是把
**live 卡在 `MAX_HOLES - 1`** 就够了，数组永远塞得下。这个界必须在**分配**这一侧执行，
不能在释放侧：一次失败的释放会漏掉地址空间而且无处上报，比一次失败的分配严重得多。

**改法（节选）**

```cpp
auto VmallocArena::allocate(usize const nr_pages) -> VirtAddr {
    ...
    if (m_nr_live + 1 >= MAX_HOLES) {
        return 0;
    }
    for (auto index = usize{0}; index < m_nr_holes; ++index) {
        auto &hole = m_holes[index];
        if (hole.nr_pages < nr_pages) { continue; }
        auto const base = hole.base;
        if (hole.nr_pages == nr_pages) {
            erase_hole_at(index);
        } else {
            // 从头部切，这样数组不用重排：空洞的 base 上升，而它本来就在前驱之上。
            hole.base += pages_to_bytes(nr_pages);
            hole.nr_pages -= nr_pages;
        }
        ++m_nr_live;
        validate();
        return base;
    }
    return 0;
}
```

`allocate` 失败返回 **0 而不是 Status**：零在这里永远不是合法答案（窗口在内核半区），
而每个调用方紧接着都要检查后续物理分配的结果，多一个 Status 只会被解包后丢掉。

释放时的三种合并（左、右、两侧）都写开了，两侧合并要把三个空洞收成一个。越界、未对齐、
与现有空洞重叠都在 `free()` 里挡掉——重叠意味着这段地址从未发出去过、或者被还了两次，
两者这一层都分辨不了，但都是调用方的 bug，继续下去会把一个活着的区间并进空闲表再发给
别人。

**参照**
`Linux mm/vmalloc.c` 的空闲侧：地址序红黑树 + 每子树最大空隙的增强、
`alloc_vmap_area()` 取最低地址 first-fit（`find_vmap_lowest_match()`）、
`merge_or_add_vmap_area()` 归还时左右合并。本实现保留 first-fit 与合并语义，换掉容器。
选 first-fit 而不是 best-fit 的理由也和 Linux 一样：紧凑不是一个几乎全空的窗口的问题，
而 first-fit 让释放的区间留在低地址，下一次分配正好与它合并。

**验证**
- 宿主单测 8 组全过（`out/bringup.x86/host/obj/kernel/mm/tests/vmalloc-arena-smoke`）：
  窗口参数校验（含**会回绕的窗口**）、从头部切、拒绝放不下的请求、左合并 / 右合并 /
  两侧合并、释放参数校验、**100 轮分配-释放循环后窗口必须完好如初**、以及把 live 顶到
  上限后隔一个释放一个（空洞数最坏的模式）仍不溢出数组。
- 写测试时我把两处期望写错了（以为孤立释放会并进尾部空洞；以为 live 上限是
  `MAX_HOLES - 2`），是测试跑出来纠正的——这正是要它的原因。
- `ninja` 通过，`clang-tidy` 对本文件无告警；QEMU 实测 `kernel.dev.dbg_1.obi` 无回归
  （913 次用户态时钟中断）。

**故意没做**
- **还没有人调用它**。`init_heap()` 仍是空的，`mm::vmalloc()` 是下一步。
- **没有对齐参数**。所有返回都是页对齐，vmalloc 的调用方没有更严的需求；真需要时是给
  `allocate()` 加一个 align 形参，而不是换容器。
- **`lower_bound` 是线性扫描**。256 项一趟顺序扫描比会误预测的二分快，代码里写了这条
  理由和"界若涨一个数量级就回来改这里"。

## `~VmCowPages` 不再把自己的页丢掉（2026-09-08）

**文件**
- `kernel/mm/vm_cow_pages.cpp` — 析构函数从 `= default` 变成真的归还
- `kernel/mm/include/ours/mm/vm_page_map.hpp` — 新增 `drain_owned()`

**症状**
潜伏至今，因为没有 VMO 被销毁过。**每一个 VMO 死亡时，它拥有的所有物理页都不归还
PMM。**

```cpp
VmCowPages::~VmCowPages() = default;
```

`VmPageMap` 只释放它自己的树节点，`decommit()` 是 `Unimplemented`，整条 VMO 路径上唯一
的 `free_frames` 是插入失败时那一次（`vm_cow_pages.cpp:95`）。
`docs/repairs/260903/mm.md:18` 记过这一条。

**原因**
内核栈和 userboot 的镜像都是"建了就不销毁"，所以这个洞一直没被走到。但它挡住了任何
建在 VMO 之上的分配器——`heap::vfree()` 会拆掉映射、放掉 VMO，然后什么也没发生。

`VmPageMap` 没有遍历或删除接口，只有 `get_page`/`insert_page`；底层的 `BTreeMap` 有
`begin()`/`end()`/`clear()`，所以缺的只是一个把它们接起来的口子。

**改法**

```cpp
template <typename Visitor>
auto drain_owned(Visitor &&visitor) -> void {
    for (auto &entry: m_slots) {
        auto const slot = entry.second;
        if (slot_tag(slot) == Tag::Owned) {
            visitor(slot_page(slot));
        }
    }
    m_slots.clear();
}
```

```cpp
VmCowPages::~VmCowPages() {
    m_pages.drain_owned([](VmPage *const page) { free_frames(page, 0); });
}
```

**只归还 `Owned` 的**：`Borrowed` 槽位是从别处借来的页，归还是出借方的事。无论如何都
清空整张表——页已经交出去了，一张还描述着它们的表是唯一不诚实的状态。

访问器接收页而不是让 `VmPageMap` 自己调分配器，是为了让这个头文件保持"容器"的身份，
不必为了描述如何拆解而 include 页分配器。

**参照**
`Zircon VmCowPages::~VmCowPages()`：同样调 `page_list_.RemoveAllPages()` 并成批释放；
`VmPageList::RemoveAllPages()` 同样是把每页交给回调后清空。

**验证**
`ninja` 通过；QEMU 实测两个镜像无回归。

直接证据来自把它临时改回 `= default` 的对照实验：让 `heap::vmalloc` 的自检 churn 半个
GiB（8192 轮 × 64 页），修好的内核跑完；恢复泄漏之后，同样的循环跑到约 1975 轮（约
494 MiB 已泄漏、512 MiB 的客机见底）时仍在继续，直到 QEMU 被杀。

那次对照实验还顺带查出**另一件更要紧的事**：内存耗尽时 `alloc_frames` 不返回空指针而是
空转（`alloc_frames_slowpath` 对 `GAF_KERNEL` 无限重试，而 reclaim/compact 是桩）。
也就是说真正的 OOM 表现为挂死，而不是任何一处 `if (!p)` 走得到。已记入
`docs/repairs/260908/heap.md` 第 3 条。

**故意没做**
- **`decommit()` 仍是 `Unimplemented`**。析构时归还和按范围归还是两件事，后者没有调用者。
- **没有成批释放**。Zircon 攒一批再还；这里逐页调 `free_frames`。VMO 还小，先要正确。
