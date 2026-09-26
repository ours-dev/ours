# kernel/mm 改动记录（2026-09-26）

## 内核镜像的 VMA 改成读 physboot 的映射清单，不再照链接符号手写（2026-09-26）

**文件**
- `kernel/mm/init_vmm.cpp` — 删掉 `PHYS_VMOS` 里描述镜像的七条，剩下的两条改名 `PRESET_VMAS`；新增 `enrol_handoff_mappings()`、`name_of()`、`vma_flags_of()`

**症状**
潜伏。`PHYS_VMOS` 是一张照着内核自己的链接符号手写的表——`IMAGE_START`、
`KERNEL_CODE_START`、`KERNEL_RODATA_START`……——用来声明内核镜像占了哪些地址、
并把已有映射的权限改写成它说的那样（`vma->reserve()`）。原地就挂着一条 TODO：

```
/// TODO(SmallHuaZi) These be provided by PhysBoot through HandOff rather than
/// direct to use linking symbols.
```

两份描述，描述同一件事，而写第二份的一方没有参与那件事。符号说的是「段被链接在哪」，
映射说的是「physboot 实际建立了什么」，中间没有任何东西校验过两者一致。

具体差在哪，这次一比就看出来了：那张表把 `.init*` 当成**一整块 RWX**
（`k:init`，`Read | Write | Exec`），而 physboot 实际映射的是三段——
`.init.text` r-x、`.init.rodata` rw-、`.init.data` rw-。也就是说
`reserve()` 一直在把 `.init.rodata` 和 `.init.data` 改写成**可执行**的。

**原因**
最终答案只有 `fix_mapping()` 知道，两件事内核无法重现：

1. **KASLR**。镜像落在 `KERNEL_LOAD_BASE + kaslr_offset`，偏移是随机的。
   链接符号在重定位之后确实带着运行期地址，所以这一条勉强能对上——但那是巧合，
   不是设计。
2. **RELRO 切分**。`PT_GNU_RELRO` 覆盖的页要从所属段的权限里去掉 Writable，
   而这个切分是在 `SegmentMapper::map_segment()` 的 run 循环里算的。没有哪个链接符号
   描述切分之后的边界——`k:relro` 那一条是照 `KERNEL_RELRO_START/END` 写的，
   而实际有**三个** `GNU_RELRO` 区间（`.dynamic`、`.data.rel.ro`、`.ccrt_init`）。

上一条（`docs/changes/260926/phys.md`）让 physboot 把它建立的每一条映射记下来交过来；
这一条是读的那一半。

**改法**
`init_vmm_preheap()` 先跑 `enrol_handoff_mappings()`：遍历 `boot::handoff().mem.mappings`，
每条按权限推出名字和 `VmaFlags`，然后交给原来那个 `entrol_preset_vma()`，
`Backing::Mapped` 不变。

名字由权限推出（Exec → `k:code`，Write → `k:data`，否则 `k:rodata`），
而不是从 phys 那边搬字符串过来：`k:` 是内核自己的词汇，而且名字本来就跟着权限走。
这样也不再声称知道某一段装的是哪个 **section**——那张表是凭一个符号在声称这件事，
而它描述的那段映射早已被 RELRO 切成了没有任何 section 边界能解释的若干 run。

留在表里的只有两条，各自因为相反的理由 phys 不该描述：

- `k:physmap` —— phys 确实映射了它，但它的基址和大小是 `aspace_layout.hpp` 里的架构常量，
  内核编译进去的和 phys 读的是同一个头文件，告诉它等于告诉它它已经知道的事。
- `k:cpulocal` —— 根本不是 phys 填的。`CpuLocal::Mapper` 在 NUMA 拓扑出现之后一个 slot
  一个 slot 地填，而拓扑是 phys 没有的知识。（这一条是和用户确认过的：CpuLocal 不进清单。）

`ASSERT(!mappings.empty(), ...)`：清单空了意味着 physboot 没有交，
而此时内核已经跑在那些映射上——继续往下走只会在别处炸。

**参照**
Zircon 的 `PhysHandoff::vmars` 由 `HandoffEnd` 在启动末尾兑现成真的 VMAR，
是同一条路子。它那边 `PhysMapping` 带 `name`，因为它的清单还要覆盖 MMIO 和交给用户态的东西，
名字不能由权限推出；这里只有内核镜像，所以名字留在消费侧。

**验证**
- `out/bringup.x86`、`out/numa.x86` 两个配置构建通过。
- **这条改动自带一个很硬的测试**：`vma->reserve()` 会按 VMA 说的权限**改写已有页表项**。
  清单只要有一处不对——少一页、多一页、权限反了——内核会在 `init_vmm()` 之后立刻死：
  `.text` 丢掉执行权限就是取指故障，`.data` 变只读就是下一次写故障。两个镜像都跑到
  shell 六组检查全过、进程正常销毁，numa 7 个 AP 全 online。
- `kernel.dev.dbg_1.obi`（trace 级别）打出的内核 VMA 列表，正是 physboot 记录的那 11 条，
  加上两条常量：

```
A k:physmap : [0xFFFFFF8000000000, 0x800000000]
A k:cpulocal: [0xFFFFFF8800000000, 0x4000000]
A k:rodata  : [0xFFFFFF8840000000, 0x1000]     <- .dynamic，被 RELRO 去掉写权限
A k:rodata  : [0xFFFFFF8840001000, 0x5000]
A k:code    : [0xFFFFFF8840006000, 0x59000]    <- .text
A k:rodata  : [0xFFFFFF884005F000, 0x16000]    <- .rodata
A k:data    : [0xFFFFFF8840075000, 0x4000]     <- .data
A k:rodata  : [0xFFFFFF8840079000, 0x1000]     <- .data.rel.ro，RELRO
A k:data    : [0xFFFFFF884007A000, 0x65000]    <- .bss
A k:code    : [0xFFFFFF88400DF000, 0x9000]     <- .init.text
A k:data    : [0xFFFFFF88400E8000, 0x1000]     <- .init.rodata
A k:data    : [0xFFFFFF88400E9000, 0x15000]    <- .init.data
A k:rodata  : [0xFFFFFF88400FE000, 0x1000]     <- .ccrt_init，RELRO
```

地址连续无空洞，这正是原来那张表的注释要求而只能靠自觉保证的性质：镜像里有一页没被
任何 VMA 认领，VMM 就可以在镜像还占着它的时候把那个地址发给别人。

- dbg_1 那次启动里的 6 处 page fault 全部来自 `fault_probe` 那个故意越界的用户程序
  （`terminating process 'fault_probe'`），是预期行为，不是内核故障。

**故意没做**
- 没有顺手修 `.init.rodata` 段是 RW 而不是 R 这件事（`llvm-readelf -l` 里第 7 个 PT_LOAD）。
  那是 `kernel.ld` 的问题，改它要动链接脚本并重新验证整个镜像布局，是另一件事。
  现在的好处是：清单如实反映了它，而不是像原来那样把它连同 `.init.text` 一起说成 RWX。
- 没有动 `Handoff::verify()`。它检查的是 `mem.ranges` 非空；`mappings` 的检查放在
  `enrol_handoff_mappings()` 里，靠近真正依赖它的地方。
- 没有把 `k:physmap`/`k:cpulocal` 也变成某种「内核自己的清单」结构。两条常量用一张
  两行的表表达就够了，再抽象一层只会多一个要读的间接层。

## `INIT_CONST` 改名 `INIT_RODATA`，`const` 由人手写（2026-09-26）

**文件**
- `kernel/include/ours/init.hpp` — `INIT_CONST` → `INIT_RODATA`，`MEM_INIT_CONST` → `MEM_INIT_RODATA`
- `kernel/mm/init_vmm.cpp` — 唯一的使用点补上 `const`

**症状**
潜伏。`INIT_CONST` 的展开只是一个链接段属性：

```c
#define INIT_SECTION(name) LINK_SECTION(".init." name)
#define INIT_CONST         INIT_SECTION("rodata")
```

它不带 `const`。全树唯一的使用点是 `kernel/mm/init_vmm.cpp` 那张预设 VMA 表
（原 `PHYS_VMOS`，现 `PRESET_VMAS`），一直是个**可写数组**，名字是唯一暗示它只读的东西。

**原因**
这是从「`.init.rodata` 段为什么是 `WA` 而不是只读」查出来的。那个输出段有三个可写贡献者，
这张表是其中之一，也是唯一纯属漏掉的一个（另两个见下）。

宏改名而不是让宏发出 `const`，理由不是风格，是**宏做不到**：宏只能前置，而前置的 `const`
绑到被指物。对指针声明它产生的是

```c
const int *p    // 可变的指针，指向常量数据
```

而作者要的是

```c
int *const p    // 只读的指针
```

也就是说，这个宏恰好会在「constness 最要紧」的那些声明上静默地给出错的东西，
只在「手写 `const` 本来就不费事」的简单情形下是对的。所以关键字留在声明处，
由写它的人放到正确的那一侧。`INIT_CODE`/`INIT_DATA` 本来也只是段名，`INIT_RODATA`
和它们对齐。

**改法**
宏改名，使用点写成 `PhysVma const PRESET_VMAS[]`。`MEM_INIT_CONST` 一起改名
（它当前没有使用点，但留一个旧名字就是留一个下次被复制的坏名字）。

**参照**
无，本项目自有约定。同一条约定的前一次是 `FORCE_INLINE` 不许把 `inline` 打包进去
（见 `system/public/ours/compiler.hpp`）：属性宏只展开成属性。

**验证**
- `out/bringup.x86`、`out/numa.x86` 两个配置构建通过，两个镜像启动到 shell 全部检查通过，
  numa 7 个 AP 全 online，无 fault / panic / 断言失败。
- **段权限没有变，仍是 `WA`**，这一点要说清楚不要误以为修好了。查了 `init_vmm.cpp.o`：

```
[271] .init.rodata      PROGBITS  ...  WA
[272] .rela.init.rodata RELA      ...
      R_X86_64_64  .rodata.str1.1 + 741
      R_X86_64_64  .rodata.str1.1 + 74b
```

表里的 `char const *name` 需要重定位，而内核是 DYN 镜像，所以工具链必须把段标成可写。
`const` 加的是**编译期**保证（谁写它就编译不过），不是硬件保证。

**故意没做**
- 没有让 `.init.rodata` 真的只读。试过两条，这个 lld 都不行：输出段属性 `(READONLY)`
  报 `symbol not found: READONLY`；把输出段改名 `.data.rel.ro.init` 想蹭 RELRO 也没用
  （`.ccrt_init` 能进 RELRO 靠的是 `INIT_ARRAY` 段类型，不是名字）。
  剩下的办法是内核自己把 `[__init_rodata_start, __init_rodata_end)` 重新保护成只读——
  这里**不需要** RELRO，因为 `fix_mapping()` 的重定位走的是镜像物理页的恒等映射，
  最终映射从来不需要可写。这是单独一条，留在待修项里。
- 没有动 `.gktl.init_hook` 和 `.task.schedules` 这两个可写贡献者。它们的条目里有函数指针，
  同样是重定位的缘故，不是漏加 `const`（两者本来就是 `const`/`constexpr`）。

## 内核把 `.init.rodata` 的映射改成只读（2026-09-26）

**文件**
- `kernel/mm/include/ours/mm/vmm.hpp` — 新增 `KERNEL_INIT_RODATA_START/END` 两个链接符号声明
- `kernel/mm/init_vmm.cpp` — 新增 `tighten_init_rodata()`，enrol 每条 handoff 映射时去掉 Write
- `docs/repairs/260925/mm.md` — 删掉第 3 条

**症状**
潜伏。`.init.rodata` 这个段名承诺只读，而映射是可写的——physboot 按 PT_LOAD 的 flags 建映射，
而那个段在 ELF 里是 `WA`。内核的 init hook 表（`.gktl.init_hook`）、计数器描述符和
调度类注册表（`.task.schedules`）都在里面，运行期可写。没有人写它们，所以今天没有故障；
值在于把「只读」从命名约定变成硬件保证。

**原因**
段之所以是 `WA`，是因为里面的条目含指针（init hook 的函数指针、discipline 的 `&INSTANCE`），
内核是 DYN 镜像，这些是 `R_X86_64_64` 动态重定位，clang 无论对象多 `const` 都必须把段标可写。
标准的解法是 `PT_GNU_RELRO`，而 lld 只把它认得的段放进 RELRO——这正是 `kernel.ld` 第 98 行
那条注释说的，所以其它 RELRO 段全被并进了 `.data.rel.ro`。`.init.rodata` 并不进去，
因为它必须留在 `.init*` 尾巴里让 `reclaim_init_area()` 整段回收。

**但这里不需要 RELRO，这是整条改动的支点。** RELRO 存在的理由是动态链接器要
**通过它正在修补的那个映射**写重定位。physboot 不是这样：`ElfImage::fix_mapping()` 的契约
写得很清楚——`relocate()` 已经通过镜像物理页的**恒等映射**跑完了，内核映射是之后才建的。
所以没有任何人会通过这个地址写这些页，内核完全可以去掉 Write。而且「这个段本该只读」
这件事只有内核知道。

**改法**
不是事后 `protect()`，而是在 enrol 每条 handoff 映射时就把权限收紧：

```cpp
auto const perms = tighten_init_rodata(mapping.perms, mapping.base, mapping.size);
entrol_preset_vma({
    .name = name_of(perms),
    ...
    .vmaf = vma_flags_of(perms),
```

因为 `entrol_preset_vma()` 正是用这些 flags 去 `reserve()`（`extract_permissions(vmaf)`），
所以在这里收紧能让 **VMA 和硬件说同一句话**。事后再 `arch_aspace().protect()` 会留下
「VMA 说可写、页表说只读」的分裂，那是下一个人要查半天的东西。名字也跟着变成 `k:rodata`。

`ASSERT` 要求映射**整条**落在 `[__init_rodata_start, __init_rodata_end)` 内：两端都被
`kernel.ld` 的 `ALIGN(MAXPAGESIZE)` 对齐过，physboot 又只映射整页，所以部分重叠意味着这两条
前提之一不成立了——而「从半条映射里去掉 Write」不是这里能做的事。

**这不是内核在质疑 handoff 清单**：清单如实报告了 physboot 建立了什么，这是内核对**自己的
数据**做的决定，做在知识所在的地方。改动记录里写明这一点，否则下一个人会以为清单又被绕过了。

**参照**
无直接对应物。Zircon 的内核镜像保护走
`VmAspace::ProtectRegion` / `vm_init()` 里对 `__rodata_start` 一类符号的重新保护，
思路相同（内核按自己知道的语义收紧启动时建立的映射），但它没有「init 尾巴要整段回收」
这个额外约束，所以不需要在 RELRO 之外另找办法。

**验证**
- 两个配置构建通过，`out/bringup.x86`、`out/numa.x86` 都启动到 shell 全部检查通过，
  numa 7 个 AP 全 online，无 fault / panic / 断言失败。
- `kernel.dev.dbg_1.obi`（断言全开 + trace）也干净，而且那条 VMA 的名字从
  `k:data` 变成了 `k:rodata`：

```
A k:code    : [0xFFFFFF88400E0000, 0x9000]
A k:rodata  : [0xFFFFFF88400E9000, 0x1000]   <- .init.rodata，原来是 k:data
A k:data    : [0xFFFFFF88400EA000, 0x15000]
```

- **硬件层面直接验证**：在 QEMU 里用 QMP 读 `CR3`，顺着页表走到 physboot 那一条
  `.init.rodata` 映射的虚拟地址（同一次启动的 debug 日志里报的是
  `[0xFFFFFF88400D0000, 0xFFFFFF88400D1000) <- 0x1FDC8000 rw-`）：

```
CR3=0x1FEDB000
PML4[0x1FF] = 0x000000001FEBF027  P=1 RW=1
PDPT[0x21]  = 0x000000001FEBE027  P=1 RW=1
PD[0x0]     = 0x000000001FEBD027  P=1 RW=1
PT[0xD0]    = 0x800000001FDC8161  P=1 RW=0
```

叶级 PTE 是 `0x...161`——P、A、G、NX 都在，**RW 位是 0**。physboot 映射成 `rw-` 的那一页，
内核起来之后是只读的。

- `reclaim_init_area()` 之后这些帧会还给 buddy（`kernel/top/main.cpp:64`，在
  `init_platform()` 之后无条件跑），而内核虚拟映射仍在。两个镜像都跑过了这一步并继续启动
  userboot、跑完 shell，说明重用那些帧的人走 physmap 别名写，不受这个只读映射影响——
  这条原来在待修项里标着「要实测确认」，现在确认了。

**故意没做**
- 没有对 `.init.text` 做对应的事。它已经是 `r-x`，physboot 映射出来就是对的。
- 没有去动 `.init.rodata` 在 ELF 里仍是 `WA` 这件事。那要么需要 lld 支持
  `(READONLY)`/`INPUT_SECTION_FLAGS`（都不支持），要么需要把段挪出 `.init*` 尾巴
  （和整段回收冲突）。现在的状态是：ELF 说可写，映射是只读的，理由写在
  `tighten_init_rodata()` 的注释里。
- 没有给 `.init.rodata` 之外的段加同类收紧。清单里其它 `rw-` 的段（`.data`、`.bss`、
  `.init.data`）本来就该可写。
