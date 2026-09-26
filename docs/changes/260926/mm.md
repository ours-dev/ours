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
