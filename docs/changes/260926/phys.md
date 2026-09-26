# kernel/phys 改动记录（2026-09-26）

## 交接快照缓冲区按整页分配，它的保留区才描述得出来（2026-09-26）

**文件**
- `kernel/phys/physboot.cpp` — `snapshot_memory_map()` 的分配从 `alignof(bootmem::MemRange)`
  改成 `PAGE_SIZE`，尺寸向上取整到整页

**症状**
潜伏，但立刻可见：给保留区加类型、再按类型给帧打标之后（见
`docs/changes/260926/bootmem.md`），启动输出里每一类保留内存都有一行总数，唯独 `handoff`
一帧都没有——而 physboot 明明为交接快照分配并保留了一段内存。

查出来的形态是这个保留区在内核这边长成了**跨页的一小段尾巴**：

```
DBG handoff [0x3FE9DBE0, 0x3FE9E000) handoff
DBG   pfn 0x3FE9D uninitialized
```

那一页既不整个空闲也不整个属于交接数据，而且 `init_memmap()` 的主遍历根本没初始化它
（`pfn_valid()` 为真，但没有 `Flags::Initialized`），于是被打标那一遍的守卫正确地跳过了。

**原因**
`snapshot_memory_map()` 按 `alignof(bootmem::MemRange)`（8 字节）分配这块缓冲区。
对 phys 自己来说这是对的——它只要一块对齐的内存放数组。

但这块缓冲区**活过了跳转**：内核的 `mh.ranges` 是一个指进它的 span，
`handoff_early_pmm()` 从里面读整张内存图。而交接之后，下游一切东西的单位都是**帧**：
memmap 一个帧一个描述符，buddy 一个帧一个链表项，`Role` 和 `Flags` 都长在帧上。
一段要活到内核里的保留区如果不是整页的，就没有任何帧描述符能表达它——
这一页到底算空闲还是算交接数据，两个答案都不对。

这不是「打标那一遍有 bug」，而是打标把一个一直存在的错位照出来了：
phys 侧按自己的对齐要求分配，内核侧按帧管理，中间那段尾巴谁都说不清。

**改法**
按 `PAGE_SIZE` 对齐、尺寸向上取整到整页。`max_snapshot_entries(1)` 的估算不受影响——
多要的字节不会多出条目。

没有改成「内核侧把不整页的保留区向外扩到整页」：那会把一页空闲内存悄悄划给交接数据，
即把一个上游的对齐问题在下游用更大的谎言盖住。整页是分配方的责任。

**参照**
Zircon 的 `PhysVmo` / `PhysMapping`（`kernel/phys/include/phys/handoff.h`）全部以页为单位
（`PmmNode::InitReservedRange()` 开头就是
`DEBUG_ASSERT(IsPageRounded(range.addr)); DEBUG_ASSERT(IsPageRounded(range.size));`），
正是同一条约束：能跨交接边界的东西必须整页，因为对岸只认帧。

**验证**
- `out/bringup.x86`、`out/numa.x86` 两个配置构建通过。
- 两个镜像的启动输出里 `handoff` 出现了，一帧：

```
[info] Reserved 12 frames (48 KiB) of page table
[info] Reserved 1 frames (4 KiB) of handoff
[info] Reserved 2052 frames (8208 KiB) of kernel data
```

- `out/numa.x86`：7 个 AP 全 online，shell 六组检查全过；`out/bringup.x86` 六组全过。
  两者都无 fault / panic / 断言失败。

**故意没做**
- 没有管 `arch_handoff_to_kernel()` 里那个跳转蹦床页。它是在快照**之后**才分配的，
  所以根本没进内核拿到的内存图，内核会把它当空闲内存收下。这没有错：蹦床只在跳转那一刻执行，
  跳完就是死的，还给 buddy 正是该做的。但值得记一笔，因为「快照之后再分配的东西内核看不见」
  是一条容易被下一个人踩到的规则。
- 没有给 bootmem 加「保留区必须整页」的断言。`reserve()` 是按字节的，而 phys 内部有合理的
  非整页用法（它自己不按帧管理）。真正的约束是「要跨交接边界的保留区必须整页」，
  这条应该加在 `snapshot()` 上，而不是 `reserve()` 上——等有第二个犯同样错的分配点再加。

## physboot 把它为内核建立的映射清单交给内核（2026-09-26）

**文件**
- `kernel/phys/include/ours/phys/handoff.hpp` — 新增 `MappingPerms`、`MemoryMapping`；`MemoryHandoff` 新增 `mappings`
- `kernel/phys/include/ours/phys/elf_image.hpp` — `fix_mapping()` 新增 `MappingRecorder` 回调
- `kernel/phys/elf_image_mapping.cpp` — `SegmentMapper::map_run()` 在映射成功之后回调
- `kernel/phys/physboot.cpp` — 新增 `MappingRecorder`（一页 `Handoff` 内存），填进 `mem.mappings`，并按 debug 级别打出来

**症状**
潜伏。physboot 建立内核地址空间——加载镜像、逐个 PT_LOAD 段映射、按 `PT_GNU_RELRO`
把段切开去掉写权限——然后跳进一个已经跑在这些映射上的内核。但它**什么都不告诉内核**。

内核那边 `kernel/mm/init_vmm.cpp` 的 `PHYS_VMOS` 表是照着自己的链接符号
（`KERNEL_CODE_START`、`KERNEL_RODATA_START`……）手写的，并且那里本来就挂着一条 TODO：

```
/// TODO(SmallHuaZi) These be provided by PhysBoot through HandOff rather than
/// direct to use linking symbols.
```

两份描述、描述的是同一件事、而写第二份的那一方没有参与那件事——这正是会漂移的东西的定义。
符号说的是「段被链接在哪」，映射说的是「physboot 实际建立了什么」，而没有任何东西校验过两者一致。

**原因**
真正只有 `fix_mapping()` 知道最终答案。它做两件内核无法重现的事：

1. KASLR。镜像落在 `KERNEL_LOAD_BASE + kaslr_offset`，偏移是随机的。
2. RELRO 切分。`PT_GNU_RELRO` 覆盖的页要从所属段的权限里去掉 Writable，
   而这个切分是在 `map_segment()` 的 run 循环里算的（`clip_relro()`）。
   让调用方再走一遍 load plan 去重算，等于写第二份实现，两份可以不一致。

所以清单由 `map_run()` 就地记录，而不是事后由谁重新推导。

**改法**
`fix_mapping(aspace, target_base, recorder, context)`。回调是**函数指针加 context**
而不是模板，因为 `fix_mapping()` 是 `PHYS_EXPORT`——physload_module 通过导出表按符号绑定它，
模板没有符号可绑。回调返回 false 会让整个映射失败：静默丢掉一条映射，等于交给内核一份
缺了它正跑在上面的某一段的清单。

记录发生在 `m_aspace.map()` 成功**之后**：硬件没拿到的 run 不是映射，不能告诉内核。

`MappingPerms`（Read/Write/Exec）而不是直接传 `arch::paging::MmuFlags`：后者是架构的页表词汇，
带 Present、Global、cacheability 之类，而跨越交接边界的值只应该携带接收方有权解释的东西。
内核会把它转成自己的 `VmaFlags`，它没有理由继承 phys 的 PTE 位。

`MemoryMapping` **不带名字**。内核的 VMA 需要名字，但命名是内核自己的词汇（`k:` 前缀），
而且可以由权限推出来；把字符串搬过去等于让 phys 成为内核诊断信息的作者。

记录数组放在**一页从 bootmem 要来的内存**里，类型 `MemoryType::Handoff`，而不是 physboot 的
`.bss` 里的 `StaticVec`：这个数组要活过跳转（内核的 `mh.mappings` 是指进它的 span），
而 physboot 自己的镜像是 `MemoryType::PhysImage`，那是内核可以随意重用的内存。
分配点放在 `snapshot_memory_map()` **之前**——bootmem 的任何分配都必须在快照之前发生，
否则内核永远不会知道那段被占了（见上一条「故意没做」里记的那条规则）。

**只记录镜像自己的段**。physmap 不记：它的地址和大小是架构常量
（`ARCH_PHYSMAP_VIRT_BASE`、`ARCH_PHYSMAP_SIZE`），内核编译进去的和 phys 读的是同一个头文件，
告诉它等于告诉它它已经知道的事。per-CPU 窗口更不记——那块根本不是 phys 填的，
`CpuLocal::Mapper` 在 NUMA 拓扑出现之后一个 slot 一个 slot 地填，而拓扑是 phys 没有的知识。

**参照**
Zircon 的 `PhysMapping` / `PhysVmar`（`kernel/phys/include/phys/handoff.h`），
`PhysHandoff::vmars` 的注释就是「Permanent VMARs to construct along with mapped regions within」。
一处不同：Zircon 的 `PhysMapping` 带 `name` 和 `Type{kNormal,kMmio}`，因为它的清单还要覆盖
MMIO 和交给用户态的东西；这里只有内核镜像，两者都还不需要。

**验证**
- `out/bringup.x86`、`out/numa.x86` 两个配置构建通过，两个镜像都启动到 shell 六组检查全过，
  numa 7 个 AP 全 online，无 fault / panic / 断言失败。
- 记录下来的 11 条映射（bringup，debug 级别）：

```
Kernel mapping [0xFFFFFF8840000000, 0xFFFFFF8840001000) <- 0x1FCFF000 r--
Kernel mapping [0xFFFFFF8840001000, 0xFFFFFF8840005000) <- 0x1FD00000 r--
Kernel mapping [0xFFFFFF8840005000, 0xFFFFFF884004C000) <- 0x1FD04000 r-x
Kernel mapping [0xFFFFFF884004C000, 0xFFFFFF884005E000) <- 0x1FD4B000 r--
Kernel mapping [0xFFFFFF884005E000, 0xFFFFFF8840062000) <- 0x1FD5D000 rw-
Kernel mapping [0xFFFFFF8840062000, 0xFFFFFF8840063000) <- 0x1FD61000 r--
Kernel mapping [0xFFFFFF8840063000, 0xFFFFFF88400C8000) <- 0x1FD62000 rw-
Kernel mapping [0xFFFFFF88400C8000, 0xFFFFFF88400CE000) <- 0x1FDC7000 r-x
Kernel mapping [0xFFFFFF88400CE000, 0xFFFFFF88400CF000) <- 0x1FDCD000 rw-
Kernel mapping [0xFFFFFF88400CF000, 0xFFFFFF88400E4000) <- 0x1FDCE000 rw-
Kernel mapping [0xFFFFFF88400E4000, 0xFFFFFF88400E5000) <- 0x1FDE3000 r--
```

逐条对得上 `llvm-readelf -l` 的 9 个 PT_LOAD 加 3 个 `GNU_RELRO`：

- `r-x` 两段 = `.text` 和 `.init.text`；
- 三条被 RELRO 切出来的只读页 = `.dynamic`、`.data.rel.ro`、`.ccrt_init`
  （第 1、6、11 条，各一页，正是三个 `GNU_RELRO` 的范围）；
- 地址连续无空洞，从 `IMAGE_START` 到镜像末尾，符合 `kernel.ld` 把每个权限边界都页对齐的设计。

这份清单比内核现在手写的 `PHYS_VMOS` **更细也更准**：那张表把 `.init*` 当成一整块 RWX，
而实际映射是 `.init.text` r-x、`.init.rodata` rw-、`.init.data` rw- 三段。

- `Reserved 2 frames (8 KiB) of handoff`：交接类型的帧从 1 变 2，正是新增的这一页。

**故意没做**
- 内核还没有读它。`init_vmm()` 的 `PHYS_VMOS` 仍然是照链接符号手写的那张表；
  换成读 `mem.mappings` 是下一条，单独验证——那一条会动内核镜像的保护，风险不在这里。
- `Handoff::verify()` 没有检查 `mappings` 非空。等内核真的依赖它了再加，
  现在加等于声称一个还没有人依赖的不变量。
- 没有把 physmap 和 per-CPU 窗口放进清单（见「改法」）。
